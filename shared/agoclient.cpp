#include <string>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sstream>
#include <assert.h>

#include <boost/bind.hpp>

#include "agoclient.h"
#include "agojson.h"

using namespace qpid::messaging;
using namespace qpid::types;
namespace fs = ::boost::filesystem;

bool agocontrol::nameval(const std::string& in, std::string& name, std::string& value) {
    std::string::size_type i = in.find("=");
    if (i == std::string::npos) {
        name = in;
        return false;
    } else {
        name = in.substr(0, i);
        if (i+1 < in.size()) {
            value = in.substr(i+1);
            return true;
        } else {
            return false;
        }
    }
}

void agocontrol::replaceString(std::string& subject, const std::string& search, const std::string& replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
        subject.replace(pos, search.length(), replace);
        pos += replace.length();
    }
}

/**
 * Split specified string
 */
std::vector<std::string> &agocontrol::split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}
std::vector<std::string> agocontrol::split(const std::string &s, char delimiter) {
    std::vector<std::string> elements;
    split(s, delimiter, elements);
    return elements;
}


std::string agocontrol::int2str(int i) {
    std::stringstream sstream;
    sstream << i;
    return sstream.str();
}

std::string agocontrol::float2str(float f) {
    std::stringstream sstream;
    sstream << f;
    return sstream.str();
}

std::string agocontrol::double2str(double f) {
    std::stringstream sstream;
    sstream << f;
    return sstream.str();
}

// generates a uuid as std::string via libuuid
std::string agocontrol::generateUuid() {
    std::unique_ptr<char> name(new char[38]);
    uuid_t tmpuuid;
    uuid_generate(tmpuuid);
    uuid_unparse(tmpuuid, name.get());
    return std::string(name.get());
}

std::string agocontrol::uint64ToString(uint64_t i) {
    std::stringstream tmp;
    tmp << i;
    return tmp.str();
}

unsigned int agocontrol::stringToUint(const std::string& v)
{
    unsigned int r;
    std::istringstream (v) >> r;
    return r;
}
agocontrol::AgoConnection::AgoConnection(const std::string& interfacename)
    : shutdownSignaled(false)
{
    // TODO: Move to AgoApp
    ::agocontrol::log::log_container::initDefault();

    Variant::Map connectionOptions;
    ConfigNameList cfgfiles = ConfigNameList(interfacename)
        .add("system");
    std::string broker = getConfigSectionOption("system", "broker", "localhost:5672", cfgfiles);
    connectionOptions["username"] = getConfigSectionOption("system", "username", "agocontrol", cfgfiles);
    connectionOptions["password"] = getConfigSectionOption("system", "password", "letmein", cfgfiles);
    connectionOptions["reconnect"] = "true";

    filterCommands = true; // only pass commands for child devices to handler by default
    instance = interfacename;
    uuidMap = Json::Value(Json::ValueType::objectValue);

    uuidMapFile = getConfigPath("uuidmap");
    uuidMapFile /= (interfacename + ".json");
    AGO_DEBUG() << "Using uuidMapFile " << uuidMapFile;
    try {
        ensureParentDirExists(uuidMapFile);
    } catch(const std::exception& error) {
        // exception msg has already been logged
        AGO_FATAL()
            << "Couldn't use configuration dir path: "
            << error.what();
        _exit(1);
    }

    loadUuidMap();

    AGO_DEBUG() << "Configured for broker connection: " << broker;
    connection = Connection(broker, connectionOptions);
    // Must call start() to actually connect
}

void agocontrol::AgoConnection::start() {
    try {
        AGO_DEBUG() << "Opening QPid broker connection";
        connection.open();
        session = connection.createSession();
        sender = session.createSender("agocontrol; {create: always, node: {type: topic}}");
    } catch(const std::exception& error) {
        AGO_FATAL() << "Failed to connect to broker: " << error.what();
        connection.close();
        _exit(1);
    }
}

agocontrol::AgoConnection::~AgoConnection() {
    try {
        if(connection.isOpen()) {
            AGO_DEBUG() << "Closing broker connection";
            connection.close();
        }
    } catch(const std::exception& error) {
        AGO_ERROR() << "Failed to close broker connection: " << error.what();
    }
}


