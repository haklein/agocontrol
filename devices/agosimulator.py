#! /usr/bin/env python

import random
import threading
import time
import logging

import agoclient
from agoclient import agoproto

class AgoSimulator(agoclient.AgoApp):
    def message_handler(self, internalid, content):
        if "command" in content:
            if content["command"] == "on":
                print("switching on: " + internalid)
                self.connection.emit_event(internalid, "event.device.statechanged", 255, "")
            elif content["command"] == "off":
                print("switching off: " + internalid)
                self.connection.emit_event(internalid, "event.device.statechanged", 0, "")
            elif content["command"] == "push":
                print("push button: " + internalid)
            elif content['command'] == 'setlevel':
                if 'level' in content:
                    print("device level changed", content["level"])
                    self.connection.emit_event(internalid, "event.device.statechanged", content["level"], "")
            else:
                return agoproto.response_unknown_command()

            return agoproto.response_success()
        else:
            return agoproto.response_bad_parameters()


    def app_cmd_line_options(self, parser):
        """App-specific command line options"""
        parser.add_argument('-i', '--interval', type=float,
                default = 5,
                help="How many seconds (int/float) to wait between sent messages")


    def setup_app(self):
        self.connection.add_handler(self.message_handler)

        self.connection.add_device("123", "dimmer", "simulated dimmer")
        self.connection.add_device("124", "switch", "simulated switch")
        self.connection.add_device("125", "binarysensor", "simulated binsensor")
        self.connection.add_device("126", "multilevelsensor", "simulated multisensor")
        self.connection.add_device("127", "pushbutton", "simulated pushbutton")

        self.log.info("Starting test thread")
        self.background = TestEvent(self, self.args.interval)
        self.background.connection = self.connection
        self.background.setDaemon(True)
        self.background.start()

        inventory = self.connection.get_inventory()
        self.log.info("Inventory has %d entries", len(inventory))

        ctrl = self.connection.get_agocontroller()
        self.log.info("Agocontroller is at %s", ctrl)

    def cleanup_app(self):
        # Unfortunately, there is no good way to wakeup the python sleep().
        # In this particular case, we can just let it die. Since it's a daemon thread,
        # it will.

        #self.background.join()
        pass


class TestEvent(threading.Thread):
    def __init__(self, app, interval):
        threading.Thread.__init__(self)
        self.app = app
        self.interval = interval

    def run(self):
        level = 0
        counter = 0
        log = logging.getLogger('SimulatorTestThread')
        while not self.app.is_exit_signaled():
            counter = counter + 1
            if counter > 3:
                counter = 0
                temp = random.randint(50,300) / 10 + random.randint(0,90)/100.0
                hum = random.randint(20, 75) + random.randint(0,90)/100.0
                log.debug("Sending enviromnet changes on sensor 126 (%.2f dgr C, %.2f %% humidity)",
                        temp, hum)
                self.app.connection.emit_event("126", "event.environment.temperaturechanged", temp, "degC")
                self.app.connection.emit_event("126", "event.environment.humiditychanged", hum, "percent")

            log.debug("Sending sensortriggered for internal-ID 125, level %d", level)
            self.app.connection.emit_event("125", "event.security.sensortriggered", level, "")

            if (level == 0):
                level = 255
            else:
                level = 0

            log.trace("Next command in %f seconds...", self.interval)
            time.sleep(self.interval)

if __name__ == "__main__":
    AgoSimulator().main()
