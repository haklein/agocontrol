#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <boost/system/system_error.hpp> 
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <execinfo.h>
#include <signal.h>
#include <time.h>
#include <cstdlib>
#include <stdexcept>
#include "serialib.h"

#include "agoapp.h"
#include "MySensors.h"
#include "agoclient.h"

#ifndef DEVICEMAPFILE
#define DEVICEMAPFILE "/maps/mysensors.json"
#endif
#define DEFAULT_PROTOCOL "0.0"

using namespace agocontrol;
using namespace boost::system; 
namespace fs = ::boost::filesystem;

enum valid_type
{
    INVALID = 0,
    VALID_SAVE,
    VALID_DONT_SAVE,
    VALID_VAR1,
    VALID_VAR2,
    VALID_VAR3,
    VALID_VAR4,
    VALID_VAR5
};

enum flush_type
{
    flush_receive = TCIFLUSH,
    flush_send = TCIOFLUSH,
    flush_both = TCIOFLUSH
};

typedef struct S_COMMAND
{
    std::string command;
    int attempts;
} T_COMMAND;

static pthread_mutex_t serialMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t devicemapMutex = PTHREAD_MUTEX_INITIALIZER;

class AgoMySensors: public AgoApp
{
    private:
        //members
        boost::thread* readThread;
        boost::thread* checkStaleThread;
        std::string units;
        Json::Value devicemap;
        std::string gateway_protocol_version;
        serialib serialPort;
        std::string serialDevice;
        int staleThreshold;
        int bNetworkRelay;
        int bStale;
        Json::Value arduinoNodes;

        //functions
        void setupApp();
        void cleanupApp();
        std::string timestampToStr(const time_t* timestamp);
        void printDeviceInfos(std::string internalid, const Json::Value& infos);
        int getFreeId();
        void setDeviceInfos(std::string internalid, const Json::Value& infos);
        Json::Value getDeviceInfos(std::string internalid);
        std::string prettyPrint(std::string message, std::string protocol);
        bool deleteDevice(std::string internalid);
        void addDevice(std::string internalid, std::string devicetype, Json::Value& infos, std::string protocol);
        bool openSerialPort(std::string device);
        void closeSerialPort();
        bool checkInternalid(std::string internalid);
        void sendcommand(std::string command);
        void sendcommandV20(std::string internalid, int messageType, int ack, int subType, std::string payload);
        void sendcommandV15(std::string internalid, int messageType, int ack, int subType, std::string payload);
        void sendcommandV14(std::string internalid, int messageType, int ack, int subType, std::string payload);
        void sendcommandV13(std::string internalid, int messageType, int subType, std::string payload);
        Json::Value commandHandler(const Json::Value& content);
        std::string readLine(bool* error);
        void newDevice(std::string internalid, std::string devicetype, std::string protocol);
        void processMessageV20(int nodeId, int childId, int messageType, int ack, int subType, std::string payload, std::string internalid, Json::Value infos);
        void processMessageV15(int nodeId, int childId, int messageType, int ack, int subType, std::string payload, std::string internalid, Json::Value infos);
        void processMessageV14(int nodeId, int childId, int messageType, int ack, int subType, std::string payload, std::string internalid, Json::Value infos);
        void processMessageV13(int radioId, int childId, int messageType, int subType, std::string payload, std::string internalid, Json::Value infos);
        void receiveFunction();
        void checkStaleFunction();
        bool splitInternalLogMessage(std::string payload, Json::Value& items);

    public:
        AGOAPP_CONSTRUCTOR_HEAD(AgoMySensors),
            units("M"),
            devicemap(Json::objectValue),
            serialDevice(""),
            staleThreshold(86400),
            bNetworkRelay(false),
            bStale(true),
            arduinoNodes(Json::objectValue)
            {}
        
};

/**
 * Convert timestamp to Human Readable date time string (19 chars)
 */
std::string AgoMySensors::timestampToStr(const time_t* timestamp)
{
    char hr[512] = "";
    if( (*timestamp)>0 )
    {
        struct tm* sTm = gmtime(timestamp);
        snprintf(hr, 512, "%02d:%02d:%02d %04d/%02d/%02d", sTm->tm_hour, sTm->tm_min, sTm->tm_sec, sTm->tm_year+1900, sTm->tm_mon+1, sTm->tm_mday);
    }
    else
    {
        snprintf(hr, 512, "?");
    }

    return std::string(hr);
}

/**
 * Make readable device infos
 */
void AgoMySensors::printDeviceInfos(std::string internalid, const Json::Value& infos)
{
    std::stringstream result;
    result << "Infos of device internalid '" << internalid << "'";
    if( infos.isMember("protocol") )
        result << " - protocol=" << infos["protocol"];
    if( infos.isMember("type") )
        result << " - type=" << infos["type"];
    if( infos.isMember("value") )
        result << " - value=" << infos["value"];
    if( !infos.isMember("counter_sent") )
        result << " - counter_sent=" << infos["counter_sent"];
    if( !infos.isMember("counter_received") )
        result << " - counter_received=" << infos["counter_received"];
    if( infos.isMember("counter_failed") )
        result << " - counter_failed=" << infos["counter_failed"];
    if( infos.isMember("last_timestamp") )
        result << " - last_timestamp=" << infos["last_timestamp"];
    AGO_TRACE() << result.str();
}

/**
 * Return free id according to current known valid sensors
 * @return free id or 0 if nothing found
 */
