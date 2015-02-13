/*
   Copyright (C) 2012 Harald Klein <hari@vt100.at>

   This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License.
   This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

   See the GNU General Public License for more details.

*/

#include <iostream>
#include <sstream>
#include <uuid/uuid.h>
#include <stdlib.h>

#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <boost/date_time/posix_time/posix_time_types.hpp>

#include <tinyxml2.h>

#include <eibclient.h>
#include "Telegram.h"

#include "agoapp.h"

using namespace qpid::messaging;
using namespace qpid::types;
using namespace tinyxml2;
using namespace std;
using namespace agocontrol;
namespace fs = ::boost::filesystem;
namespace pt = boost::posix_time;

class AgoKnx: public AgoApp {
private:
    int polldelay;

    Variant::Map deviceMap;

    std::string eibdurl;
    EIBConnection *eibcon;
    pthread_mutex_t mutexCon;
    boost::thread *listenerThread;

    qpid::types::Variant::Map commandHandler(qpid::types::Variant::Map content);
    void setupApp();
    void cleanupApp();

    bool loadDevices(fs::path &filename, Variant::Map& _deviceMap);
    void reportDevices(Variant::Map devicemap);
    string uuidFromGA(Variant::Map devicemap, string ga);
    string typeFromGA(Variant::Map device, string ga);

    void *listener();
public:
    AGOAPP_CONSTRUCTOR_HEAD(AgoKnx)
        , polldelay(0)
        {}
};

/**
 * parses the device XML file and creates a qpid::types::Variant::Map with the data
 */
bool AgoKnx::loadDevices(fs::path &filename, Variant::Map& _deviceMap) {
    XMLDocument devicesFile;
    int returncode;

    AGO_DEBUG() << "trying to open device file: " << filename;
    returncode = devicesFile.LoadFile(filename.c_str());
    if (returncode != XML_NO_ERROR) {
        AGO_ERROR() << "error loading XML file, code: " << returncode;
        return false;
    }

    AGO_TRACE() << "parsing file";
    XMLHandle docHandle(&devicesFile);
    XMLElement* device = docHandle.FirstChildElement( "devices" ).FirstChild().ToElement();
    if (device) {
        XMLElement *nextdevice = device;
        while (nextdevice != NULL) {
            Variant::Map content;

            AGO_TRACE() << "node: " << nextdevice->Attribute("uuid") << " type: " << nextdevice->Attribute("type");

            content["devicetype"] = nextdevice->Attribute("type");
            XMLElement *ga = nextdevice->FirstChildElement( "ga" );
            if (ga) {
                XMLElement *nextga = ga;
                while (nextga != NULL) {
                    AGO_DEBUG() << "GA: " << nextga->GetText() << " type: " << nextga->Attribute("type");
                    string type = nextga->Attribute("type");
/*
                    if (type=="onoffstatus" || type=="levelstatus") {
                        AGO_DEBUG() << "Requesting current status: " << nextga->GetText();
                        Telegram *tg = new Telegram();
                        eibaddr_t dest;
                        dest = Telegram::stringtogaddr(nextga->GetText());
                        tg->setGroupAddress(dest);
                        tg->setType(EIBREAD);
                        tg->sendTo(eibcon);
                    }
*/
                    content[nextga->Attribute("type")]=nextga->GetText();
                    nextga = nextga->NextSiblingElement();
                }
            }
            _deviceMap[nextdevice->Attribute("uuid")] = content;
            nextdevice = nextdevice->NextSiblingElement();
        }
    }
    return true;
}

/**
 * announces our devices in the devicemap to the resolver
 */
void AgoKnx::reportDevices(Variant::Map devicemap) {
    for (Variant::Map::const_iterator it = devicemap.begin(); it != devicemap.end(); ++it) {
        Variant::Map device;
        Variant::Map content;
        Message event;

        device = it->second.asMap();
        agoConnection->addDevice(it->first.c_str(), device["devicetype"].asString().c_str(), true);
    }
}

/**
 * looks up the uuid for a specific GA - this is needed to match incoming telegrams to the right device
 */
string AgoKnx::uuidFromGA(Variant::Map devicemap, string ga) {
    for (Variant::Map::const_iterator it = devicemap.begin(); it != devicemap.end(); ++it) {
        Variant::Map device;

        device = it->second.asMap();
        for (Variant::Map::const_iterator itd = device.begin(); itd != device.end(); itd++) {
            if (itd->second.asString() == ga) {
                // AGO_TRACE() << "GA " << itd->second.asString() << " belongs to " << itd->first;
                return(it->first);
            }
        }
    }	
    return("");
}

/**
 * looks up the type for a specific GA - this is needed to match incoming telegrams to the right event type
 */