void agocontrol::AgoConnection::run() {
    try {
        receiver = session.createReceiver("agocontrol; {create: always, node: {type: topic}}");
    } catch(const std::exception& error) {
        AGO_FATAL() << "Failed to create broker receiver: " << error.what();
        _exit(1);
    }

    while( !shutdownSignaled ) {
        try{
            Variant::Map content;
            Message message = receiver.fetch(Duration::SECOND * 3);
            session.acknowledge();

            // workaround for bug qpid-3445
            if (message.getContent().size() < 4) {
                throw qpid::messaging::EncodingException("message too small");
            }

            decode(message, content);
            AGO_TRACE() << "Incoming message [src=" << message.getReplyTo() <<
                ", sub="<< message.getSubject()<<"]: " << content;

            if (content["command"] == "discover")
            {
                reportDevices(); // make resolver happy and announce devices on discover request
            }
            else
            {
                if (message.getSubject().size() == 0) {
                    // no subject, this is a command
                    std::string internalid = uuidToInternalId(content["uuid"].asString());
                    // lets see if this is for one of our devices
                    bool isOurDevice = (internalid.size() > 0) && (deviceMap.isMember(internalIdToUuid(internalid)));
                    //  only handle if a command handler is set. In addition it needs to be one of our device when the filter is enabled
                    if ( ( isOurDevice || (!(filterCommands))) && !commandHandler.empty()) {

                        // printf("command for id %s found, calling handler\n", internalid.c_str());
                        if (internalid.size() > 0)
                            content["internalid"] = internalid;

                        // found a match, reply to sender and pass the command to the assigned handler method
                        qpid::types::Variant::Map responsemap;
                        try {
                            responsemap = commandHandler(content);
                        }catch(const AgoCommandException& ex) {
                            responsemap = ex.toResponse();
                        }catch(const std::exception &ex) {
                            AGO_ERROR() << "Unhandled exception in command handler:" << ex.what();
                            responsemap = responseError(RESPONSE_ERR_INTERNAL, "Unhandled exception in command handler");
                        }

                        const Address& replyaddress = message.getReplyTo();
                        // only send a reply if this was for one of our childs
                        // or if it was the special command inventory when the filterCommands was false, that's used by the resolver
                        // to reply to "anonymous" requests not destined to any specific uuid
                        if ((replyaddress && isOurDevice) || (content["command"]=="inventory" && filterCommands==false)) {
                            AGO_TRACE() << "Sending reply " << responsemap;
                            Session replysession = connection.createSession();
                            try {
                                Sender replysender = replysession.createSender(replyaddress);
                                Message response;
                                encode(responsemap, response);
                                response.setSubject(instance);
                                replysender.send(response);
                            } catch(const std::exception& error) {
                                AGO_ERROR() << "Failed to send reply: " << error.what();;
                            }
                            replysession.close();
                        }
                    }
                } else if (!eventHandler.empty()) {
                    eventHandler(message.getSubject(), content);
                }
            }
        } catch(const NoMessageAvailable& error) {

        } catch(const std::exception& error) {
            if(shutdownSignaled)
                break;

            AGO_ERROR() << "Exception in message loop: " << error.what();

            if (session.hasError()) {
                AGO_ERROR() << "Session has error, recreating";
                session.close();
                session = connection.createSession();
                receiver = session.createReceiver("agocontrol; {create: always, node: {type: topic}}");
                sender = session.createSender("agocontrol; {create: always, node: {type: topic}}");
            }

            usleep(50);
        }
    }
    AGO_TRACE() << "Leaving run() message loop";
}

void agocontrol::AgoConnection::shutdown() {
    if(shutdownSignaled) return;
    shutdownSignaled = true;

    if(receiver.isValid()) {
        AGO_DEBUG() << "Closing notification receiver";
        receiver.close();
    }

    if(!session.isValid() && connection.isValid()) {
        AGO_DEBUG() << "Closing pending broker connection";
        // Not yet connected, break out of connection attempt
        // TODO: This does not actually abort on old qpid
        connection.close();
    }
}

/**
 * Report device has been discovered
 */
bool agocontrol::AgoConnection::emitDeviceDiscover(const std::string& internalId, const std::string& deviceType)
{
    Variant::Map content;
    Message event;

    content["devicetype"] = deviceType;
    content["internalid"] = internalId;
    content["handled-by"] = instance;
    content["uuid"] = internalIdToUuid(internalId);
    encode(content, event);
    event.setSubject("event.device.discover");
    try
    {
        sender.send(event);
    }
    catch(const std::exception& error)
    {
        AGO_ERROR() << "Exception in emitDeviceDiscover: " << error.what();
        return false;
    }
    return true;
}