int AgoMySensors::getFreeId()
{
    int freeId = 0;
    Json::Value existingIds(Json::arrayValue);

    pthread_mutex_lock(&devicemapMutex);
    if( devicemap.isMember("devices") )
    {
        //get list of existing ids
        Json::Value devices = devicemap["devices"];
        for( auto it = devices.begin(); it != devices.end(); it++ )
        {
            std::string internalid = it.name();
            //format <int>/<int>
            std::vector<std::string> splits;
            boost::split(splits, internalid, boost::is_any_of("/"));
            if( splits.size()==2 && splits[0].length()>0 && splits[1].length()>0 )
            {
                int nodeId = atoi(splits[0].c_str());
                existingIds.append(nodeId);
            }
            else
            {
                //invalid internalid
            }
        }
        AGO_TRACE() << "Existing ids list: " << existingIds;

        //search free id
        bool found = false;
        for( int i=1; i<255; i++ )
        {
            found = false;
            for( Json::Value::iterator it=existingIds.begin(); it!=existingIds.end(); it++ )
            {
                if( it->asInt()==i )
                {
                    found = true;
                    break;
                }
            }

            if( !found )
            {
                freeId = i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&devicemapMutex);

    //no id found
    AGO_DEBUG() << "Free id found: " << freeId;
    return freeId;
}

/**
 * Save specified device infos
 */
void AgoMySensors::setDeviceInfos(std::string internalid, const Json::Value& infos)
{
    pthread_mutex_lock(&devicemapMutex);
    if( !infos.isNull() && !infos.empty() )
    {
        if( devicemap.isMember("devices") )
        {
            AGO_TRACE() << "Device [" << internalid << "] infos updated";
            Json::Value& devices = devicemap["devices"];
            devices[internalid] = infos;

            writeJsonFile(devicemap, getConfigPath(DEVICEMAPFILE));
        }
    }
    else
    {
        AGO_TRACE() << "Device [" << internalid << "] not updated because devices specified infos is empty";
    }
    pthread_mutex_unlock(&devicemapMutex);
}

/**
 * Get infos of specified device
 */
Json::Value AgoMySensors::getDeviceInfos(std::string internalid)
{
    Json::Value out;

    if( devicemap.isMember("devices") )
    {
        Json::Value& devices = devicemap["devices"];

        if( devices.isMember(internalid) )
        {
            out = devices[internalid];

            //check field existence
            if( !out.isMember("protocol") )
            {
                out["protocol"] = DEFAULT_PROTOCOL;
                setDeviceInfos(internalid, out);
            }
        }
    }

    return out;
}

/**
 * Make readable received message from MySensor gateway
 */
std::string AgoMySensors::prettyPrint(std::string message, std::string protocol)
{
    std::string payload = "";
    std::string ack = "";
    std::stringstream result;
    std::string internalid = "";
    Json::Value infos;

    std::vector<std::string> items = split(message, ';');
    if ( items.size()<4 || items.size()>6 )
    {
        result.str("");
        result << "ERROR, malformed string: " << message;
    }
    else
    {
        //internalid
        internalid = items[0] + "/" + items[1];
        result << internalid << ";";
        if( boost::algorithm::starts_with(protocol, "2.0") )
        {
            //protocol v2.0.X

            //message type
            result << getMsgTypeNameV20((msgTypeV20)atoi(items[2].c_str())) << ";";
            //ack
            result << items[3] << ";";
            //message subtype
            switch (atoi(items[2].c_str())) {
                case PRESENTATION_V20:
                    //device type
                    result << getDeviceTypeNameV20((deviceTypesV20)atoi(items[4].c_str())) << ";";
                    //protocol version (payload)
                    if( items.size()==6 )
                        result << items[5];
                    break;
                case SET_V20:
                case REQ_V20:
                    //variable type
                    result << getVariableTypeNameV20((varTypesV20)atoi(items[4].c_str())) << ";";
                    //value (payload)
                    if( items.size()==6 )
                        result << items[5];
                    break;
                case INTERNAL_V20:
                    //internal message type
                    if( atoi(items[4].c_str())==I_LOG_MESSAGE_V20 && !logLevel==log::trace )
                    {
                        //filter gateway log message
                        result.str("");
                    }
                    else
                    {
                        result << getInternalTypeNameV20((internalTypesV20)atoi(items[4].c_str())) << ";";
                        //value (payload)
                        if( items.size()==6 )
                            result << items[5];
                    }
                    break;
                case STREAM_V20:
                    //stream message
                    //TODO when fully implemented in MySensors
                    result << "STREAM (not implemented!)";
                    break;
                default:
                    result << items[3];
            }
        }
        else if( boost::algorithm::starts_with(protocol, "1.5") )
        {
            //protocol v1.5

            //message type
            result << getMsgTypeNameV15((msgTypeV15)atoi(items[2].c_str())) << ";";
            //ack
            result << items[3] << ";";
            //message subtype
            switch (atoi(items[2].c_str())) {
                case PRESENTATION_V15:
                    //device type
                    result << getDeviceTypeNameV15((deviceTypesV15)atoi(items[4].c_str())) << ";";
                    //protocol version (payload)
                    if( items.size()==6 )
                        result << items[5];
                    break;
                case SET_V15:
                case REQ_V15:
                    //variable type
                    result << getVariableTypeNameV15((varTypesV15)atoi(items[4].c_str())) << ";";
                    //value (payload)
                    if( items.size()==6 )
                        result << items[5];
                    break;
                case INTERNAL_V15:
                    //internal message type
                    if( atoi(items[4].c_str())==I_LOG_MESSAGE_V15 && !logLevel==log::trace )
                    {
                        //filter gateway log message
                        result.str("");
                    }
                    else
                    {
                        result << getInternalTypeNameV15((internalTypesV15)atoi(items[4].c_str())) << ";";
                        //value (payload)
                        if( items.size()==6 )
                            result << items[5];
                    }
                    break;
                case STREAM_V15:
                    //stream message
                    //TODO when fully implemented in MySensors
                    result << "STREAM (not implemented!)";
                    break;
                default:
                    result << items[3];
            }
        }
        else if( boost::algorithm::starts_with(protocol, "1.4") )
        {
            //protocol v1.4

            //message type
            result << getMsgTypeNameV14((msgTypeV14)atoi(items[2].c_str())) << ";";
            //ack
            result << items[3] << ";";
            //message subtype
            switch (atoi(items[2].c_str())) {
                case PRESENTATION_V14:
                    //device type
                    result << getDeviceTypeNameV14((deviceTypesV14)atoi(items[4].c_str())) << ";";
                    //protocol version (payload)
                    if( items.size()==6 )
                        result << items[5];
                    break;
                case SET_V14:
                case REQ_V14:
                    //variable type
                    result << getVariableTypeNameV14((varTypesV14)atoi(items[4].c_str())) << ";";
                    //value (payload)
                    if( items.size()==6 )
                        result << items[5];
                    break;
                case INTERNAL_V14:
                    //internal message type
                    if( atoi(items[4].c_str())==I_LOG_MESSAGE_V14 && !logLevel==log::trace )
                    {
                        //filter gateway log message
                        result.str("");
                    }
                    else
                    {
                        result << getInternalTypeNameV14((internalTypesV14)atoi(items[4].c_str())) << ";";
                        //value (payload)
                        if( items.size()==6 )
                            result << items[5];
                    }
                    break;
                case STREAM_V14:
                    //stream message
                    //TODO when fully implemented in MySensors
                    result << "STREAM (not implemented!)";
                    break;
                default:
                    result << items[3];
            }
        }
        else if( boost::algorithm::starts_with(protocol, "1.3") )
        {
            //protocol v1.3

            if (items.size() == 5)
                payload=items[4];
            result << items[0] << "/" << items[1] << ";" << getMsgTypeNameV13((msgTypeV13)atoi(items[2].c_str())) << ";";
            switch (atoi(items[2].c_str())) {
                case PRESENTATION_V13:
                    result << getDeviceTypeNameV13((deviceTypesV13)atoi(items[3].c_str()));
                    break;
                case SET_VARIABLE_V13:
                    result << getVariableTypeNameV13((varTypesV13)atoi(items[3].c_str()));
                    break;
                case REQUEST_VARIABLE_V13:
                    result << getVariableTypeNameV13((varTypesV13)atoi(items[3].c_str()));
                    break;
                case VARIABLE_ACK_V13:
                    result << getVariableTypeNameV13((varTypesV13)atoi(items[3].c_str()));
                    break;
                case INTERNAL_V13:
                    result << getInternalTypeNameV13((internalTypesV13)atoi(items[3].c_str()));
                    break;
                default:
                    result << items[3];
            }
            result <<  ";" << payload;
        }
        else
        {
            result.str("");
            result << "ERROR, unsupported protocol version '" << protocol << "'";
        }
    }
    return result.str();
}

/**
 * Delete device and all associated infos
 */
bool AgoMySensors::deleteDevice(std::string internalid)
{
    //init
    bool result = false;

    if( devicemap.isMember("devices") )
    {
        Json::Value& devices = devicemap["devices"];

        //check if device exists
        if( devices.isMember(internalid) )
        {
            //remove device from uuidmap
            if( agoConnection->removeDevice(internalid) )
            {
                //clear all infos
                pthread_mutex_lock(&devicemapMutex);
                devices.removeMember(internalid);
                writeJsonFile(devicemap, getConfigPath(DEVICEMAPFILE));
                pthread_mutex_unlock(&devicemapMutex);

                result = true;
                AGO_INFO() << "Device \"" << internalid << "\" removed successfully";
            }
            else
            {
                //unable to remove device
                result = false;
                AGO_ERROR() << "Unable to remove device \"" << internalid << "\"";
            }
        }
        else
        {
            //device not found
            result = false;
            AGO_ERROR() << "Unable to remove unknown device \"" << internalid << "\"";
        }
    }

    return result;
}

/**
 * Save all necessary infos for new device and register it to agocontrol
 */
void AgoMySensors::addDevice(std::string internalid, std::string devicetype, Json::Value& infos, std::string protocol)
{
    pthread_mutex_lock(&devicemapMutex);
    std::string addStatus = "added";
    if( infos.size()>0 )
    {
        addStatus = "updated";
    }

    infos["type"] = devicetype;
    infos["value"] = "0";
    infos["counter_sent"] = 0;
    infos["counter_received"] = 0;
    infos["counter_failed"] = 0;
    infos["last_route"] = "";
    infos["last_timestamp"] = (int)(time(NULL));
    infos["protocol"] = protocol;

    Json::Value& devices = devicemap["devices"];
    devices[internalid] = infos;

    writeJsonFile(devicemap, getConfigPath(DEVICEMAPFILE));

    agoConnection->addDevice(internalid, devicetype);
    AGO_TRACE() << "Device [" << internalid << "] " << addStatus;
    pthread_mutex_unlock(&devicemapMutex);
}

/**
 * Split I_LOG_MESSAGE payload
 */
bool AgoMySensors::splitInternalLogMessage(std::string payload, Json::Value& items)
{
    bool result = true;

    if( boost::algorithm::starts_with(payload, "send: ") )
    {
        //split send message
        //format: send: <sender>-<last>-<to>-<destination> s=<sensor>,c=<command>,t=<type>,pt=<payload type>,l=<length>,sg=<signed>,st=<status(fail|ok)>:<payload>
        items["type"] = "send";
        std::vector<std::string> allItems = split(payload, ':');
        if( allItems.size()>=3 )
        {
            std::string temp = allItems[1];
            boost::algorithm::trim(temp);
            std::vector<std::string> routeItems = split(temp, '-');
            if( routeItems.size()==4 )
            {
                items["sender"] = routeItems[0];
                items["last"] = routeItems[1];
                items["to"] = routeItems[2];
                items["destination"] = routeItems[3];
            }
            else
            {
                AGO_TRACE() << "I_LOG_MESSAGE send payload: route string is not valid [" << allItems[1] << "]";
                result = false;
            }

            std::vector<std::string> infosItems = split(allItems[2], ',');
            if( infosItems.size()==7 )
            {
                //destination child is item #0 (s=<sensor>)
                std::string temp = infosItems[0];
                boost::replace_all(temp, "s=", "");
                items["sensor"] = temp;
                temp = infosItems[1];
                boost::replace_all(temp, "c=", "");
                items["command"] = temp;
                temp = infosItems[2];
                boost::replace_all(temp, "t=", "");
                items["commandtype"] = temp;
                temp = infosItems[3];
                boost::replace_all(temp, "pt=", "");
                items["payloadtype"] = temp;
                temp = infosItems[4];
                boost::replace_all(temp, "l=", "");
                items["length"] = temp;
                temp = infosItems[5];
                boost::replace_all(temp, "sg=", "");
                items["signed"] = temp;
                temp = infosItems[6];
                std::vector<std::string> temps = split(temp, ':');
                if( temps.size()==2 )
                {
                    temp = temps[0];
                    boost::replace_all(temp, "st=", "");
                    items["status"] = temp;
                }
                else
                {
                    AGO_DEBUG() << "I_LOG_MESSAGE send payload: no payload in message? [" << temp << "]";
                    result = false;
                }
            }
            else
            {
                AGO_DEBUG() << "I_LOG_MESSAGE send payload: infos string is not valid [" << allItems[2] << "]";
                result = false;
            }
        }
        else
        {
            AGO_DEBUG() << "I_LOG_MESSAGE send payload is not valid";
            result = false;
        }
    }
    else if( boost::algorithm::starts_with(payload, "read: ") )
    {
        //split read message
        //format: read: <sender>-<last>-<destination> s=<sensor>,c=<command>,t=<type>,pt=<payload type>,l=<length>,sg=<signed>:<payload>
        items["type"] = "read";
        std::vector<std::string> allItems = split(payload, ':');
        //allItems[0] = "read"
        //allItems[1] = " <sender>-<last>-<destination> s=<sensor>,c=<command>,t=<type>,pt=<payload type>,l=<length>,sg=<signed>"
        //allItems[2] = "<payload>"
        if( allItems.size()>=3 )
        {
            //prepare data
            std::string temp = allItems[1];
            //remove first space character before route info
            boost::algorithm::trim(temp);
            std::vector<std::string> subItems = split(temp, ' ');
            //subItems[0] = "<sender>-<last>-<destination>"
            //subItems[1] = "s=<sensor>,c=<command>,t=<type>,pt=<payload type>,l=<length>,sg=<signed>"
            
            //get route
            std::vector<std::string> routeItems = split(subItems[0], '-');
            if( routeItems.size()==3 )
            {
                items["sender"] = routeItems[0];
                items["last"] = routeItems[1];
                items["destination"] = routeItems[2];
            }
            else
            {
                AGO_DEBUG() << "I_LOG_MESSAGE read payload: route string is not valid [" << allItems[1] << "]";
                result = false;
            }

            //get log infos
            std::vector<std::string> infosItems = split(subItems[1], ',');
            if( infosItems.size()==6 )
            {
                //s=<sensor>
                std::string temp = infosItems[0];
                boost::replace_all(temp, "s=", "");
                items["sensor"] = temp;

                //c=<command>
                temp = infosItems[1];
                boost::replace_all(temp, "c=", "");
                items["command"] = temp;

                //t=<type>
                temp = infosItems[2];
                boost::replace_all(temp, "t=", "");
                items["commandtype"] = temp;

                //pt=<payload type>
                temp = infosItems[3];
                boost::replace_all(temp, "pt=", "");
                items["payloadtype"] = temp;

                //l=<length>
                temp = infosItems[4];
                boost::replace_all(temp, "l=", "");
                items["length"] = temp;

                //above v1.5 signed info exists
                temp = infosItems[5];
                if( temp.length()>0 )
                {
                    //sg=<signed>
                    boost::replace_all(temp, "sg=", "");
                    items["signed"] = temp;
                }
            }
            else
            {
                AGO_DEBUG() << "I_LOG_MESSAGE read payload: infos string is not valid [" << allItems[2] << "]";
                result = false;
            }
        }
        else
        {
            AGO_DEBUG() << "I_LOG_MESSAGE read payload is not valid";
            result = false;
        }
    }
    else
    {
        AGO_DEBUG() << "I_LOG_MESSAGE is neither send nor read message";
        result = false;
    }

    AGO_TRACE() << "splitInternalLogMessage found items: " << items;

    return result;
}

/**
 * Open serial port
 */
bool AgoMySensors::openSerialPort(std::string device)
{
    bool result = true;
    try
    {
        int res = serialPort.Open(device.c_str(), 115200);
        if( res!=1 )
        {
            AGO_ERROR() << "Can't open serial port: " << res;
            result = false;
        }
        else
        {
            //reset arduino
            serialPort.EnableDTR(false);
            serialPort.FlushReceiver();
            sleep(1);
            serialPort.EnableDTR(true);
            sleep(1);
        }
    }
    catch(std::exception const&  ex)
    {
        AGO_ERROR()  << "Can't open serial port: " << ex.what();
        result = false;
    }
    return result;
}

/**
 * Close serial port
 */
void AgoMySensors::closeSerialPort()
{
    try
    {
        serialPort.Close();
    }
    catch( std::exception const&  ex)
    {
        AGO_DEBUG() << "Can't close serial port: " << ex.what();
    }
}

/**
 * Check internalid
 */
bool AgoMySensors::checkInternalid(std::string internalid)
{
    bool result = false;
    if( internalid.length()>0 )
    {
        //format <int>/<int>
        std::vector<std::string> splits;
        boost::split(splits, internalid, boost::is_any_of("/"));
        if( splits.size()==2 && splits[0].length()>0 && splits[1].length()>0 )
        {
            int nodeId = atoi(splits[0].c_str());
            int childId = atoi(splits[1].c_str());
            if( nodeId>=0 && nodeId<=255 && childId>=0 && childId<=255 )
            {
                //specified internalid is valid
                result = true;
            }
            else
            {
                //seems to be invalid
                result = false;
            }
        }
        else
        {
            //seems to be invalid
            result = false;
        }
    }
    else
    {  
        //invalid input internalid
        result = false;
    }
    return result;
}

/**
 * Send command to MySensor gateway
 */
void AgoMySensors::sendcommand(std::string command)
{
    std::string logCommand(command);
    boost::replace_all(logCommand, "\n", "<NL>");
    boost::replace_all(logCommand, "\r", "<CR>");
    AGO_DEBUG() << " => RE-SENDING: " << logCommand;
    serialPort.WriteString(command.c_str());
}

void AgoMySensors::sendcommandV20(std::string internalid, int messageType, int ack, int subType, std::string payload)
{
    std::vector<std::string> items = split(internalid, '/');
    std::stringstream command;
    Json::Value infos = getDeviceInfos(internalid);

    //prepare command
    int nodeId = atoi(items[0].c_str());
    int childId = atoi(items[1].c_str());
    command << nodeId << ";" << childId << ";" << messageType << ";" << ack << ";" << subType << ";" << payload << "\n";

    //send command
    std::string logCommand(command.str());
    boost::replace_all(logCommand, "\n", "<NL>");
    boost::replace_all(logCommand, "\r", "<CR>");
    AGO_DEBUG() << " => SENDINGv20: " << logCommand;
    serialPort.WriteString(command.str().c_str());
}

void AgoMySensors::sendcommandV15(std::string internalid, int messageType, int ack, int subType, std::string payload)
{
    std::vector<std::string> items = split(internalid, '/');
    std::stringstream command;
    Json::Value infos = getDeviceInfos(internalid);

    //prepare command
    int nodeId = atoi(items[0].c_str());
    int childId = atoi(items[1].c_str());
    command << nodeId << ";" << childId << ";" << messageType << ";" << ack << ";" << subType << ";" << payload << "\n";

    //send command
    std::string logCommand(command.str());
    boost::replace_all(logCommand, "\n", "<NL>");
    boost::replace_all(logCommand, "\r", "<CR>");
    AGO_DEBUG() << " => SENDINGv15: " << logCommand;
    serialPort.WriteString(command.str().c_str());
}

void AgoMySensors::sendcommandV14(std::string internalid, int messageType, int ack, int subType, std::string payload)
{
    std::vector<std::string> items = split(internalid, '/');
    std::stringstream command;
    Json::Value infos = getDeviceInfos(internalid);

    //prepare command
    int nodeId = atoi(items[0].c_str());
    int childId = atoi(items[1].c_str());
    command << nodeId << ";" << childId << ";" << messageType << ";" << ack << ";" << subType << ";" << payload << "\n";

    //send command
    std::string logCommand(command.str());
    boost::replace_all(logCommand, "\n", "<NL>");
    boost::replace_all(logCommand, "\r", "<CR>");
    AGO_DEBUG() << " => SENDINGv14: " << logCommand;
    serialPort.WriteString(command.str().c_str());
}

void AgoMySensors::sendcommandV13(std::string internalid, int messageType, int subType, std::string payload)
{
    std::vector<std::string> items = split(internalid, '/');
    std::stringstream command;
    Json::Value infos = getDeviceInfos(internalid);

    //prepare command
    int nodeId = atoi(items[0].c_str());
    int childId = atoi(items[1].c_str());
    command << nodeId << ";" << childId << ";" << messageType << ";" << subType << ";" << payload << "\n";

    //send command
    std::string logCommand(command.str());
    boost::replace_all(logCommand, "\n", "<NL>");
    boost::replace_all(logCommand, "\r", "<CR>");
    AGO_DEBUG() << " => SENDINGv13: " << logCommand;
    serialPort.WriteString(command.str().c_str());
}

/**
 * Agocontrol command handler
 */
Json::Value AgoMySensors::commandHandler(const Json::Value& content)
{
    std::string errorMessage = "";

    Json::Value returnval;
    Json::Value infos;
    std::string deviceType;
    std::string cmd;

    AGO_TRACE() << "CommandHandler" << content;
    checkMsgParameter(content, "command", Json::stringValue);

    //get values
    cmd = content["command"].asString();

    //switch to specified command
    if( cmd=="getcounters" )
    {
        //return devices counters
        Json::Value counters(Json::objectValue);
        if( devicemap.isMember("devices") )
        {
            const Json::Value& devices = devicemap["devices"];
            for (auto it = devices.begin(); it != devices.end(); it++)
            {
                Json::Value content = *it;
                //add device name + device type
                std::stringstream deviceNameType;
                deviceNameType << it.name().c_str() << " (";
                infos = getDeviceInfos(it.name());
                if( infos.size()>0 && infos.isMember("type") )
                {
                    deviceNameType << infos["type"];
                }
                else
                {
                    deviceNameType << "unknown";
                }
                deviceNameType << ")";
                content["device"] = deviceNameType.str();
                //add device last datetime
                if( content.isMember("last_timestamp") )
                {
                    int64_t timestamp = content["last_timestamp"].asInt64();
                    content["datetime"] = timestampToStr((time_t*)&timestamp);
                }
                else
                {
                    content["datetime"] = "?";
                }
                counters[it.name()] = content;
            }
        }
        returnval["counters"] = counters;
    }
    else if( cmd=="resetallcounters" )
    {
        //reset all counters
        if( devicemap.isMember("devices") )
        {
            const Json::Value& devices = devicemap["devices"];
            for (auto it = devices.begin(); it != devices.end(); it++)
            {
                infos = getDeviceInfos(it.name());
                if( infos.size()>0 )
                {
                    infos["counter_received"] = 0;
                    infos["counter_sent"] = 0;
                    infos["counter_failed"] = 0;
                    setDeviceInfos(it.name(), infos);
                }
            }
        }
    }
    else if( cmd=="resetcounters" )
    {
        //reset counters of specified sensor
        //command["device"] format: {internalid:<>, type:<>}
        if( content.isMember("device") )
        {
            Json::Value device = content["device"];
            if( device.isMember("internalid") )
            {
                infos = getDeviceInfos(device["internalid"].asString());
                if( infos.size()>0 )
                {
                    infos["counter_received"] = 0;
                    infos["counter_sent"] = 0;
                    infos["counter_failed"] = 0;
                    setDeviceInfos(device["internalid"].asString(), infos);
                }
            }
        }
        else
        {
            //invalid command format
            errorMessage = "Invalid command received";
        }
    }
    else if( cmd=="getport" )
    {
        //get serial port
        returnval["error"] = 0;
        returnval["msg"] = "";
        returnval["port"] = serialDevice;
    }
    else if( cmd=="setport" )
    {
        //set serial port
        if( content.isMember("port") ) {
            //restart communication
            closeSerialPort();
            if( !openSerialPort(content["port"].asString()) ) {
                errorMessage = "Unable to open specified port";
            }
            else {
                //everything looks good, save port
                serialDevice = content["port"].asString();
                if( !setConfigSectionOption("mysensors", "device", serialDevice.c_str()) ) {
                    errorMessage = "Unable to save serial port to config file";
                }
            }
        }
        else {
            //port is missing
            errorMessage = "No port specified";
        }
    }
    else if( cmd=="getdevices" )
    {
        //return list of devices (with device type too!!)
        returnval["error"] = 0;
        returnval["msg"] = "";
        Json::Value devicesList(Json::arrayValue);
        if( devicemap.isMember("devices") )
        {
            Json::Value devices = devicemap["devices"];
            for (auto it = devices.begin(); it != devices.end(); it++)
            {
                infos = getDeviceInfos(it.name());
                Json::Value item;
                item["internalid"] = it.name().c_str();
                if( infos.size()>0 && infos.isMember("type") )
                {
                    item["type"] = infos["type"];
                }
                else
                {
                    item["type"] = "unknown";
                }
                devicesList.append(item);
            }
        }
        returnval["devices"] = devicesList;
    }
    else if( cmd=="remove" )
    {
        //remove specified device
        if( content.isMember("device") )
        {
            //command["device"] format: {internalid:<>, type:<>}
            Json::Value device = content["device"];
            if( device.isMember("internalid") )
            {
                if( deleteDevice(device["internalid"].asString()) )
                {
                    errorMessage = "";
                }
                else {
                    errorMessage = "Unable to delete sensor";
                }
            }
            else
            {
                //invalid command format
                errorMessage = "Invalid command received";
            }
        }
        else
        {
            //device id is missing
            errorMessage = "Device is missing";
        }
    }
    else if( cmd=="setcustomvariable" )
    {
        checkMsgParameter(content, "internalid", Json::stringValue);
        checkMsgParameter(content, "device", Json::stringValue);
        checkMsgParameter(content, "variable", Json::stringValue);
        checkMsgParameter(content, "value", Json::stringValue);

        std::string internalid = content["internalid"].asString();
        std::string devInternalid = content["device"].asString();
        std::string customvar = content["variable"].asString();
        std::string value = content["value"].asString();

        //check device
        Json::Value infos = getDeviceInfos(devInternalid);
        if( infos.size()==0 )
        {
            //specified device is surely not handled by mysensors
            errorMessage = "Device not handled by this controller";
        }
        else
        {
            if( customvar=="VAR1" )
            {
                //reserved customvar
                AGO_WARNING() << "Reserved customvar '" << customvar << "'. Nothing processed";
                errorMessage = "Reserved customvar";
            }
            else if( customvar=="VAR2" )
            {
                if( boost::algorithm::starts_with(infos["protocol"].asString(), "2.0") )
                {
                    sendcommandV20(internalid, SET_V20, 1, V_VAR2_V20, value);
                }
                else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.5") )
                {
                    sendcommandV15(internalid, SET_V15, 1, V_VAR2_V15, value);
                }
                else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.4") )
                {
                    sendcommandV14(internalid, SET_V14, 1, V_VAR2_V14, value);
                }
                else
                {
                    AGO_WARNING() << "Customvar is supported from protocol v1.4";
                    errorMessage = "Customvar is supported from protocol v1.4";
                }
            }
            else if( customvar=="VAR3" )
            {
                if( boost::algorithm::starts_with(infos["protocol"].asString(), "2.0") )
                {
                    sendcommandV20(internalid, SET_V20, 1, V_VAR3_V20, value);
                }
                else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.5") )
                {
                    sendcommandV15(internalid, SET_V15, 1, V_VAR3_V15, value);
                }
                else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.4") )
                {
                    sendcommandV14(internalid, SET_V14, 1, V_VAR3_V14, value);
                }
                else
                {
                    AGO_WARNING() << "Customvar is supported from protocol v1.4";
                    errorMessage = "Customvar is supported from protocol v1.4";
                }
            }
            else if( customvar=="VAR4" )
            {
                if( boost::algorithm::starts_with(infos["protocol"].asString(), "2.0") )
                {
                    sendcommandV20(internalid, SET_V20, 1, V_VAR4_V20, value);
                }
                else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.5") )
                {
                    sendcommandV15(internalid, SET_V15, 1, V_VAR4_V15, value);
                }
                else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.4") )
                {
                    sendcommandV14(internalid, SET_V14, 1, V_VAR4_V14, value);
                }
                else
                {
                    AGO_WARNING() << "Customvar is supported from protocol v1.4";
                    errorMessage = "Customvar is supported from protocol v1.4";
                }
            }
            else if( customvar=="VAR5" )
            {
                if( boost::algorithm::starts_with(infos["protocol"].asString(), "2.0") )
                {
                    sendcommandV20(internalid, SET_V20, 1, V_VAR5_V20, value);
                }
                else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.5") )
                {
                    sendcommandV15(internalid, SET_V15, 1, V_VAR5_V15, value);
                }
                else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.4") )
                {
                    sendcommandV14(internalid, SET_V14, 1, V_VAR5_V14, value);
                }
                else
                {
                    AGO_WARNING() << "Customvar is supported from protocol v1.4";
                    errorMessage = "Customvar is supported from protocol v1.4";
                }
            }
            else
            {
                //unknown customvar
                AGO_ERROR() << "Unsupported customvar '" << customvar << "'. Nothing processed";
                errorMessage = "Unsupported specified customvar";
            }
        }
    }
    else
    {
        checkMsgParameter(content, "internalid", Json::stringValue);
        std::string internalid = content["internalid"].asString();

        //get device infos
        infos = getDeviceInfos(internalid);
        //check if device found
        if( infos.size()>0 )
        {
            deviceType = infos["type"].asString();
            //switch according to specific device type
            if( deviceType=="switch" )
            {
                if( cmd=="off" )
                {
                    if( boost::algorithm::starts_with(infos["protocol"].asString(), "2.0") )
                    {
                        sendcommandV20(internalid, SET_V20, 1, V_STATUS_V20, "0");
                    }
                    else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.5") )
                    {
                        sendcommandV15(internalid, SET_V15, 1, V_STATUS_V15, "0");
                    }
                    else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.4") )
                    {
                        sendcommandV14(internalid, SET_V14, 1, V_LIGHT_V14, "0");
                    }
                    else
                    {
                        sendcommandV13(internalid, SET_VARIABLE_V13, V_LIGHT_V13, "0");
                    }
                }
                else if( cmd=="on" )
                {
                    if( boost::algorithm::starts_with(infos["protocol"].asString(), "2.0") )
                    {
                        sendcommandV20(internalid, SET_V20, 1, V_STATUS_V20, "1");
                    }
                    else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.5") )
                    {
                        sendcommandV15(internalid, SET_V15, 1, V_STATUS_V15, "1");
                    }
                    else if( boost::algorithm::starts_with(infos["protocol"].asString(), "1.4") )
                    {
                        sendcommandV14(internalid, SET_V14, 1, V_LIGHT_V14, "1");
                    }
                    else
                    {
                        sendcommandV13(internalid, SET_VARIABLE_V13, V_LIGHT_V13, "1");
                    }
                }
            }
            else
            {
                //unhandled case
                AGO_ERROR() << "Unhandled case for device " << internalid << "[" << deviceType << "]";
                errorMessage = "Unhandled case";
            }
        }
        else
        {
            //internalid doesn't belong to this controller
            errorMessage = "Unhandled internalid";
        }
    }

    if (errorMessage.length() > 0)
    {
        return responseFailed(errorMessage);
    }

    return responseSuccess(returnval);
}

/**
 * Read line from Serial
 */
std::string AgoMySensors::readLine(bool* error)
{
    char c;
    std::string result;
    (*error) = false;
    try {
        for(;;) {
            serialPort.ReadChar(&c, 0);
            switch(c) {
                case '\r':
                    break;
                case '\n':
                    return result;
                default:
                    result+=c;
            }
        }
    }
    catch (std::exception& e) {
        AGO_ERROR() << "Unable to read line: " << e.what();
        (*error) = true;
    }
    return result;
}

/**
 * Create new device (called during init or when PRESENTATION message is received from MySensors gateway)
 */
void AgoMySensors::newDevice(std::string internalid, std::string devicetype, std::string protocol)
{
    //check some stuff
    AGO_TRACE() << "newdevice " << internalid << "-" << devicetype;
    if( !checkInternalid(internalid) )
    {
        //internal id is not valid!
        AGO_ERROR() << "Unable to add device[" << internalid << "], internalid '" << internalid << "' is not valid";
        return;
    }
    if( devicetype.length()==0 )
    {
        AGO_ERROR() << "Unable to add device[" << internalid << "], empty devicetype";
        return;
    }
    if( protocol.length()==0 || protocol==DEFAULT_PROTOCOL )
    {
        AGO_ERROR() << "Unable to add device[" << internalid << "], protocol[" << protocol << "] is invalid";
        return;
    }

    //check if device already exists
    Json::Value infos = getDeviceInfos(internalid);
    if( devicemap.isMember("devices") )
    {
        if( infos.size()>0 )
        {
            if( boost::algorithm::starts_with(protocol, "1.5") )
            {
                //no need to check protocol version for protocol version >= 1.5
            }
            else
            {
                //check protocol version
                if( infos.isMember("protocol") && protocol.size()>0 && protocol!=DEFAULT_PROTOCOL && infos["protocol"].asString()!=protocol )
                {
                    //sensors code was updated to different protocol
                    AGO_INFO() << "Sensor protocol changed (" << infos["protocol"] << "=>" << protocol << ")";

                    //refresh infos
                    infos["protocol"] = protocol;
                    setDeviceInfos(internalid, infos);
                }
            }

            //internalid already referenced
            if( infos.isMember("type") && infos["type"].asString()!=devicetype )
            {
                //sensors is probably reconditioned, remove it before adding it
                AGO_INFO() << "Reconditioned sensor detected (" << infos["type"] << "=>" << devicetype << ")";
                deleteDevice(internalid);
                //refresh infos
                infos = getDeviceInfos(internalid);
                //and add brand new device
                addDevice(internalid, devicetype, infos, protocol);
            }
            else
            {
                //sensor has just rebooted
                AGO_TRACE() << "Sensor '" << internalid << "'[" << devicetype << "] rebooted";
                addDevice(internalid, devicetype, infos, protocol);
            }
        }
        else
        {
            //add new device
            AGO_TRACE() << "Add new device '" << devicetype << "' with internalid '" << internalid << "'";
            addDevice(internalid, devicetype, infos, protocol);
        }
    }
}

/**
 * Process message of protocol v1.3
 */
void AgoMySensors::processMessageV13(int radioId, int childId, int messageType, int subType, std::string payload, std::string internalid, Json::Value infos)
{
    int valid = 0;
    std::stringstream id;
    std::stringstream timestamp;
    int freeid;

    switch (messageType)
    {
        case INTERNAL_V13:
            switch (subType)
            {
                case I_BATTERY_LEVEL_V13:
                    if( infos.size()==0 )
                    {
                        //create device
                        newDevice(internalid, "batterysensor", "1.3");
                    }

                    //update counters
                    if( infos.size()>0 )
                    {
                        if( !infos.isMember("counter_received") )
                        {
                            infos["counter_received"] = 1;
                        }
                        else
                        {
                            infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                        }
                        infos["last_timestamp"] = (int)(time(NULL));
                        setDeviceInfos(internalid, infos);
                    }

                    //emit battery level
                    agoConnection->emitEvent(internalid, "event.device.batterylevelchanged", payload, "percent");
                    break;
                case I_SKETCH_NAME_V13:
                    //only used to update timestamp. Useful if network relay support enabled
                    infos["last_timestamp"] = (int)(time(NULL));
                    setDeviceInfos(internalid, infos);
                    break;
                case I_TIME_V13:
                    timestamp << time(NULL);
                    sendcommandV13(internalid, INTERNAL_V13, I_TIME_V13, timestamp.str());
                    break;
                case I_REQUEST_ID_V13:
                    //return radio id to sensor
                    freeid = getFreeId();
                    //@info nodeId - The unique id (1-254) for this sensor. Default 255 (auto mode).
                    if( freeid>254 || freeid==0 )
                    {
                        AGO_ERROR() << "FATAL: no nodeId available!";
                    }
                    else
                    {
                        id << freeid;
                        sendcommandV13(internalid, INTERNAL_V13, I_REQUEST_ID_V13, id.str());
                    }
                    break;
                case I_PING_V13:
                    sendcommandV13(internalid, INTERNAL_V13, I_PING_ACK_V13, "");
                    break;
                case I_UNIT_V13:
                    sendcommandV13(internalid, INTERNAL_V13, I_UNIT_V13, units);
                    break;
                default:
                    AGO_WARNING() << "INTERNAL subtype '" << subType << "' not supported (protocol v1.3)";
            }
            break;

        case PRESENTATION_V13:
            AGO_TRACE() << "PRESENTATION: " << subType;
            switch (subType)
            {
                case S_DOOR_V13:
                case S_MOTION_V13:
                    newDevice(internalid, "binarysensor", payload);
                    break;
                case S_SMOKE_V13:
                    newDevice(internalid, "smokedetector", payload);
                    break;
                case S_LIGHT_V13:
                case S_HEATER_V13:
                    newDevice(internalid, "switch", payload);
                    break;
                case S_DIMMER_V13:
                    newDevice(internalid, "dimmer", payload);
                    break;
                case S_COVER_V13:
                    newDevice(internalid, "drapes", payload);
                    break;
                case S_TEMP_V13:
                    newDevice(internalid, "temperaturesensor", payload);
                    break;
                case S_HUM_V13:
                    newDevice(internalid, "humiditysensor", payload);
                    break;
                case S_BARO_V13:
                    newDevice(internalid, "barometersensor", payload);
                    break;
                case S_WIND_V13:
                    newDevice(internalid, "windsensor", payload);
                    break;
                case S_RAIN_V13:
                    newDevice(internalid, "rainsensor", payload);
                    break;
                case S_UV_V13:
                    newDevice(internalid, "uvsensor", payload);
                    break;
                case S_WEIGHT_V13:
                    newDevice(internalid, "weightsensor", payload);
                    break;
                case S_POWER_V13:
                    newDevice(internalid, "powermeter", payload);
                    break;
                case S_DISTANCE_V13:
                    newDevice(internalid, "distancesensor", payload);
                    break;
                case S_LIGHT_LEVEL_V13:
                    newDevice(internalid, "brightnesssensor", payload);
                    break;
                case S_LOCK_V13:
                    newDevice(internalid, "lock", payload);
                    break;
                case S_IR_V13:
                    newDevice(internalid, "infraredblaster", payload);
                    break;
                case S_WATER_V13:
                    newDevice(internalid, "watermeter", payload);
                    break;
                default:
                    AGO_WARNING() << "PRESENTATION subtype '" << subType << "' not supported (protocol v1.3)";
            }
            break;

        case REQUEST_VARIABLE_V13:
            if( infos.size()>0 )
            {
                //update counters
                if( !infos.isMember("counter_sent") )
                {
                    infos["counter_sent"] = 1;
                }
                else
                {
                    infos["counter_sent"] = infos["counter_sent"].asUInt64()+1;
                }
                setDeviceInfos(internalid, infos);

                //send value
                sendcommandV13(internalid, SET_VARIABLE_V13, subType, infos["value"].asString());
            }
            else
            {
                //device not found
                //TODO log flood!
                AGO_ERROR()  << "Device not found: unable to get its value";
            }
            break;

        case SET_VARIABLE_V13:
            //update counters
            if( infos.size()>0 )
            {
                if( !infos.isMember("counter_received") )
                {
                    infos["counter_received"] = 1;
                }
                else
                {
                    infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                }
                infos["last_timestamp"] = (int)(time(NULL));
                setDeviceInfos(internalid, infos);
            }

            //do something on received event
            switch (subType)
            {
                case V_TEMP_V13:
                    valid = 1;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.temperaturechanged", payload, "degC");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.temperaturechanged", payload, "degF");
                    }
                    break;
                case V_TRIPPED_V13:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.security.sensortriggered", payload == "1" ? 255 : 0, "");
                    break;
                case V_HUM_V13:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.humiditychanged", payload, "percent");
                    break;
                case V_LIGHT_V13:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.device.statechanged", payload=="1" ? 255 : 0, "");
                    break;
                case V_DIMMER_V13:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.device.statechanged", payload, "");
                    break;
                case V_PRESSURE_V13:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.pressurechanged", payload, "mBar");
                    break;
                case V_FORECAST_V13:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.forecastchanged", payload, "");
                    break;
                case V_RAIN_V13:
                case V_RAINRATE_V13:
                    valid = 1;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.rainchanged", payload, "mm");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.rainchanged", payload, "inches");
                    }
                    break;
                case V_WIND_V13:
                    break;
                case V_GUST_V13:
                    break;
                case V_DIRECTION_V13:
                    break;
                case V_UV_V13:
                    break;
                case V_WEIGHT_V13:
                    break;
                case V_DISTANCE_V13: 
                    valid = 1;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.distancechanged", payload, "cm");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.distancechanged", payload, "inch");
                    }
                    break;
                case V_IMPEDANCE_V13:
                    break;
                case V_ARMED_V13:
                    break;
                case V_WATT_V13:
                    break;
                case V_KWH_V13:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.powerchanged", payload, "kWh");
                    break;
                case V_SCENE_ON_V13:
                    break;
                case V_SCENE_OFF_V13:
                    break;
                case V_HEATER_V13:
                    break;
                case V_HEATER_SW_V13:
                    break;
                case V_LIGHT_LEVEL_V13:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.brightnesschanged", payload, "lux");
                    break;
                case V_VAR1_V13:
                    break;
                case V_VAR2_V13:
                    break;
                case V_VAR3_V13:
                    break;
                case V_VAR4_V13:
                    break;
                case V_VAR5_V13:
                    break;
                case V_UP_V13:
                    break;
                case V_DOWN_V13:
                    break;
                case V_STOP_V13:
                    break;
                case V_IR_SEND_V13:
                    break;
                case V_IR_RECEIVE_V13:
                    break;
                case V_FLOW_V13:
                    break;
                case V_VOLUME_V13:
                    break;
                case V_LOCK_STATUS_V13:
                    break;
                default:
                    break;
            }

            if( valid==1 )
            {
                //save current device value
                infos = getDeviceInfos(internalid);
                if( infos.size()>0 )
                {
                    infos["value"] = payload;
                    setDeviceInfos(internalid, infos);
                }
            }
            else
            {
                //unsupported sensor
                AGO_ERROR() << "WARN: sensor with subType=" << subType << " not supported yet (protocol v1.3)";
            }

            //send ack
            sendcommandV13(internalid, VARIABLE_ACK_V13, subType, payload);
            break;

        case VARIABLE_ACK_V13:
            //TODO useful on controller?
            AGO_TRACE() << "VARIABLE_ACK";
            break;

        default:
            break;
    }
}

