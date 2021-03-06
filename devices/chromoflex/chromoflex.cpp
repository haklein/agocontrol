/*
   Copyright (C) 2009 Harald Klein <hari@vt100.at>

   This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License.
   This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

   See the GNU General Public License for more details.

*/

#include <iostream>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include <termios.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#include "agoapp.h"

using namespace agocontrol;

class AgoChromoflex: public AgoApp {
private:
    int fd; // file desc for device
    unsigned short   usp_crc; // initialise per packet with $FFFF.
    int increment;
    int speed;

    void setupApp();
    void cleanupApp();
    Json::Value commandHandler(const Json::Value& content);

    void process_crc(unsigned char ucData);
public:
    AGOAPP_CONSTRUCTOR(AgoChromoflex);
};

void AgoChromoflex::process_crc(unsigned char ucData) {
    int i;
    usp_crc^=ucData;
    for(i=0;i<8;i++){ // Process each Bit
        if(usp_crc&1){ usp_crc >>=1; usp_crc^=0xA001;}
        else{          usp_crc >>=1; }
    }

}

Json::Value AgoChromoflex::commandHandler(const Json::Value& content) {
    Json::Value returnval;
    int red = 0;
    int green = 0;
    int blue = 0;
    unsigned char buf[1024];

    checkMsgParameter(content, "command", Json::stringValue);
    std::string command = content["command"].asString();

    int level = 0;
    if (command == "on" ) {
        red = 255; green = 255; blue=255;
    } else if (command == "off") {
        red = 0; green = 0; blue=0;
    } else if (command == "setlevel") {
        checkMsgParameter(content, "level", Json::uintValue);
        level = content["level"].asInt();
        red = green = blue = (int) ( 255.0 * level / 100 );
    } else if (command == "setcolor") {
        checkMsgParameter(content, "red", Json::uintValue);
        checkMsgParameter(content, "green", Json::uintValue);
        checkMsgParameter(content, "blue", Json::uintValue);

        red = content["red"].asUInt();
        green = content["green"].asUInt();
        blue = content["blue"].asUInt();
    }

    // assemble frame
    buf[0]=0xca; // preamble
    buf[1]=0x00; // broadcast
    buf[2]=0x00; // broadcast
    buf[3]=0x00; // broadcast
    buf[4]=0x00; // length 
    buf[5]=0x08; // length
    buf[6]=0x7e; // 7e == effect color
    buf[7]=0x04; // register addr
    buf[8]=red; // R
    buf[9]=green; // G
    buf[10]=blue; // B
    buf[11]=0x00; // X
    buf[12]=increment; // reg 8 - red increment
    buf[13]=increment; // reg 9 - green increment
    buf[14]=increment; // reg 10 - blue increment

    // calc crc16
    usp_crc = 0xffff;
    for (int i = 0; i < 15; i++) process_crc(buf[i]);

    buf[15] = (usp_crc >> 8);
    buf[16] = (usp_crc & 0xff);

    // TODO: also emit event
    AGO_TRACE() << "sending command";
    if (write (fd, buf, 17) != 17) {
        AGO_ERROR() <<  "Write error: " << strerror(errno);
        return responseError(RESPONSE_ERR_INTERNAL, "Cannot write to serial port");
    }
    return responseSuccess();
}


void AgoChromoflex::setupApp() {
    std::string devicefile=getConfigOption("device", "/dev/ttyS_01");

    fd = open(devicefile.c_str(), O_RDWR);
    unsigned char buf[1024];

    increment=1;
    speed=1;

    // init crc
    usp_crc = 0xffff;

    // disable any programs on the units
    buf[0]=0xca; // preamble
    buf[1]=0x00; // broadcast
    buf[2]=0x00; // broadcast
    buf[3]=0x00; // broadcast
    buf[4]=0x00; // length 
    buf[5]=0x02; // length
    buf[6]=0x7e; // 7e == write register
    buf[7]=18; // register addr
    buf[8]=0x01; // disable internal programs
    for (int i = 0; i < 9; i++) process_crc(buf[i]);
    buf[9] = (usp_crc >> 8);
    buf[10] = (usp_crc & 0xff);

    // setup B9600 8N1 first
    struct termios tio;
    tcgetattr(fd, &tio);
    tio.c_cflag = B9600 | CS8 | CLOCAL | CREAD;
    tcflush(fd, TCIFLUSH);
    tcsetattr(fd,TCSANOW,&tio);

    if (write (fd, buf, 11) != 11) {
        AGO_FATAL() << "cannot open device:" << devicefile << " - error: " << fd;
        throw StartupError();
    }

    agoConnection->addDevice("0", "dimerrgb");
    addCommandHandler();
}

void AgoChromoflex::cleanupApp() {
    close(fd);
}

AGOAPP_ENTRY_POINT(AgoChromoflex)