bool agocontrol::AgoConnection::emitDeviceAnnounce(const std::string& internalId, const std::string& deviceType, const std::string& initialName) {
    Variant::Map content;
    Message event;

    content["devicetype"] = deviceType;
    content["internalid"] = internalId;
    content["handled-by"] = instance;
    content["uuid"] = internalIdToUuid(internalId);

    if(!initialName.empty())
        content["initial_name"] = initialName;

    encode(content, event);
    event.setSubject("event.device.announce");
    try {
        sender.send(event);
    } catch(const std::exception& error) {
        AGO_ERROR() << "Exception in emitDeviceAnnounce: " << error.what();
        return false;
    }
    return true;
}

/**
 * Emit stale state
 */
bool agocontrol::AgoConnection::emitDeviceStale(const std::string& uuid, const int stale)
{
    Variant::Map content;
    Message event;

    //content["internalid"] = internalId;
    content["stale"] = stale;
    content["uuid"] = uuid;
    encode(content, event);
    event.setSubject("event.device.stale");
    try
    {
        sender.send(event);
    }
    catch(const std::exception& error)
    {
        AGO_ERROR() << "Exception in emitDeviceStale: " << error.what();
        return false;
    }
    return true;
}

bool agocontrol::AgoConnection::emitDeviceRemove(const std::string& internalId) {
    Variant::Map content;
    Message event;

    content["uuid"] = internalIdToUuid(internalId);
    encode(content, event);
    event.setSubject("event.device.remove");
    try {
        sender.send(event);
    } catch(const std::exception& error) {
        AGO_ERROR() << "Exception in emitDeviceRemove: " << error.what();
        return false;
    }
    return true;
}

bool agocontrol::AgoConnection::addDevice(const std::string& internalId, const std::string& deviceType, bool passuuid) {
    if (!passuuid) return addDevice(internalId, deviceType);
    uuidMap[internalId] = internalId;
    storeUuidMap();
    return addDevice(internalId, deviceType);

}

bool agocontrol::AgoConnection::addDevice(const std::string& internalId, const std::string& deviceType, const std::string& initialName) {
    if (internalIdToUuid(internalId).size()==0) {
        // need to generate new uuid
        uuidMap[generateUuid()] = internalId;
        storeUuidMap();
    }
    Json::Value device(Json::ValueType::objectValue);
    device["devicetype"] = deviceType;
    device["internalid"] = internalId;
    device["stale"] = 0;

    // XXX: This is not read by agoresolver currently
    Json::Value parameters(Json::ValueType::objectValue); //specific parameters map
    device["parameters"] = parameters;

    deviceMap[internalIdToUuid(internalId)] = device;
    emitDeviceAnnounce(internalId, deviceType, initialName);
    return true;
}

bool agocontrol::AgoConnection::removeDevice(const std::string& internalId) {
    const std::string &uuid = internalIdToUuid(internalId);
    if (uuid.size() != 0) {
        emitDeviceRemove(internalId);
        deviceMap.removeMember(uuid);
        // deviceMap[internalIdToUuid(internalId)] = device;
        return true;
    } else return false;
}

/**
 * Suspend device (set stale flag)
 */
bool agocontrol::AgoConnection::suspendDevice(const std::string& internalId)
{
    const std::string &uuid = internalIdToUuid(internalId);
    if( uuid.length() > 0 && deviceMap.isMember(uuid))
    {
        deviceMap[uuid]["stale"] = 1;
        return emitDeviceStale(uuid, 1);
    }
    return false;
}

/**
 * Resume device (reset stale flag)
 */
bool agocontrol::AgoConnection::resumeDevice(const std::string& internalId)
{
    std::string uuid = internalIdToUuid(internalId);
    if(uuid.length() > 0 && deviceMap.isMember(uuid))
    {
        deviceMap[uuid]["stale"] = 0;
        return emitDeviceStale(uuid, 0);
    }
    return false;
}

std::string agocontrol::AgoConnection::uuidToInternalId(const std::string& uuid) {
    if(uuidMap.isMember(uuid))
        return uuidMap[uuid].asString();
    return std::string();
}

std::string agocontrol::AgoConnection::internalIdToUuid(const std::string& internalId) {
    for (auto it = uuidMap.begin(); it != uuidMap.end(); ++it) {
        if (it->asString() == internalId)
            return it.name();
    }
    return std::string();
}