string AgoKnx::typeFromGA(Variant::Map device, string ga) {
    for (Variant::Map::const_iterator itd = device.begin(); itd != device.end(); itd++) {
        if (itd->second.asString() == ga) {
            // AGO_TRACE() << "GA " << itd->second.asString() << " belongs to " << itd->first;
            return(itd->first);
        }
    }
    return("");
}
/**
 * thread to poll the knx bus for incoming telegrams
 */
void *AgoKnx::listener() {
    int received = 0;

    AGO_TRACE() << "starting listener thread";
    while(!isExitSignaled()) {
        string uuid;
        pthread_mutex_lock (&mutexCon);
        received=EIB_Poll_Complete(eibcon);
        pthread_mutex_unlock (&mutexCon);
        switch(received) {
            case(-1): 
                AGO_WARNING() << "cannot poll bus";
                try {
                    //boost::this_thread::sleep(pt::seconds(3)); FIXME: check why boost sleep interferes with EIB_Poll_complete, causing delays on status feedback
                    sleep(3);
                } catch(boost::thread_interrupted &e) {
                    AGO_DEBUG() << "listener thread cancelled";
                    break;
                }
                AGO_INFO() << "reconnecting to eibd"; 
                pthread_mutex_lock (&mutexCon);
                EIBClose(eibcon);
                eibcon = EIBSocketURL(eibdurl.c_str());
                if (!eibcon) {
                    pthread_mutex_unlock (&mutexCon);
                    AGO_FATAL() << "cannot reconnect to eibd";
                    signalExit();
                } else {
                    if (EIBOpen_GroupSocket (eibcon, 0) == -1) {
                        AGO_FATAL() << "cannot reconnect to eibd";
                        pthread_mutex_unlock (&mutexCon);
                        signalExit();
                    } else {
                        pthread_mutex_unlock (&mutexCon);
                        AGO_INFO() << "reconnect to eibd succeeded"; 
                    }
                }
                break;
                ;;
            case(0)	:
                try {
                    //boost::this_thread::sleep(pt::milliseconds(polldelay)); FIXME: check why boost sleep interferes with EIB_Poll_complete, causing delays on status feedback
                    usleep(polldelay);
                } catch(boost::thread_interrupted &e) {
                    AGO_DEBUG() << "listener thread cancelled";
                }
                break;
                ;;
            default:
                Telegram tl;
                pthread_mutex_lock (&mutexCon);
                tl.receivefrom(eibcon);
                pthread_mutex_unlock (&mutexCon);
                AGO_DEBUG() << "received telegram from: " << Telegram::paddrtostring(tl.getSrcAddress()) << " to: " 
                    << Telegram::gaddrtostring(tl.getGroupAddress()) << " type: " << tl.decodeType() << " shortdata: "
                    << tl.getShortUserData();
                uuid = uuidFromGA(deviceMap, Telegram::gaddrtostring(tl.getGroupAddress()));
                if (uuid != "") {
                    string type = typeFromGA(deviceMap[uuid].asMap(),Telegram::gaddrtostring(tl.getGroupAddress()));
                    if (type != "") {
                        AGO_DEBUG() << "handling telegram, GA from telegram belongs to: " << uuid << " - type: " << type;
                        if(type == "onoff" || type == "onoffstatus") { 
                            agoConnection->emitEvent(uuid.c_str(), "event.device.statechanged", tl.getShortUserData()==1 ? 255 : 0, "");
                        } else if (type == "setlevel" || type == "levelstatus") {
                            int data = tl.getUIntData(); 
                            agoConnection->emitEvent(uuid.c_str(), "event.device.statechanged", data, "");
                        } else if (type == "temperature") {
                            agoConnection->emitEvent(uuid.c_str(), "event.environment.temperaturechanged", tl.getFloatData(), "degC");
                        } else if (type == "brightness") {
                            agoConnection->emitEvent(uuid.c_str(), "event.environment.brightnesschanged", tl.getFloatData(), "lux");
                        } else if (type == "energy") {
                            agoConnection->emitEvent(uuid.c_str(), "event.environment.energychanged", tl.getFloatData(), "mA");
                        } else if (type == "energyusage") {
                            unsigned char buffer[4];
                            if (tl.getUserData(buffer,4) == 4) {
                                AGO_DEBUG() << "USER DATA: " << std::hex << buffer[0] << " " << buffer[1] << " " << buffer[2] << buffer[3];
                            }
                            // event.setSubject("event.environment.powerchanged");
                        } else if (type == "binary") {
                            agoConnection->emitEvent(uuid.c_str(), "event.security.sensortriggered", tl.getShortUserData()==1 ? 255 : 0, "");
                        }
                    }
                }
                break;
                ;;
        }

    }

    return NULL;
}