/**
 * Process message v1.4
 */
void AgoMySensors::processMessageV14(int nodeId, int childId, int messageType, int ack, int subType, std::string payload, std::string internalid, Json::Value infos)
{
    int valid = INVALID;
    std::stringstream timestamp;
    std::stringstream id;
    int freeid;
    std::map<std::string, T_COMMAND>::iterator cmd;

    switch (messageType)
    {
        case INTERNAL_V14:
            switch (subType)
            {
                case I_BATTERY_LEVEL_V14:
                    if( infos.size()==0 )
                    {
                        //create device
                        newDevice(internalid, "batterysensor", "1.4");
                    }

                    //update counters
                    if( infos.size()>0 )
                    {
                        if( !infos.isMember("counter_received") )
                        {
                            infos["counter_received"] = 1;
                        }
                        else
                        {
                            infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                        }
                        infos["last_timestamp"] = (int)(time(NULL));
                        setDeviceInfos(internalid, infos);
                    }

                    //emit battery level
                    agoConnection->emitEvent(internalid, "event.device.batterylevelchanged", payload, "percent");
                    break;
                case I_TIME_V14:
                    timestamp << time(NULL);
                    sendcommandV14(internalid, INTERNAL_V14, 0, I_TIME_V14, timestamp.str());
                    break;
                case I_ID_REQUEST_V14:
                    //return radio id to sensor
                    freeid = getFreeId();
                    //@info nodeId - The unique id (1-254) for this sensor. Default 255 (auto mode).
                    if( freeid>254 || freeid==0 )
                    {
                        AGO_ERROR() << "FATAL: no nodeId available!";
                    }
                    else
                    {
                        id << freeid;
                        sendcommandV14(internalid, INTERNAL_V14, 0, I_ID_RESPONSE_V14, id.str());
                    }
                    break;
                case I_LOG_MESSAGE_V14:
                    {
                        //debug message from gateway. Already displayed with prettyPrint when debug activated
                        //we use it here to count errors (only when gateway sends something to a sensor) and update last route
                        
                        //parse message
                        Json::Value items;
                        if( splitInternalLogMessage(payload, items) )
                        {
                            //handle send message
                            if( items["type"].asString()=="send" )
                            {
                                //build destinationid
                                std::string destinationid = items["destination"].asString() + "/" + items["sensor"].asString();
                                AGO_TRACE() << "destinationid=" << destinationid;
    
                                Json::Value infos = getDeviceInfos(destinationid);
                                if( infos.size()>0 )
                                {
                                    //increase sent counter
                                    if( !infos.isMember("counter_sent") )
                                    {
                                        infos["counter_sent"] = 1;
                                    }
                                    else
                                    {
                                        infos["counter_sent"] = infos["counter_sent"].asUInt64()+1;
                                    }
                                    
                                    //increase failed counter
                                    if( items["status"].asString()=="fail" )
                                    {
                                        if( !infos.isMember("counter_failed") )
                                        {
                                            infos["counter_failed"] = 1;
                                        }
                                        else
                                        {
                                            infos["counter_failed"] = infos["counter_failed"].asUInt64()+1;
                                        }
                                    }
                                    setDeviceInfos(destinationid, infos);
                                }
                            }
                            else if( items["type"]=="read" )
                            {
                                //handle read message
                                std::string destinationid = items["sender"].asString() + "/" + items["sensor"].asString();
                                AGO_TRACE() << "destinationid=" << destinationid;
    
                                //build route
                                if( items["sender"].asString()!="255" && items["last"].asString()!="255" && items["destination"].asString()!="255" )
                                {
                                    std::string route = items["sender"].asString() + "->" + items["last"].asString() + "->" + items["destination"].asString();

                                    Json::Value infos = getDeviceInfos(destinationid);
                                    if( infos.size()>0 )
                                    {
                                        //update last route
                                        if( !infos.isMember("last_route") )
                                        {
                                            AGO_TRACE() << destinationid << " route changed to " << route;
                                            infos["last_route"] = route;
                                            setDeviceInfos(destinationid, infos);
                                        }
                                        else if( infos["last_route"].asString()!=route )
                                        {
                                            AGO_TRACE() << destinationid << " route changed from " << infos["last_route"].asString() << " to " << route;
                                            infos["last_route"] = route;
                                            setDeviceInfos(destinationid, infos);
                                        }
                                    }
                                }
                            }
                        }   
                    }
                    break;
                case I_CONFIG_V14:
                    //return config
                    sendcommandV14(internalid, INTERNAL_V14, 0, I_CONFIG_V14, units.c_str());
                case I_SKETCH_NAME_V14:
                    //handled by useless (just to remove some unsupported log messages)
                    break;
                case I_SKETCH_VERSION_V14:
                    //only used to update network relay timestamp to detect stale state
                    if( bNetworkRelay )
                    {
                        Json::Value infos = getDeviceInfos(internalid);
                        if( infos.size()>0 && infos["type"]=="networkrelay" )
                        {
                            infos["last_timestamp"] = (int)(time(NULL));
                            infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                            setDeviceInfos(internalid, infos);
                        }
                    }
                    break;
                default:
                    AGO_WARNING() << "INTERNAL subtype '" << subType << "' not supported (protocol v1.4)";
            }
            break;

        case PRESENTATION_V14:
            switch (subType)
            {
                case S_DOOR_V14:
                case S_MOTION_V14:
                    newDevice(internalid, "binarysensor", payload);
                    break;
                case S_SMOKE_V14:
                    newDevice(internalid, "smokedetector", payload);
                    break;
                case S_LIGHT_V14:
                case S_HEATER_V14:
                    newDevice(internalid, "switch", payload);
                    break;
                case S_DIMMER_V14:
                    newDevice(internalid, "dimmer", payload);
                    break;
                case S_COVER_V14:
                    newDevice(internalid, "drapes", payload);
                    break;
                case S_TEMP_V14:
                    newDevice(internalid, "temperaturesensor", payload);
                    break;
                case S_HUM_V14:
                    newDevice(internalid, "humiditysensor", payload);
                    break;
                case S_BARO_V14:
                    newDevice(internalid, "barometersensor", payload);
                    break;
                case S_WIND_V14:
                    newDevice(internalid, "windsensor", payload);
                    break;
                case S_RAIN_V14:
                    newDevice(internalid, "rainsensor", payload);
                    break;
                case S_UV_V14:
                    newDevice(internalid, "uvsensor", payload);
                    break;
                case S_WEIGHT_V14:
                    newDevice(internalid, "weightsensor", payload);
                    break;
                case S_POWER_V14:
                    newDevice(internalid, "powermeter", payload);
                    break;
                case S_DISTANCE_V14:
                    newDevice(internalid, "distancesensor", payload);
                    break;
                case S_LIGHT_LEVEL_V14:
                    newDevice(internalid, "brightnesssensor", payload);
                    break;
                case S_ARDUINO_RELAY_V14:
                    if( bNetworkRelay )
                    {
                        newDevice(internalid, "networkrelay", payload);
                    }
                    break;
                case S_LOCK_V14:
                    newDevice(internalid, "lock", payload);
                    break;
                case S_IR_V14:
                    newDevice(internalid, "infraredblaster", payload);
                    break;
                case S_AIR_QUALITY_V14:
                    newDevice(internalid, "airsensor", payload);
                    break;
                case S_CUSTOM_V14:
                    AGO_WARNING() << "Device type 'CUSTOM' cannot be implemented in agocontrol";
                    break;
                case S_DUST_V14:
                    newDevice(internalid, "dustsensor", payload);
                    break;
                case S_SCENE_CONTROLLER_V14:
                    newDevice(internalid, "scenecontroller", payload);
                    break;
                default:
                    AGO_WARNING() << "PRESENTATION subtype '" << subType << "' not supported (protocol v1.4)";
            }
            break;

        case REQ_V14:
            if( infos.size()>0 )
            {
                //agocontrol save device value in device map regardless the sensor type.
                //here we handle specific custom vars, to make possible having multiple values for the same device
                switch (subType)
                {
                    case V_VAR1_V14:
                        //send var1 value (already reserved for pin code)
                        if( infos.isMember("pin") )
                        {
                            sendcommandV14(internalid, SET_V14, 0, subType, infos["pin"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'pin' value. Returned value [0] is not valid.";
                            sendcommandV14(internalid, SET_V14, 0, subType, "0");
                        }
                        break;
                    case V_VAR2_V14:
                        //send var2 value
                        if( infos.isMember("custom_var2") )
                        {
                            sendcommandV14(internalid, SET_V14, 0, subType, infos["custom_var2"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var2' value. Returned value [0] is not valid.";
                            sendcommandV14(internalid, SET_V14, 0, subType, "0");
                        }
                        break;
                    case V_VAR3_V14:
                        //send var3 value
                        if( infos.isMember("custom_var3") )
                        {
                            sendcommandV14(internalid, SET_V14, 0, subType, infos["custom_var3"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var3' value. Returned value [0] is not valid.";
                            sendcommandV14(internalid, SET_V14, 0, subType, "0");
                        }
                        break;
                    case V_VAR4_V14:
                        //send var4 value
                        if( infos.isMember("custom_var4") )
                        {
                            sendcommandV14(internalid, SET_V14, 0, subType, infos["custom_var4"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var4' value. Returned value [0] is not valid.";
                            sendcommandV14(internalid, SET_V14, 0, subType, "0");
                        }
                        break;
                    case V_VAR5_V14:
                        //send var5 value
                        if( infos.isMember("custom_var5") )
                        {
                            sendcommandV14(internalid, SET_V14, 0, subType, infos["custom_var5"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var5' value. Returned value [0] is not valid.";
                            sendcommandV14(internalid, SET_V14, 0, subType, "0");
                        }
                        break;
                    default:
                        //send default value
                        sendcommandV14(internalid, SET_V14, 0, subType, infos["value"].asString());
                }
            }
            else
            {
                //device not found
                AGO_ERROR() << "Device not found: unable to get its value";
            }
            break;

        case SET_V14:
            //update counters
            if( infos.size()>0 )
            {
                if( !infos.isMember("counter_received") )
                {
                    infos["counter_received"] = 1;
                }
                else
                {
                    infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                }
                infos["last_timestamp"] = (int)(time(NULL));
                setDeviceInfos(internalid, infos);
            }

            //do something on received event
            switch (subType)
            {
                case V_TEMP_V14:
                    valid = VALID_SAVE;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.temperaturechanged", payload, "degC");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.temperaturechanged", payload, "degF");
                    }
                    break;
                case V_TRIPPED_V14:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.security.sensortriggered", payload == "1" ? 255 : 0, "");
                    break;
                case V_HUM_V14:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.humiditychanged", payload, "percent");
                    break;
                case V_LIGHT_V14:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.device.statechanged", payload=="1" ? 255 : 0, "");
                    break;
                case V_DIMMER_V14:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.device.statechanged", payload, "");
                    break;
                case V_PRESSURE_V14:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.pressurechanged", payload, "mBar");
                    break;
                case V_FORECAST_V14:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.forecastchanged", payload, "");
                    break;
                case V_RAIN_V14:
                case V_RAINRATE_V14:
                    valid = VALID_SAVE;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.rainchanged", payload, "mm");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.rainchanged", payload, "inches");
                    }
                    break;
                case V_WIND_V14:
                    break;
                case V_GUST_V14:
                    break;
                case V_DIRECTION_V14:
                    break;
                case V_UV_V14:
                    break;
                case V_WEIGHT_V14:
                    break;
                case V_DISTANCE_V14: 
                    valid = VALID_SAVE;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.distancechanged", payload, "cm");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.distancechanged", payload, "inch");
                    }
                    break;
                case V_IMPEDANCE_V14:
                    break;
                case V_ARMED_V14:
                    break;
                case V_WATT_V14:
                    break;
                case V_KWH_V14:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.powerchanged", payload, "kWh");
                    break;
                case V_SCENE_ON_V14:
                    break;
                case V_SCENE_OFF_V14:
                    break;
                case V_HEATER_V14:
                    break;
                case V_HEATER_SW_V14:
                    break;
                case V_LIGHT_LEVEL_V14:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.brightnesschanged", payload, "lux");
                    break;
                case V_VAR1_V14:
                    //custom value 1 is reserved for pin code
                    valid = VALID_DONT_SAVE;
                    {
                        Json::Value payloadMap;
                        payloadMap["pin"]=payload;
                        agoConnection->emitEvent(internalid, "event.security.pinentered", payloadMap);
                    }
                    break;
                case V_VAR2_V14:
                    //save custom value
                    valid = VALID_VAR2;
                    //but no event triggered
                    break;
                case V_VAR3_V14:
                    //save custom value
                    valid = VALID_VAR3;
                    //but no event triggered
                    break;
                case V_VAR4_V14:
                    //save custom value
                    valid = VALID_VAR4;
                    //but no event triggered
                    break;
                case V_VAR5_V14:
                    //save custom value
                    valid = VALID_VAR5;
                    //but no event triggered
                    break;
                case V_UP_V14:
                    break;
                case V_DOWN_V14:
                    break;
                case V_STOP_V14:
                    break;
                case V_IR_SEND_V14:
                    break;
                case V_IR_RECEIVE_V14:
                    break;
                case V_FLOW_V14:
                    break;
                case V_VOLUME_V14:
                    break;
                case V_LOCK_STATUS_V14:
                    break;
                case V_DUST_LEVEL_V14:
                    break;
                case V_VOLTAGE_V14:
                    break;
                case V_CURRENT_V14:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.powerchanged", payload, "A");
                    break;
                default:
                    break;
            }

            if( valid==INVALID )
            {
                //unsupported sensor
                AGO_ERROR() << "WARN: sensor with subType=" << subType << " not supported yet (protocol v1.4)";
            }
            else if( valid==VALID_DONT_SAVE )
            {
                //don't save received value
            }
            else 
            {
                //save current device value
                infos = getDeviceInfos(internalid);
                if( infos.size()>0 )
                {
                    switch(valid)
                    {
                        case VALID_SAVE:
                            //default value
                            infos["value"] = payload;
                            break;
                        case VALID_VAR1:
                            //custom var1 is reserved for pin code
                            break;
                        case VALID_VAR2:
                            //save custom var2
                            infos["custom_var2"] = payload;
                            break;
                        case VALID_VAR3:
                            //save custom var3
                            infos["custom_var3"] = payload;
                            break;
                        case VALID_VAR4:
                            //save custom var4
                            infos["custom_var4"] = payload;
                            break;
                        case VALID_VAR5:
                            //save custom var5
                            infos["custom_var5"] = payload;
                            break;
                        default:
                            AGO_ERROR() << "Unhandled valid case [" << valid << "]. Please check code!";
                    }
                    setDeviceInfos(internalid, infos);
                }
            }

            //send ack if necessary
            if( ack )
            {
                sendcommandV14(internalid, SET_V14, 0, subType, payload);
            }
            break;

        case STREAM_V14:
            //TODO nothing implemented in MySensor yet
            AGO_TRACE() << "STREAM";
            break;
        default:
            break;
    }
}

/**
 * Process message v2.0
 */
void AgoMySensors::processMessageV20(int nodeId, int childId, int messageType, int ack, int subType, std::string payload, std::string internalid, Json::Value infos)
{
    int valid = INVALID;
    std::stringstream timestamp;
    std::stringstream id;
    int freeid;
    std::map<std::string, T_COMMAND>::iterator cmd;
    std::string strNodeId = boost::lexical_cast<std::string>(nodeId);

    switch (messageType)
    {
        case INTERNAL_V20:
            switch (subType)
            {
                case I_BATTERY_LEVEL_V20:
                    if( infos.size()==0 )
                    {
                        //create device
                        newDevice(internalid, "batterysensor", "2.0");
                    }

                    //update counters
                    if( infos.size()>0 )
                    {
                        if( !infos.isMember("counter_received") )
                        {
                            infos["counter_received"] = 1;
                        }
                        else
                        {
                            infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                        }
                        infos["last_timestamp"] = (int)(time(NULL));
                        setDeviceInfos(internalid, infos);
                    }

                    //emit battery level
                    agoConnection->emitEvent(internalid, "event.device.batterylevelchanged", payload, "percent");
                    break;
                case I_TIME_V20:
                    timestamp << time(NULL);
                    sendcommandV20(internalid, INTERNAL_V20, 0, I_TIME_V20, timestamp.str());
                    break;
                case I_ID_REQUEST_V20:
                    //return radio id to sensor
                    freeid = getFreeId();
                    //@info nodeId - The unique id (1-254) for this sensor. Default 255 (auto mode).
                    if( freeid>254 || freeid==0 )
                    {
                        AGO_ERROR() << "FATAL: no nodeId available!";
                    }
                    else
                    {
                        id << freeid;
                        sendcommandV20(internalid, INTERNAL_V20, 0, I_ID_RESPONSE_V20, id.str());
                    }
                    break;
                case I_LOG_MESSAGE_V20:
                    {
                        //debug message from gateway. Already displayed with prettyPrint when debug activated
                        //we use it here to count errors (only when gateway sends something to a sensor) and update last route
                        
                        //parse message
                        Json::Value items;
                        if( splitInternalLogMessage(payload, items) )
                        {
                            //handle send message
                            if( items["type"].asString()=="send" )
                            {
                                //build destinationid
                                std::string destinationid = items["destination"].asString() + "/" + items["sensor"].asString();
                                AGO_TRACE() << "destinationid=" << destinationid;
    
                                Json::Value infos = getDeviceInfos(destinationid);
                                if( infos.size()>0 )
                                {
                                    //increase sent counter
                                    if( !infos.isMember("counter_sent") )
                                    {
                                        infos["counter_sent"] = 1;
                                    }
                                    else
                                    {
                                        infos["counter_sent"] = infos["counter_sent"].asUInt64()+1;
                                    }
                                    
                                    //increase failed counter
                                    if( items["status"].asString()=="fail" )
                                    {
                                        if( !infos.isMember("counter_failed") )
                                        {
                                            infos["counter_failed"] = 1;
                                        }
                                        else
                                        {
                                            infos["counter_failed"] = infos["counter_failed"].asUInt64()+1;
                                        }
                                    }
                                    setDeviceInfos(destinationid, infos);
                                }
                            }
                            else if( items["type"]=="read" )
                            {
                                //handle read message
                                std::string destinationid = items["sender"].asString() + "/" + items["sensor"].asString();
                                AGO_TRACE() << "destinationid=" << destinationid;
    
                                //build route
                                if( items["sender"].asString()!="255" && items["last"].asString()!="255" && items["destination"].asString()!="255" )
                                {
                                    std::string route = items["sender"].asString() + "->" + items["last"].asString() + "->" + items["destination"].asString();
    
                                    Json::Value infos = getDeviceInfos(destinationid);
                                    if( infos.size()>0 )
                                    {
                                        //update last route
                                        if( !infos.isMember("last_route") )
                                        {
                                            AGO_TRACE() << destinationid << " route changed to " << route;
                                            infos["last_route"] = route;
                                            setDeviceInfos(destinationid, infos);
                                        }
                                        else if( infos["last_route"].asString()!=route )
                                        {
                                            AGO_TRACE() << destinationid << " route changed from " << infos["last_route"].asString() << " to " << route;
                                            infos["last_route"] = route;
                                            setDeviceInfos(destinationid, infos);
                                        }
                                    }
                                }
                            }
                        }   
                    }
                    break;
                case I_CONFIG_V20:
                    //return config
                    sendcommandV20(internalid, INTERNAL_V20, 0, I_CONFIG_V20, units.c_str());
                case I_SKETCH_NAME_V20:
                    //handled but useless (just to remove some unsupported log messages)
                    break;
                case I_SKETCH_VERSION_V20:
                    //only used to update network relay timestamp to detect stale state
                    if( bNetworkRelay )
                    {
                        Json::Value infos = getDeviceInfos(internalid);
                        if( infos.size()>0 && infos["type"]=="networkrelay" )
                        {
                            infos["last_timestamp"] = (int)(time(NULL));
                            infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                            setDeviceInfos(internalid, infos);
                        }
                    }
                    break;
                case I_GATEWAY_READY_V20:
                    AGO_TRACE() << "Received GATEWAY_READY message";
                    break;
                default:
                    AGO_WARNING() << "INTERNAL subtype '" << subType << "' not supported (protocol v1.4)";
            }
            break;

        case PRESENTATION_V20:
            switch (subType)
            {
                case S_DOOR_V20:
                case S_MOTION_V20:
                    newDevice(internalid, "binarysensor", payload);
                    break;
                case S_SMOKE_V20:
                    newDevice(internalid, "smokedetector", payload);
                    break;
                case S_BINARY_V20:
                case S_HEATER_V20:
                    newDevice(internalid, "switch", payload);
                    break;
                case S_DIMMER_V20:
                    newDevice(internalid, "dimmer", payload);
                    break;
                case S_COVER_V20:
                    newDevice(internalid, "drapes", payload);
                    break;
                case S_TEMP_V20:
                    newDevice(internalid, "temperaturesensor", payload);
                    break;
                case S_HUM_V20:
                    newDevice(internalid, "humiditysensor", payload);
                    break;
                case S_BARO_V20:
                    newDevice(internalid, "barometersensor", payload);
                    break;
                case S_WIND_V20:
                    newDevice(internalid, "windsensor", payload);
                    break;
                case S_RAIN_V20:
                    newDevice(internalid, "rainsensor", payload);
                    break;
                case S_UV_V20:
                    newDevice(internalid, "uvsensor", payload);
                    break;
                case S_WEIGHT_V20:
                    newDevice(internalid, "weightsensor", payload);
                    break;
                case S_POWER_V20:
                    newDevice(internalid, "powermeter", payload);
                    break;
                case S_DISTANCE_V20:
                    newDevice(internalid, "distancesensor", payload);
                    break;
                case S_LIGHT_LEVEL_V20:
                    newDevice(internalid, "brightnesssensor", payload);
                    break;
                case S_ARDUINO_NODE_V20:
                {
                    //in v1.5 and v2.0 each device sends ARDUINO_NODE presentation that contains protocol version
                    //while in v1.4.X and above each presentation messages contained protocol version
                    //so we need to check protocol here
                    
                    //check and update if necessary protocol version of all sensors of current node
                    if( devicemap.isMember("devices") )
                    {
                        const Json::Value& devices = devicemap["devices"];
                        for(auto it=devices.begin(); it!=devices.end(); it++ )
                        {
                            if( it->type() == Json::objectValue )
                            {
                                const Json::Value& infos = *it;
                                std::string tmpInternalid = it.name();
                                if( boost::algorithm::starts_with(tmpInternalid, strNodeId) )
                                {
                                    //sensor is found
                                    if( infos.isMember("protocol") && payload.size()>0 && payload!=DEFAULT_PROTOCOL && infos["protocol"].asString()!=payload )
                                    {
                                        //sensors code was updated to different protocol
                                        AGO_INFO() << "Sensor " << tmpInternalid << " protocol changed (" << infos["protocol"] << "=>" << payload << ")";

                                        //refresh infos
                                        Json::Value info = infos;
                                        info["protocol"] = payload;
                                        setDeviceInfos(tmpInternalid, info);
                                    }
                                }
                            }
                        }
                    }

                    //always keep track of arduino nodes and save associated protocol version
                    AGO_TRACE() << "Arduino node received, save its infos nodeid[" << nodeId << "]==protocol[" << payload << "]";
                    if( payload.length()>0 )
                    {
                        arduinoNodes[strNodeId] = payload;
                    }

                    break;
                }
                case S_ARDUINO_REPEATER_NODE_V20:
                    if( bNetworkRelay )
                    {
                        newDevice(internalid, "networkrelay", payload);
                    }

                    //save protocol version for this node
                    if( payload.length()>0 )
                    {
                        arduinoNodes[strNodeId] = payload;
                    }
                    break;
                case S_LOCK_V20:
                    newDevice(internalid, "lock", payload);
                    break;
                case S_IR_V20:
                    newDevice(internalid, "infraredblaster", payload);
                    break;
                case S_AIR_QUALITY_V20:
                    newDevice(internalid, "airsensor", payload);
                    break;
                case S_CUSTOM_V20:
                    AGO_WARNING() << "Device type 'CUSTOM' cannot be implemented in agocontrol";
                    break;
                case S_DUST_V20:
                    newDevice(internalid, "dustsensor", payload);
                    break;
                case S_SCENE_CONTROLLER_V20:
                    newDevice(internalid, "scenecontroller", payload);
                    break;
                default:
                    AGO_WARNING() << "PRESENTATION subtype '" << subType << "' not supported (protocol v1.4)";
            }
            break;

        case REQ_V20:
            if( infos.size()>0 )
            {
                //agocontrol save device value in device map regardless the sensor type.
                //here we handle specific custom vars, to make possible having multiple values for the same device
                switch (subType)
                {
                    case V_VAR1_V20:
                        //send var1 value (already reserved for security pin code)
                        if( infos.isMember("pin") )
                        {
                            sendcommandV20(internalid, SET_V20, 0, subType, infos["pin"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'pin' value. Returned value [0] is not valid.";
                            sendcommandV20(internalid, SET_V20, 0, subType, "0");
                        }
                        break;
                    case V_VAR2_V20:
                        //send var2 value
                        if( infos.isMember("custom_var2") )
                        {
                            sendcommandV20(internalid, SET_V20, 0, subType, infos["custom_var2"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var2' value. Returned value [0] is not valid.";
                            sendcommandV20(internalid, SET_V20, 0, subType, "0");
                        }
                        break;
                    case V_VAR3_V20:
                        //send var3 value
                        if( infos.isMember("custom_var3") )
                        {
                            sendcommandV20(internalid, SET_V20, 0, subType, infos["custom_var3"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var3' value. Returned value [0] is not valid.";
                            sendcommandV20(internalid, SET_V20, 0, subType, "0");
                        }
                        break;
                    case V_VAR4_V20:
                        //send var4 value
                        if( infos.isMember("custom_var4") )
                        {
                            sendcommandV20(internalid, SET_V20, 0, subType, infos["custom_var4"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var4' value. Returned value [0] is not valid.";
                            sendcommandV20(internalid, SET_V20, 0, subType, "0");
                        }
                        break;
                    case V_VAR5_V20:
                        //send var5 value
                        if( infos.isMember("custom_var5") )
                        {
                            sendcommandV20(internalid, SET_V20, 0, subType, infos["custom_var5"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var5' value. Returned value [0] is not valid.";
                            sendcommandV20(internalid, SET_V20, 0, subType, "0");
                        }
                        break;
                    default:
                        //send default value
                        sendcommandV20(internalid, SET_V20, 0, subType, infos["value"].asString());
                }
            }
            else
            {
                //device not found
                AGO_ERROR() << "Device not found: unable to get its value";
            }
            break;

        case SET_V20:
            //update counters
            if( infos.size()>0 )
            {
                if( !infos.isMember("counter_received") )
                {
                    infos["counter_received"] = 1;
                }
                else
                {
                    infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                }
                infos["last_timestamp"] = (int)(time(NULL));
                setDeviceInfos(internalid, infos);
            }

            //do something on received event
            switch (subType)
            {
                case V_TEMP_V20:
                    valid = VALID_SAVE;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.temperaturechanged", payload, "degC");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.temperaturechanged", payload, "degF");
                    }
                    break;
                case V_TRIPPED_V20:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.security.sensortriggered", payload == "1" ? 255 : 0, "");
                    break;
                case V_HUM_V20:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.humiditychanged", payload, "percent");
                    break;
                case V_STATUS_V20:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.device.statechanged", payload=="1" ? 255 : 0, "");
                    break;
                case V_PERCENTAGE_V20:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.device.statechanged", payload, "");
                    break;
                case V_PRESSURE_V20:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.pressurechanged", payload, "mBar");
                    break;
                case V_FORECAST_V20:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.forecastchanged", payload, "");
                    break;
                case V_RAIN_V20:
                case V_RAINRATE_V20:
                    valid = VALID_SAVE;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.rainchanged", payload, "mm");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.rainchanged", payload, "inches");
                    }
                    break;
                case V_WIND_V20:
                    break;
                case V_GUST_V20:
                    break;
                case V_DIRECTION_V20:
                    break;
                case V_UV_V20:
                    break;
                case V_WEIGHT_V20:
                    break;
                case V_DISTANCE_V20:
                    valid = VALID_SAVE;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.distancechanged", payload, "cm");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.distancechanged", payload, "inch");
                    }
                    break;
                case V_IMPEDANCE_V20:
                    break;
                case V_ARMED_V20:
                    break;
                case V_WATT_V20:
                    break;
                case V_KWH_V20:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.powerchanged", payload, "kWh");
                    break;
                case V_SCENE_ON_V20:
                    break;
                case V_SCENE_OFF_V20:
                    break;
                case V_HVAC_FLOW_STATE_V20:
                    break;
                case V_HVAC_SPEED_V20:
                    break;
                case V_LIGHT_LEVEL_V20:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.brightnesschanged", payload, "lux");
                    break;
                case V_VAR1_V20:
                    //custom value 1 is reserved for security pin code
                    valid = VALID_DONT_SAVE;
                    {
                        Json::Value payloadMap;
                        payloadMap["pin"]=payload;
                        agoConnection->emitEvent(internalid, "event.security.pinentered", payloadMap);
                    }
                    break;
                case V_VAR2_V20:
                    //save custom value
                    valid = VALID_VAR2;
                    //but no event triggered
                    break;
                case V_VAR3_V20:
                    //save custom value
                    valid = VALID_VAR3;
                    //but no event triggered
                    break;
                case V_VAR4_V20:
                    //save custom value
                    valid = VALID_VAR4;
                    //but no event triggered
                    break;
                case V_VAR5_V20:
                    //save custom value
                    valid = VALID_VAR5;
                    //but no event triggered
                    break;
                case V_UP_V20:
                    break;
                case V_DOWN_V20:
                    break;
                case V_STOP_V20:
                    break;
                case V_IR_SEND_V20:
                    break;
                case V_IR_RECEIVE_V20:
                    break;
                case V_FLOW_V20:
                    break;
                case V_VOLUME_V20:
                    break;
                case V_LOCK_STATUS_V20:
                    break;
                case V_LEVEL_V20:
                    break;
                case V_VOLTAGE_V20:
                    break;
                case V_CURRENT_V20:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.powerchanged", payload, "A");
                    break;
                case V_RGB_V20:
                    break;
                case V_RGBW_V20:
                    break;
                case V_ID_V20:
                    break;
                case V_UNIT_PREFIX_V20:
                    break;
                case V_HVAC_SETPOINT_COOL_V20:
                    break;
                case V_HVAC_SETPOINT_HEAT_V20:
                    break;
                case V_HVAC_FLOW_MODE_V20:
                    break;
                default:
                    break;
            }

            if( valid==INVALID )
            {
                //unsupported sensor
                AGO_ERROR() << "WARN: sensor with subType=" << subType << " not supported yet (protocol v1.5)";
            }
            else if( valid==VALID_DONT_SAVE )
            {
                //don't save received value
            }
            else 
            {
                //save current device value
                infos = getDeviceInfos(internalid);
                if( infos.size()>0 )
                {
                    switch(valid)
                    {
                        case VALID_SAVE:
                            //default value
                            infos["value"] = payload;
                            break;
                        case VALID_VAR1:
                            //custom var1 is reserved for pin code
                            break;
                        case VALID_VAR2:
                            //save custom var2
                            infos["custom_var2"] = payload;
                            break;
                        case VALID_VAR3:
                            //save custom var3
                            infos["custom_var3"] = payload;
                            break;
                        case VALID_VAR4:
                            //save custom var4
                            infos["custom_var4"] = payload;
                            break;
                        case VALID_VAR5:
                            //save custom var5
                            infos["custom_var5"] = payload;
                            break;
                        default:
                            AGO_ERROR() << "Unhandled valid case [" << valid << "]. Please check code!";
                    }
                    setDeviceInfos(internalid, infos);
                }
            }

            //send ack if necessary
            if( ack )
            {
                sendcommandV20(internalid, SET_V20, 0, subType, payload);
            }
            break;

        case STREAM_V20:
            AGO_TRACE() << "STREAM";
            break;
        default:
            break;
    }
}

/**
 * Process message v1.5
 */
void AgoMySensors::processMessageV15(int nodeId, int childId, int messageType, int ack, int subType, std::string payload, std::string internalid, Json::Value infos)
{
    int valid = INVALID;
    std::stringstream timestamp;
    std::stringstream id;
    int freeid;
    std::map<std::string, T_COMMAND>::iterator cmd;
    std::string strNodeId = boost::lexical_cast<std::string>(nodeId);

    switch (messageType)
    {
        case INTERNAL_V15:
            switch (subType)
            {
                case I_BATTERY_LEVEL_V15:
                    if( infos.size()==0 )
                    {
                        //create device
                        newDevice(internalid, "batterysensor", "1.5");
                    }

                    //update counters
                    if( infos.size()>0 )
                    {
                        if( !infos.isMember("counter_received") )
                        {
                            infos["counter_received"] = 1;
                        }
                        else
                        {
                            infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                        }
                        infos["last_timestamp"] = (int)(time(NULL));
                        setDeviceInfos(internalid, infos);
                    }

                    //emit battery level
                    agoConnection->emitEvent(internalid, "event.device.batterylevelchanged", payload, "percent");
                    break;
                case I_TIME_V15:
                    timestamp << time(NULL);
                    sendcommandV15(internalid, INTERNAL_V15, 0, I_TIME_V15, timestamp.str());
                    break;
                case I_ID_REQUEST_V15:
                    //return radio id to sensor
                    freeid = getFreeId();
                    //@info nodeId - The unique id (1-254) for this sensor. Default 255 (auto mode).
                    if( freeid>254 || freeid==0 )
                    {
                        AGO_ERROR() << "FATAL: no nodeId available!";
                    }
                    else
                    {
                        id << freeid;
                        sendcommandV15(internalid, INTERNAL_V15, 0, I_ID_RESPONSE_V15, id.str());
                    }
                    break;
                case I_LOG_MESSAGE_V15:
                    {
                        //debug message from gateway. Already displayed with prettyPrint when debug activated
                        //we use it here to count errors (only when gateway sends something to a sensor) and update last route
                        
                        //parse message
                        Json::Value items;
                        if( splitInternalLogMessage(payload, items) )
                        {
                            //handle send message
                            if( items["type"].asString()=="send" )
                            {
                                //build destinationid
                                std::string destinationid = items["destination"].asString() + "/" + items["sensor"].asString();
                                AGO_TRACE() << "destinationid=" << destinationid;
    
                                Json::Value infos = getDeviceInfos(destinationid);
                                if( infos.size()>0 )
                                {
                                    //increase sent counter
                                    if( !infos.isMember("counter_sent") )
                                    {
                                        infos["counter_sent"] = 1;
                                    }
                                    else
                                    {
                                        infos["counter_sent"] = infos["counter_sent"].asUInt64()+1;
                                    }
                                    
                                    //increase failed counter
                                    if( items["status"].asString()=="fail" )
                                    {
                                        if( !infos.isMember("counter_failed") )
                                        {
                                            infos["counter_failed"] = 1;
                                        }
                                        else
                                        {
                                            infos["counter_failed"] = infos["counter_failed"].asUInt64()+1;
                                        }
                                    }
                                    setDeviceInfos(destinationid, infos);
                                }
                            }
                            else if( items["type"]=="read" )
                            {
                                //handle read message
                                std::string destinationid = items["sender"].asString() + "/" + items["sensor"].asString();
                                AGO_TRACE() << "destinationid=" << destinationid;
    
                                //build route
                                if( items["sender"].asString()!="255" && items["last"].asString()!="255" && items["destination"].asString()!="255" )
                                {
                                    std::string route = items["sender"].asString() + "->" + items["last"].asString() + "->" + items["destination"].asString();
    
                                    Json::Value infos = getDeviceInfos(destinationid);
                                    if( infos.size()>0 )
                                    {
                                        //update last route
                                        if( !infos.isMember("last_route") )
                                        {
                                            AGO_TRACE() << destinationid << " route changed to " << route;
                                            infos["last_route"] = route;
                                            setDeviceInfos(destinationid, infos);
                                        }
                                        else if( infos["last_route"].asString()!=route )
                                        {
                                            AGO_TRACE() << destinationid << " route changed from " << infos["last_route"].asString() << " to " << route;
                                            infos["last_route"] = route;
                                            setDeviceInfos(destinationid, infos);
                                        }
                                    }
                                }
                            }
                        }   
                    }
                    break;
                case I_CONFIG_V15:
                    //return config
                    sendcommandV15(internalid, INTERNAL_V15, 0, I_CONFIG_V15, units.c_str());
                case I_SKETCH_NAME_V15:
                    //handled but useless (just to remove some unsupported log messages)
                    break;
                case I_SKETCH_VERSION_V15:
                    //only used to update network relay timestamp to detect stale state
                    if( bNetworkRelay )
                    {
                        Json::Value infos = getDeviceInfos(internalid);
                        if( infos.size()>0 && infos["type"]=="networkrelay" )
                        {
                            infos["last_timestamp"] = (int)(time(NULL));
                            infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                            setDeviceInfos(internalid, infos);
                        }
                    }
                    break;
                case I_GATEWAY_READY_V15:
                    AGO_TRACE() << "Received GATEWAY_READY message";
                    break;
                default:
                    AGO_WARNING() << "INTERNAL subtype '" << subType << "' not supported (protocol v1.4)";
            }
            break;

        case PRESENTATION_V15:
            switch (subType)
            {
                case S_DOOR_V15:
                case S_MOTION_V15:
                    newDevice(internalid, "binarysensor", payload);
                    break;
                case S_SMOKE_V15:
                    newDevice(internalid, "smokedetector", payload);
                    break;
                case S_BINARY_V15:
                case S_HEATER_V15:
                    newDevice(internalid, "switch", payload);
                    break;
                case S_DIMMER_V15:
                    newDevice(internalid, "dimmer", payload);
                    break;
                case S_COVER_V15:
                    newDevice(internalid, "drapes", payload);
                    break;
                case S_TEMP_V15:
                    newDevice(internalid, "temperaturesensor", payload);
                    break;
                case S_HUM_V15:
                    newDevice(internalid, "humiditysensor", payload);
                    break;
                case S_BARO_V15:
                    newDevice(internalid, "barometersensor", payload);
                    break;
                case S_WIND_V15:
                    newDevice(internalid, "windsensor", payload);
                    break;
                case S_RAIN_V15:
                    newDevice(internalid, "rainsensor", payload);
                    break;
                case S_UV_V15:
                    newDevice(internalid, "uvsensor", payload);
                    break;
                case S_WEIGHT_V15:
                    newDevice(internalid, "weightsensor", payload);
                    break;
                case S_POWER_V15:
                    newDevice(internalid, "powermeter", payload);
                    break;
                case S_DISTANCE_V15:
                    newDevice(internalid, "distancesensor", payload);
                    break;
                case S_LIGHT_LEVEL_V15:
                    newDevice(internalid, "brightnesssensor", payload);
                    break;
                case S_ARDUINO_NODE_V15:
                {
                    //in v1.5 each device sends ARDUINO_NODE presentation that contains protocol version
                    //while in v1.4.X and above each presentation messages contained protocol version
                    //so we need to check protocol here
                    
                    //check and update if necessary protocol version of all sensors of current node
                    if( devicemap.isMember("devices") )
                    {
                        const Json::Value& devices = devicemap["devices"];
                        for( auto it=devices.begin(); it!=devices.end(); it++ )
                        {
                            if( it->type() == Json::objectValue )
                            {
                                const Json::Value& infos = *it;
                                std::string tmpInternalid = it.name();
                                if( boost::algorithm::starts_with(tmpInternalid, strNodeId) )
                                {
                                    //sensor is found
                                    if( infos.isMember("protocol") && payload.size()>0 && payload!=DEFAULT_PROTOCOL && infos["protocol"].asString()!=payload )
                                    {
                                        //sensors code was updated to different protocol
                                        AGO_INFO() << "Sensor " << tmpInternalid << " protocol changed (" << infos["protocol"] << "=>" << payload << ")";

                                        //refresh infos
                                        Json::Value info = infos;
                                        info["protocol"] = payload;
                                        setDeviceInfos(tmpInternalid, info);
                                    }
                                }
                            }
                        }
                    }

                    //always keep track of arduino nodes and save associated protocol version
                    AGO_TRACE() << "Arduino node received, save its infos nodeid[" << nodeId << "]==protocol[" << payload << "]";
                    if( payload.length()>0 )
                    {
                        arduinoNodes[strNodeId] = payload;
                    }

                    break;
                }
                case S_ARDUINO_REPEATER_NODE_V15:
                    if( bNetworkRelay )
                    {
                        newDevice(internalid, "networkrelay", payload);
                    }

                    //save protocol version for this node
                    if( payload.length()>0 )
                    {
                        arduinoNodes[strNodeId] = payload;
                    }
                    break;
                case S_LOCK_V15:
                    newDevice(internalid, "lock", payload);
                    break;
                case S_IR_V15:
                    newDevice(internalid, "infraredblaster", payload);
                    break;
                case S_AIR_QUALITY_V15:
                    newDevice(internalid, "airsensor", payload);
                    break;
                case S_CUSTOM_V15:
                    AGO_WARNING() << "Device type 'CUSTOM' cannot be implemented in agocontrol";
                    break;
                case S_DUST_V15:
                    newDevice(internalid, "dustsensor", payload);
                    break;
                case S_SCENE_CONTROLLER_V15:
                    newDevice(internalid, "scenecontroller", payload);
                    break;
                default:
                    AGO_WARNING() << "PRESENTATION subtype '" << subType << "' not supported (protocol v1.4)";
            }
            break;

        case REQ_V15:
            if( infos.size()>0 )
            {
                //agocontrol save device value in device map regardless the sensor type.
                //here we handle specific custom vars, to make possible having multiple values for the same device
                switch (subType)
                {
                    case V_VAR1_V15:
                        //send var1 value (already reserved for pin code)
                        if( infos.isMember("pin") )
                        {
                            sendcommandV15(internalid, SET_V15, 0, subType, infos["pin"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'pin' value. Returned value [0] is not valid.";
                            sendcommandV15(internalid, SET_V15, 0, subType, "0");
                        }
                        break;
                    case V_VAR2_V15:
                        //send var2 value
                        if( infos.isMember("custom_var2") )
                        {
                            sendcommandV15(internalid, SET_V15, 0, subType, infos["custom_var2"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var2' value. Returned value [0] is not valid.";
                            sendcommandV15(internalid, SET_V15, 0, subType, "0");
                        }
                        break;
                    case V_VAR3_V15:
                        //send var3 value
                        if( infos.isMember("custom_var3") )
                        {
                            sendcommandV15(internalid, SET_V15, 0, subType, infos["custom_var3"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var3' value. Returned value [0] is not valid.";
                            sendcommandV15(internalid, SET_V15, 0, subType, "0");
                        }
                        break;
                    case V_VAR4_V15:
                        //send var4 value
                        if( infos.isMember("custom_var4") )
                        {
                            sendcommandV15(internalid, SET_V15, 0, subType, infos["custom_var4"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var4' value. Returned value [0] is not valid.";
                            sendcommandV15(internalid, SET_V15, 0, subType, "0");
                        }
                        break;
                    case V_VAR5_V15:
                        //send var5 value
                        if( infos.isMember("custom_var5") )
                        {
                            sendcommandV15(internalid, SET_V15, 0, subType, infos["custom_var5"].asString());
                        }
                        else
                        {
                            AGO_ERROR() << "Device '" << internalid << "' has no 'custom_var5' value. Returned value [0] is not valid.";
                            sendcommandV15(internalid, SET_V15, 0, subType, "0");
                        }
                        break;
                    default:
                        //send default value
                        sendcommandV15(internalid, SET_V15, 0, subType, infos["value"].asString());
                }
            }
            else
            {
                //device not found
                AGO_ERROR() << "Device not found: unable to get its value";
            }
            break;

        case SET_V15:
            //update counters
            if( infos.size()>0 )
            {
                if( !infos.isMember("counter_received") )
                {
                    infos["counter_received"] = 1;
                }
                else
                {
                    infos["counter_received"] = infos["counter_received"].asUInt64()+1;
                }
                infos["last_timestamp"] = (int)(time(NULL));
                setDeviceInfos(internalid, infos);
            }

            //do something on received event
            switch (subType)
            {
                case V_TEMP_V15:
                    valid = VALID_SAVE;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.temperaturechanged", payload, "degC");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.temperaturechanged", payload, "degF");
                    }
                    break;
                case V_TRIPPED_V15:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.security.sensortriggered", payload == "1" ? 255 : 0, "");
                    break;
                case V_HUM_V15:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.humiditychanged", payload, "percent");
                    break;
                case V_STATUS_V15:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.device.statechanged", payload=="1" ? 255 : 0, "");
                    break;
                case V_PERCENTAGE_V15:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.device.statechanged", payload, "");
                    break;
                case V_PRESSURE_V15:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.pressurechanged", payload, "mBar");
                    break;
                case V_FORECAST_V15:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.forecastchanged", payload, "");
                    break;
                case V_RAIN_V15:
                case V_RAINRATE_V15:
                    valid = VALID_SAVE;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.rainchanged", payload, "mm");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.rainchanged", payload, "inches");
                    }
                    break;
                case V_WIND_V15:
                    break;
                case V_GUST_V15:
                    break;
                case V_DIRECTION_V15:
                    break;
                case V_UV_V15:
                    break;
                case V_WEIGHT_V15:
                    break;
                case V_DISTANCE_V15:
                    valid = VALID_SAVE;
                    if (units == "M")
                    {
                        agoConnection->emitEvent(internalid, "event.environment.distancechanged", payload, "cm");
                    }
                    else
                    {
                        agoConnection->emitEvent(internalid, "event.environment.distancechanged", payload, "inch");
                    }
                    break;
                case V_IMPEDANCE_V15:
                    break;
                case V_ARMED_V15:
                    break;
                case V_WATT_V15:
                    break;
                case V_KWH_V15:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.powerchanged", payload, "kWh");
                    break;
                case V_SCENE_ON_V15:
                    break;
                case V_SCENE_OFF_V15:
                    break;
                case V_HVAC_FLOW_STATE_V15:
                    break;
                case V_HVAC_SPEED_V15:
                    break;
                case V_LIGHT_LEVEL_V15:
                    valid = VALID_SAVE;
                    agoConnection->emitEvent(internalid, "event.environment.brightnesschanged", payload, "lux");
                    break;
                case V_VAR1_V15:
                    //custom value 1 is reserved for pin code
                    valid = VALID_DONT_SAVE;
                    {
                        Json::Value payloadMap;
                        payloadMap["pin"]=payload;
                        agoConnection->emitEvent(internalid, "event.security.pinentered", payloadMap);
                    }
                    break;
                case V_VAR2_V15:
                    //save custom value
                    valid = VALID_VAR2;
                    //but no event triggered
                    break;
                case V_VAR3_V15:
                    //save custom value
                    valid = VALID_VAR3;
                    //but no event triggered
                    break;
                case V_VAR4_V15:
                    //save custom value
                    valid = VALID_VAR4;
                    //but no event triggered
                    break;
                case V_VAR5_V15:
                    //save custom value
                    valid = VALID_VAR5;
                    //but no event triggered
                    break;
                case V_UP_V15:
                    break;
                case V_DOWN_V15:
                    break;
                case V_STOP_V15:
                    break;
                case V_IR_SEND_V15:
                    break;
                case V_IR_RECEIVE_V15:
                    break;
                case V_FLOW_V15:
                    break;
                case V_VOLUME_V15:
                    break;
                case V_LOCK_STATUS_V15:
                    break;
                case V_LEVEL_V15:
                    break;
                case V_VOLTAGE_V15:
                    break;
                case V_CURRENT_V15:
                    valid = 1;
                    agoConnection->emitEvent(internalid, "event.environment.powerchanged", payload, "A");
                    break;
                case V_RGB_V15:
                    break;
                case V_RGBW_V15:
                    break;
                case V_ID_V15:
                    break;
                case V_UNIT_PREFIX_V15:
                    break;
                case V_HVAC_SETPOINT_COOL_V15:
                    break;
                case V_HVAC_SETPOINT_HEAT_V15:
                    break;
                case V_HVAC_FLOW_MODE_V15:
                    break;
                default:
                    break;
            }

            if( valid==INVALID )
            {
                //unsupported sensor
                AGO_ERROR() << "WARN: sensor with subType=" << subType << " not supported yet (protocol v1.5)";
            }
            else if( valid==VALID_DONT_SAVE )
            {
                //don't save received value
            }
            else 
            {
                //save current device value
                infos = getDeviceInfos(internalid);
                if( infos.size()>0 )
                {
                    switch(valid)
                    {
                        case VALID_SAVE:
                            //default value
                            infos["value"] = payload;
                            break;
                        case VALID_VAR1:
                            //custom var1 is reserved for pin code
                            break;
                        case VALID_VAR2:
                            //save custom var2
                            infos["custom_var2"] = payload;
                            break;
                        case VALID_VAR3:
                            //save custom var3
                            infos["custom_var3"] = payload;
                            break;
                        case VALID_VAR4:
                            //save custom var4
                            infos["custom_var4"] = payload;
                            break;
                        case VALID_VAR5:
                            //save custom var5
                            infos["custom_var5"] = payload;
                            break;
                        default:
                            AGO_ERROR() << "Unhandled valid case [" << valid << "]. Please check code!";
                    }
                    setDeviceInfos(internalid, infos);
                }
            }

            //send ack if necessary
            if( ack )
            {
                sendcommandV15(internalid, SET_V15, 0, subType, payload);
            }
            break;

        case STREAM_V15:
            //TODO nothing implemented in MySensor yet
            AGO_TRACE() << "STREAM";
            break;
        default:
            break;
    }
}

/**
 * Serial read function (threaded)
 */
void AgoMySensors::receiveFunction()
{
    bool error = false;
    std::string log = "";

    while (1)
    {
        pthread_mutex_lock (&serialMutex);

        //read line
        std::string line = readLine(&error);

        //check errors on serial port
        if( error )
        {
            //error occured! close port
            AGO_INFO() << "Reconnecting to serial port";
            closeSerialPort();
            //pause (100ms)
            usleep(100000);
            //and reopen it
            openSerialPort(serialDevice);

            pthread_mutex_unlock (&serialMutex);
            continue;
        }

        std::vector<std::string> items = split(line, ';');
        if ( items.size()>=4 && items.size()<=6 )
        {
            AGO_TRACE() << "------------ NEW LINE RECEIVED ------------";
            int nodeId = atoi(items[0].c_str());
            std::string strNodeId = items[0];
            int childId = atoi(items[1].c_str());
            std::string internalid = items[0] + "/" + items[1];
            int messageType = atoi(items[2].c_str());
            int subType;
            Json::Value infos;
            std::string payload = "";
            int ack = 0;
            std::string protocol = "";

            //try to get device infos
            infos = getDeviceInfos(internalid);

            //get protocol version
            if( infos.size()>0 )
            {
                AGO_TRACE() << "infos found=" << infos;
                //get protocol version from device infos
                if( infos.isMember("protocol") && infos["protocol"].asString().size()>0 )
                {
                    AGO_TRACE() << "Use protocol version from infos map";
                    protocol = infos["protocol"].asString();
                }
            }
            else if( (nodeId==0 && childId==0) || (nodeId==255 && childId==255) )
            {
                //message from gateway or broadcast message, set protocol version to gateway one
                AGO_TRACE() << "Use protocol version of gateway (gateway or broadcast message)";
                protocol = gateway_protocol_version;
            }
            else if( arduinoNodes.isMember(strNodeId) )
            {
                //we have informations from previous arduino nodes request, use it
                AGO_TRACE() << "Use protocol version found in arduinoNodes map";
                protocol = arduinoNodes[strNodeId].asString();
            }
            else
            {
                //get protocol version from current message (payload)
                
                //try >= v1.4 first
                if( items.size()==6 )
                    payload = items[5];

                if( boost::algorithm::starts_with(payload, "1.5") )
                {
                    //protocol v1.5 found
                    AGO_TRACE() << "Use protocol version 1.5 found in current message";
                    protocol = payload;
                }
                else if( boost::algorithm::starts_with(payload, "1.4") )
                {
                    //protocol v1.4 found
                    AGO_TRACE() << "Use protocol version 1.4 found in current message";
                    protocol = payload;
                }
                else
                {
                    //try protocol v1.3
                    if( items.size()==5 )
                        payload = items[4];
                    if( boost::algorithm::starts_with(payload, "1.3") )
                    {
                        AGO_TRACE() << "Use protocol version 1.3 found in current message";
                        //protocol v1.3 found
                        protocol = payload;
                    }
                }
            }
            AGO_TRACE() << "protocol found: " << protocol;

            //pretty print message
            if( logLevel<=log::debug )
            {
                if( protocol.size()==0 || protocol==DEFAULT_PROTOCOL )
                {
                    log = prettyPrint(line, gateway_protocol_version);
                }
                else
                {
                    log = prettyPrint(line, protocol);
                }
                if( log.size()>0 )
                {
                    AGO_DEBUG() << " => RECEIVING: " << log;
                }
            }

            //process message according to found protocol
            if( protocol.size()>0 && protocol!=DEFAULT_PROTOCOL )
            {
                if( boost::algorithm::starts_with(protocol, "2.0") )
                {
                    ack = atoi(items[3].c_str());
                    subType = atoi(items[4].c_str());
                    if( messageType==0 )
                    {
                        //if PRESENTATION message, force payload to found protocol version
                        payload = protocol;
                    }
                    else if( items.size()==6 )
                    {
                        payload = items[5];
                    }
                    processMessageV20(nodeId, childId, messageType, ack, subType, payload, internalid, infos);
                }
                else if( boost::algorithm::starts_with(protocol, "1.5") )
                {
                    ack = atoi(items[3].c_str());
                    subType = atoi(items[4].c_str());
                    if( messageType==0 )
                    {
                        //if PRESENTATION message, force payload to found protocol version
                        payload = protocol;
                    }
                    else if( items.size()==6 )
                    {
                        payload = items[5];
                    }
                    processMessageV15(nodeId, childId, messageType, ack, subType, payload, internalid, infos);
                }
                else if( boost::algorithm::starts_with(protocol, "1.4") )
                {
                    ack = atoi(items[3].c_str());
                    subType = atoi(items[4].c_str());
                    if( messageType==0 )
                    {
                        //if PRESENTATION message, force payload to found protocol version
                        payload = protocol;
                    }
                    else if( items.size()==6 )
                    {
                        payload = items[5];
                    }
                    processMessageV14(nodeId, childId, messageType, ack, subType, payload, internalid, infos);
                }
                else if( boost::algorithm::starts_with(protocol, "1.3") )
                {
                    subType = atoi(items[3].c_str());
                    if( messageType==0 )
                    {
                        //if PRESENTATION message, force payload to found protocol version
                        payload = protocol;
                    }
                    else if( items.size()==5 )
                    {
                        payload = items[4];
                    }
                    processMessageV13(nodeId, childId, messageType, subType, payload, internalid, infos);
                }
                else
                {
                    //unsupported protocol version
                    AGO_WARNING() << "Device is based on unsupported protocol version '" << protocol << "'";
                }
            }
            else
            {
                //no protocol version found for this message, drop it
                AGO_WARNING() << "No protocol version found for this message, drop it";
                AGO_TRACE() << "line=" << line;
                AGO_TRACE() << "nodeId=" << nodeId << " childId=" << childId;
                AGO_TRACE() << "infos=" << infos;
            }

            AGO_TRACE() << "------------ EOL ------------";
        }

        pthread_mutex_unlock (&serialMutex);
    }
}

/**
 * Device stale checking thread
 * Check stale state every minutes
 */
void AgoMySensors::checkStaleFunction()
{
    while(true)
    {
        //get current time
        time_t now = time(NULL);

        if( devicemap.isMember("devices") )
        {
            const Json::Value& devices = devicemap["devices"];
            for (auto it = devices.begin(); it != devices.end(); it++)
            {
                if ( it->type() == Json::objectValue )
                {
                    const Json::Value& infos = *it;
                    std::string internalid = it.name();
                    if( infos.isMember("last_timestamp") && checkInternalid(internalid) )
                    {
                        if( !agoConnection->isDeviceStale(internalid) )
                        {
                            if( (int)now>(infos["last_timestamp"].asInt()+staleThreshold) )
                            {
                                //device is stalled
                                AGO_TRACE() << "Stale: Suspend device " << internalid << " last_ts=" << infos["last_timestamp"].asInt() << " threshold=" << staleThreshold << " now=" << (int)now;
                                agoConnection->suspendDevice(internalid);
                            }
                        }
                        else
                        {
                            if( infos["last_timestamp"].asInt()>=((int)now-staleThreshold) )
                            {
                                //device woke up
                                AGO_TRACE() << "Stale: Resume device " << internalid << " last_ts=" << infos["last_timestamp"].asInt() << " threshold=" << staleThreshold << " now=" << (int)now;
                                agoConnection->resumeDevice(internalid);
                            }
                        }
                    }
                } else {
                    AGO_ERROR() << "Invalid entry in device map";
                }
            }
        }

        //pause (1min)
        sleep(60);
    }
}

/**
 * Setup
 */
void AgoMySensors::setupApp()
{
    //get config
    serialDevice = getConfigSectionOption("mysensors", "device", "/dev/ttyACM0");
    staleThreshold = atoi(getConfigSectionOption("mysensors", "staleThreshold", "86400").c_str());
    bNetworkRelay = false;
    if( atoi(getConfigSectionOption("mysensors", "networkrelay", "0").c_str())==1 )
    {
        bNetworkRelay = true;
        AGO_INFO() << "Network relay support enabled";
    }
    bStale = true;
    if( atoi(getConfigSectionOption("mysensors", "stale", "1").c_str())==0 )
    {
        bStale = false;
        AGO_INFO() << "Stale feature disabled";
    }

    // determine reply for INTERNAL;I_UNIT message - defaults to "M"etric
    units = "M";
    if( getConfigSectionOption("system","units","SI")!="SI" )
    {
        units = "I";
    }

    // load map, create sections if empty
    fs::path dmf = getConfigPath(DEVICEMAPFILE);
    readJsonFile(devicemap, dmf);
    if (!devicemap.isMember("devices"))
    {
        Json::Value devices;
        devicemap["devices"] = devices;
        writeJsonFile(devicemap, dmf);
    }

    //connect to gateway
    bool error = false;
    std::string line = "";
    int attempts = 0;
    AGO_INFO() << "Waiting for the gateway starts...";
    for( int i=0; i<3; i++ )
    {
        error = false;

        //open serial port
        AGO_DEBUG() << "Opening serial port '" << serialDevice << "'...";
        if( !openSerialPort(serialDevice) )
        {
            exit(1);
        }

        while( !error )
        {
            //read line from serial port
            line = readLine(&error);
            AGO_DEBUG() << "Read: " << line;

            //check connectivity
            if( line.find("check wires")!=std::string::npos )
            {
                AGO_ERROR() << "The serial gateway arduino sketch can't talk to the NRF24 module! Check wires and power supply!";
                exit(1);
            }

            //check gateway startup string
            if( line.find(" startup complete")!=std::string::npos )
            {
                //gateway is started
                AGO_DEBUG() << "Startup string found, continue";
                break;
            }

            //check attemps
            if( attempts>3 )
            {
                //max attemps reached without receiving awaited string
                AGO_DEBUG() << "Max attempts reached. Retry once more";
                error = true;
            }
            else
            {
                attempts++;
            }
        }
        
        if( error )
        {
            //no way to get controller init, close port and start again
            AGO_DEBUG() << "Close serial port";
            closeSerialPort();
        }
        else
        {
            //controller started correctly, stop statement
            break;
        }
    }
    if( error )
    {
        AGO_ERROR() << "Unable to connect to MySensors gateway. Stop now.";
        exit(1);
    }
    AGO_INFO() << "Done.";

    AGO_INFO() << "Requesting gateway version...";
    while( !error )
    {
        //request v1.4 version
        serialPort.WriteString("0;0;3;0;2\n");
        line = readLine(&error);
        AGO_DEBUG() << "Read: " << line;
        if( boost::algorithm::starts_with(line, "0;0;3;0;2;") )
        {
            //response to protocol >=1.4 request
            break;
        }

        if( error )
        {
            break;
        }

        //request v1.3
        serialPort.WriteString("0;0;4;4\n");
        line = readLine(&error);
        AGO_DEBUG() << "Read: " << line;
        if( boost::algorithm::starts_with(line, "0;0;4;4;") )
        {
            //response to protocol 1.3 request
            break;
        }

    }
    if( !error )
    {
        std::vector<std::string> items = split(line, ';');
        if( items.size()>0 )
        {
            //payload (last field, contains protocol version)
            gateway_protocol_version = items[items.size()-1].c_str();

            //check protocol version
            if( !boost::algorithm::starts_with(gateway_protocol_version, "1.4") &&
                !boost::algorithm::starts_with(gateway_protocol_version, "1.3") && 
                !boost::algorithm::starts_with(gateway_protocol_version, "1.5") )
            {
                //unknown protocol version, exit now
                AGO_ERROR() << "Unknown gateway protocol version. Exit. (received \"" << line  << "\" from gateway)";
                exit(1);
            }
            else
            {
                AGO_INFO() << " found v" << gateway_protocol_version;
            }
        }
    }

    //init agocontrol client
    AGO_INFO() << "Initializing MySensors controller";
    agoConnection->addDevice("mysensorscontroller", "mysensorscontroller");
    addCommandHandler();

    //init threads and mutexes
    pthread_mutex_init(&serialMutex, NULL);
    pthread_mutex_init(&devicemapMutex, NULL);
    readThread = new boost::thread(boost::bind(&AgoMySensors::receiveFunction, this));

    //register existing devices
    AGO_INFO() << "Register existing devices:";
    if( ( devicemap.isMember("devices") ) && devicemap["devices"].type() == Json::objectValue )
    {
        std::list<std::string> devicesToPurge;
        Json::Value& devices = devicemap["devices"];
        for (auto it = devices.begin(); it != devices.end(); it++)
        {
            std::stringstream log;
            if( it->type() == Json::objectValue )
            {
                Json::Value infos = *it;
                std::string internalid = it.name();
                log << " - " << internalid << ":" << infos["type"].asString().c_str();
                if( internalid.length()>0 && checkInternalid(internalid) )
                {
                    agoConnection->addDevice(it.name(), (infos["type"].asString()));
                }
                else
                {
                    devicesToPurge.push_back(internalid);
                    log << " [INVALID]";
                }
                AGO_INFO() << log.str();
            }
            else
            {
                AGO_WARNING() << "Invalid entry in device map";
            }
        }

        //purge from config invalid entries
        if( devicesToPurge.size()>0 )
        {
            for( auto it=devicesToPurge.begin(); it!=devicesToPurge.end(); it++ )
            {
                AGO_DEBUG() << "Remove invalid device with internalid '" << (*it) << "' from map config file";
                devices.removeMember(*it);
            }

            //and save config
            writeJsonFile(devicemap, dmf);
        }
    }
    else
    {
        //problem with map file
        AGO_ERROR() << "No device map file available. Exit now.";
        exit(1);
    }

    //run check stale thread
    if( bStale )
    {
        checkStaleThread = new boost::thread(boost::bind(&AgoMySensors::checkStaleFunction, this));
    }

    //run client
    AGO_INFO() << "Running MySensors controller...";
    agoConnection->run();
}

/**
 * Destructor
 */
void AgoMySensors::cleanupApp()
{
    AGO_TRACE() << "Waiting for threads";
    if( readThread )
    {
        readThread->join();
    }
    if( checkStaleThread )
    {
        checkStaleThread->join();
    }

    AGO_TRACE() << "All webserver threads returned";
    delete readThread;
    delete checkStaleThread;

    AGO_TRACE() << "Destroy mutexes";
    pthread_mutex_destroy( &serialMutex );
    pthread_mutex_destroy( &devicemapMutex );
}

AGOAPP_ENTRY_POINT(AgoMySensors);