/**
 * Report controller devices after discover request from agoresolver
 */
void agocontrol::AgoConnection::reportDevices()
{
    for (auto it = deviceMap.begin(); it != deviceMap.end(); ++it)
    {
        Json::Value device = *it;
        // do not announce stale devices
        if( device["stale"] == 0 )
        {
            emitDeviceDiscover(device["internalid"].asString(), device["devicetype"].asString());
        }
    }
}

bool agocontrol::AgoConnection::storeUuidMap() {
    writeJsonFile(uuidMap, uuidMapFile);
    return true;
}

bool agocontrol::AgoConnection::loadUuidMap() {
    boost::system::error_code ignore;
    if(!fs::exists(uuidMapFile, ignore) || ignore)
        return false;

    if(readJsonFile(uuidMap, uuidMapFile)) {
        if (uuidMap.type() == Json::ValueType::objectValue)
            return true;

        AGO_ERROR() << "Invalid contents in " << uuidMapFile;
    }

    return false;
}

bool agocontrol::AgoConnection::addHandler(qpid::types::Variant::Map (*handler)(qpid::types::Variant::Map)) {
    addHandler(boost::bind(handler, _1));
    return true;
}

bool agocontrol::AgoConnection::addHandler(boost::function<qpid::types::Variant::Map (qpid::types::Variant::Map)> handler)
{
    commandHandler = handler;
    return true;
}

bool agocontrol::AgoConnection::addEventHandler(void (*handler)(const std::string&, qpid::types::Variant::Map)) {
    addEventHandler(boost::bind(handler, _1, _2));
    return true;
}

bool agocontrol::AgoConnection::addEventHandler(boost::function<void (const std::string&, qpid::types::Variant::Map)> handler)
{
    eventHandler = handler;
    return true;
}


bool agocontrol::AgoConnection::sendMessage(const std::string& subject, qpid::types::Variant::Map content) {
    Message message;

    try {
        encode(content, message);
        message.setSubject(subject);

        AGO_TRACE() << "Sending message [src=" << message.getReplyTo() <<
            ", sub="<< message.getSubject()<<"]: " << content;
        sender.send(message);
    } catch(const std::exception& error) {
        AGO_ERROR() << "Exception in sendMessage: " << error.what();
        return false;
    }

    return true;
}

agocontrol::AgoResponse agocontrol::AgoConnection::sendRequest(const qpid::types::Variant::Map& content) {
    return sendRequest("", content, Duration::SECOND * 3);
}

agocontrol::AgoResponse agocontrol::AgoConnection::sendRequest(const std::string& subject, const qpid::types::Variant::Map& content) {
    return sendRequest("", content, Duration::SECOND * 3);
}

agocontrol::AgoResponse agocontrol::AgoConnection::sendRequest(const std::string& subject, const qpid::types::Variant::Map& content, qpid::messaging::Duration timeout) {
    AgoResponse r;
    Message message;
    Receiver responseReceiver;
    Session recvsession = connection.createSession();

    try {
        encode(content, message);
        if(!subject.empty())
            message.setSubject(subject);

        Address responseQueue("#response-queue; {create:always, delete:always}");
        responseReceiver = recvsession.createReceiver(responseQueue);
        message.setReplyTo(responseQueue);

        AGO_TRACE() << "Sending request [sub=" << subject << ", replyTo=" << responseQueue <<"]" << content;
        sender.send(message);

        Message response = responseReceiver.fetch(timeout);

        try {
            r.init(response);
            AGO_TRACE() << "Remote response received: " << r.response;
        }catch(const std::invalid_argument& ex) {
            AGO_ERROR() << "Failed to initate response, wrong response format? Error: "
                << ex.what()
                << ". Message: " << r.response;

            r.init(responseError(RESPONSE_ERR_INTERNAL, ex.what()));
        }
        recvsession.acknowledge();

    } catch (qpid::messaging::NoMessageAvailable) {
        AGO_WARNING() << "No reply for message sent to subject " << subject;

        r.init(responseError(RESPONSE_ERR_NO_REPLY, "Timeout"));
    } catch(const std::exception& ex) {
        AGO_ERROR() << "Exception in sendRequest: " << ex.what();

        r.init(responseError(RESPONSE_ERR_INTERNAL, ex.what()));
    }

    recvsession.close();
    return r;
}