qpid::types::Variant::Map AgoKnx::commandHandler(qpid::types::Variant::Map content) {
    qpid::types::Variant::Map returnval;
    std::string internalid = content["internalid"].asString();
    AGO_TRACE() << "received command " << content["command"] << " for device " << internalid;
    qpid::types::Variant::Map::const_iterator it = deviceMap.find(internalid);
    qpid::types::Variant::Map device;
    if (it != deviceMap.end()) {
        device=it->second.asMap();
    } else {
        returnval["result"]=-1;
    }
    Telegram *tg = new Telegram();
    eibaddr_t dest;
    bool handled=true;
    if (content["command"] == "on") {
        string destGA = device["onoff"];
        dest = Telegram::stringtogaddr(destGA);
        if (device["devicetype"]=="drapes") {
            tg->setShortUserData(0);
        } else {
            tg->setShortUserData(1);
        }
    } else if (content["command"] == "off") {
        string destGA = device["onoff"];
        dest = Telegram::stringtogaddr(destGA);
        if (device["devicetype"]=="drapes") {
            tg->setShortUserData(1);
        } else {
            tg->setShortUserData(0);
        }
    } else if (content["command"] == "stop") {
        string destGA = device["stop"];
        dest = Telegram::stringtogaddr(destGA);
        tg->setShortUserData(1);
    } else if (content["command"] == "push") {
        string destGA = device["push"];
        dest = Telegram::stringtogaddr(destGA);
        tg->setShortUserData(0);
    } else if (content["command"] == "setlevel") {
        int level=0;
        string destGA = device["setlevel"];
        dest = Telegram::stringtogaddr(destGA);
        level = atoi(content["level"].asString().c_str());
        tg->setDataFromChar(level);
    } else if (content["command"] == "settemperature") {
        float temp = content["temperature"];
        string destGA = device["settemperature"];
        dest = Telegram::stringtogaddr(destGA);
        tg->setDataFromFloat(temp);
    } else if (content["command"] == "setcolor") {
        int level=0;
        Telegram *tg2 = new Telegram();
        Telegram *tg3 = new Telegram();
        tg->setDataFromChar(atoi(content["red"].asString().c_str()));
        dest = Telegram::stringtogaddr(device["red"].asString());
        tg2->setDataFromChar(atoi(content["green"].asString().c_str()));
        tg2->setGroupAddress(Telegram::stringtogaddr(device["green"].asString()));
        tg3->setDataFromChar(atoi(content["blue"].asString().c_str()));
        tg3->setGroupAddress(Telegram::stringtogaddr(device["blue"].asString()));
        pthread_mutex_lock (&mutexCon);
        AGO_TRACE() << "sending telegram";
        tg2->sendTo(eibcon);
        AGO_TRACE() << "sending telegram";
        tg3->sendTo(eibcon);
        pthread_mutex_unlock (&mutexCon);

    } else {
        handled=false;
    }
    if (handled) {	
        tg->setGroupAddress(dest);
        AGO_TRACE() << "sending telegram";
        pthread_mutex_lock (&mutexCon);
        bool result = tg->sendTo(eibcon);
        pthread_mutex_unlock (&mutexCon);
        AGO_DEBUG() << "Result: " << result;
        returnval["result"]=result ? 0 : -1;
    } else {
        AGO_ERROR() << "received undhandled command";
        returnval["result"]=-1;
    }
    return returnval;
}

void AgoKnx::setupApp() {
    fs::path devicesFile;

    // parse config
    eibdurl=getConfigOption("url", "ip:127.0.0.1");
    polldelay=atoi(getConfigOption("polldelay", "5000").c_str());
    devicesFile=getConfigOption("devicesfile", getConfigPath("/knx/devices.xml"));


    AGO_INFO() << "connecting to eibd"; 
    eibcon = EIBSocketURL(eibdurl.c_str());
    if (!eibcon) {
        AGO_FATAL() << "can't connect to eibd url:" << eibdurl;
        throw StartupError();
    }

    if (EIBOpen_GroupSocket (eibcon, 0) == -1)
    {
        EIBClose(eibcon);
        AGO_FATAL() << "can't open EIB Group Socket";
        throw StartupError();
    }

    addCommandHandler();

    // load xml file into map
    if (!loadDevices(devicesFile, deviceMap)) {
        AGO_FATAL() << "can't load device xml";
        throw StartupError();
    }
    // announce devices to resolver
    reportDevices(deviceMap);

    pthread_mutex_init(&mutexCon,NULL);

    AGO_DEBUG() << "Spawning thread for KNX listener";
    listenerThread = new boost::thread(boost::bind(&AgoKnx::listener, this));
}

void AgoKnx::cleanupApp() {
    AGO_TRACE() << "waiting for listener thread to stop";
    listenerThread->interrupt();
    listenerThread->join();
    AGO_DEBUG() << "closing eibd connection";
    EIBClose(eibcon);
}

AGOAPP_ENTRY_POINT(AgoKnx);