bool agocontrol::AgoConnection::sendMessage(qpid::types::Variant::Map content) {
    return sendMessage("",content);
}

bool agocontrol::AgoConnection::emitEvent(const std::string& internalId, const std::string& eventType, const std::string& level, const std::string& unit) {
    Variant::Map content;
    std::string _level = level;
    Variant value;
    value.parse(level);
    content["level"] = value;
    content["unit"] = unit;
    content["uuid"] = internalIdToUuid(internalId);
    return sendMessage(eventType, content);
}
bool agocontrol::AgoConnection::emitEvent(const std::string& internalId, const std::string& eventType, double level, const std::string& unit) {
    Variant::Map content;
    content["level"] = level;
    content["unit"] = unit;
    content["uuid"] = internalIdToUuid(internalId);
    return sendMessage(eventType, content);
}
bool agocontrol::AgoConnection::emitEvent(const std::string& internalId, const std::string& eventType, int level, const std::string& unit) {
    Variant::Map content;
    content["level"] = level;
    content["unit"] = unit;
    content["uuid"] = internalIdToUuid(internalId);
    return sendMessage(eventType, content);
}

bool agocontrol::AgoConnection::emitEvent(const std::string& internalId, const std::string& eventType, qpid::types::Variant::Map _content) {
    Variant::Map content;
    content = _content;
    content["uuid"] = internalIdToUuid(internalId);
    return sendMessage(eventType, content);
}

std::string agocontrol::AgoConnection::getDeviceType(const std::string& internalId) {
    std::string uuid = internalIdToUuid(internalId);
    if (uuid.size() > 0 && deviceMap.isMember(uuid)) {
        Json::Value device = deviceMap[uuid];
        return device["devicetype"].asString();
    }
    return std::string();
}

/**
 * Return device stale state
 */
int agocontrol::AgoConnection::isDeviceStale(const std::string& internalId)
{
    std::string uuid = internalIdToUuid(internalId);
    if (uuid.size() > 0)
    {
        if(deviceMap.isMember(uuid))
        {
            Json::Value device = deviceMap[uuid];
            return device["stale"].asInt();
        }
        else
        {
            AGO_WARNING() << "internalid '" << internalId << "' doesn't exist in deviceMap";
            return 0;
        }
    }
    else
    {
        return 0;
    }
}

bool agocontrol::AgoConnection::setFilter(bool filter) {
    filterCommands = filter;
    return filterCommands;
}

qpid::types::Variant::Map agocontrol::AgoConnection::getInventory() {
    Variant::Map content;
    content["command"] = "inventory";
    AgoResponse r = sendRequest(content);

    if(r.isOk()) {
        AGO_TRACE() << "Inventory obtained";
        return r.getData();
    }else{
        AGO_WARNING() << "Failed to obtain inventory: " << r.response;
    }

    // TODO: Some way to report error?
    return qpid::types::Variant::Map();
}

std::string agocontrol::AgoConnection::getAgocontroller() {
    std::string agocontroller;
    int retry = 10;
    while(agocontroller=="" && retry-- > 0) {
        qpid::types::Variant::Map inventory = getInventory();
        if (!(inventory["devices"].isVoid())) {
            qpid::types::Variant::Map devices = inventory["devices"].asMap();
            qpid::types::Variant::Map::const_iterator it;
            for (it = devices.begin(); it != devices.end(); it++) {
                if (!(it->second.isVoid())) {
                    qpid::types::Variant::Map device = it->second.asMap();
                    if (device["devicetype"] == "agocontroller") {
                        AGO_DEBUG() << "Found Agocontroller: " << it->first;
                        agocontroller = it->first;
                    }
                }
            }
        }

        if (agocontroller == "" && retry) {
            AGO_WARNING() << "Unable to resolve agocontroller, retrying";
            sleep(1);
        }
    }

    if (agocontroller == "")
        AGO_WARNING() << "Failed to resolve agocontroller, giving up";

    return agocontroller;
}

bool agocontrol::AgoConnection::setGlobalVariable(const std::string& variable, const qpid::types::Variant& value) {
    Variant::Map setvariable;
    std::string agocontroller = getAgocontroller();
    if (agocontroller != "") {
        setvariable["uuid"] = agocontroller;
        setvariable["command"] = "setvariable";
        setvariable["variable"] = variable;
        setvariable["value"] = value;
        return sendMessage("", setvariable);
    }
    return false;
}

