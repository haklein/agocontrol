from __future__ import print_function

import agoclient._logging

import argparse

from agoclient.agotransport import AgoTransportConfigError
from . import config
import logging
import os.path
import signal
import sys
from agoclient.agoconnection import AgoConnection
from logging.handlers import SysLogHandler

try:
    import faulthandler
except:
    faulthandler = None

__all__ = ["AgoApp"]

agoclient._logging.init()


class StartupError(Exception):
    pass


class ConfigurationError(Exception):
    pass


class AgoApp:
    """This is a base class for all Python AgoControl applications

    Each application needs to implement a class which extends the AgoApp class.
    Please see devices/example/ for basic usage examples.

    It must then provide the main entry point:

        if __name__ == "__main__":
            TheApp().main()

    """

    def __init__(self):
        self.app_name = self.__class__.__name__
        if self.app_name.find("Ago") == 0:
            self.app_short_name = self.app_name[3:].lower()
        else:
            self.app_short_name = self.app_name.lower()

        self.log = None
        self.exit_signaled = False
        self.connection = None  # type: AgoConnection

    def parse_command_line(self, argv):
        """Parse the provided command line.

        This sets up the default options for all apps. If an application
        wants custom options, override the app_cmd_line_options method.

        Arguments are parsed by the argparse module, and the resulting Namespace
        object is available in self.args.

        Arguments:
            argv -- An array of parameters to parse, not including program name.
        """
        parser = argparse.ArgumentParser(add_help=False)

        LOG_LEVELS = ['TRACE', 'DEBUG', 'INFO', 'WARNING', 'ERROR', 'FATAL']

        parser.add_argument('-h', '--help', action='store_true',
                            help='show this help message and exit')

        parser.add_argument('--log-level', dest="log_level",
                            help='Log level', choices=LOG_LEVELS)
        parser.add_argument('--log-method', dest="log_method",
                            help='Where to log', choices=['console', 'syslog'])

        facilities = list(SysLogHandler.facility_names.keys())
        facilities.sort()
        parser.add_argument('--log-syslog-facility', dest="syslog_facility",
                            help='Which syslog facility to log to.',
                            choices=facilities)

        parser.add_argument('-d', '--debug', action='store_true',
                            help='Shortcut to set console logging with level DEBUG')
        parser.add_argument('-t', '--trace', action='store_true',
                            help='Shortcut to set console logging with level TRACE')

        parser.add_argument('--config-dir', dest="config_dir",
                            help='Directory with configuration files')
        parser.add_argument('--state-dir', dest="state_dir",
                            help='Directory with local state files')

        self.app_cmd_line_options(parser)

        # If parsing fails, this will print error and exit
        args = parser.parse_args(argv[1:])

        if args.config_dir:
            config.set_config_dir(args.config_dir)

        if args.state_dir:
            config.set_localstate_dir(args.state_dir)

        if args.help:
            config.init_directories()

            parser.print_help()

            print()
            print("Paths")
            print("  Default config dir: %s" % _directories.DEFAULT_CONFDIR)
            print("  Default state dir : %s" % _directories.DEFAULT_LOCALSTATEDIR)
            print("  Active config dir : %s" % config.CONFDIR)
            print("  Active state dir  : %s" % config.LOCALSTATEDIR)
            print()
            print("System configuration file      : %s" % config.get_config_path('conf.d/system.conf'))
            print(
                "App-specific configuration file: %s" % config.get_config_path('conf.d/%s.conf' % self.app_short_name))
            print()

            return False

        self.args = args
        return True

    def app_cmd_line_options(self, parser):
        """Override this to add your own command line options

        Arguments:
            parser -- An instance of argparse.ArgumentParser which the implementation should
                      add options to.
        """
        pass

    def setup(self):
        """Execute initial setup.

        This is done after command line arguments have been parsed.
        App specific setup should be done in setup_app method.
        """
        self.setup_logging()
        if not self.setup_connection():
            return False

        self.setup_signals()
        self.setup_app()

        return True

    def cleanup(self):
        """Cleanup after shutdown.

        Note that this may be called even if corresponding setup call
        have not been executed, implementations must ensure that they
        can handle this."""
        if self.log:
            self.log.trace("Cleaning up")
        self.cleanup_app()
        self.cleanup_connection()

    def setup_logging(self):
        root = logging.getLogger()
        # Find log level..
        if self.args.trace:
            lvl_name = "TRACE"
        elif self.args.debug:
            lvl_name = "DEBUG"
        elif self.args.log_level:
            lvl_name = self.args.log_level
        else:
            lvl_name = self.get_config_option("log_level", "INFO",
                                              section=[None, "system"])

            lvl_name = lvl_name.upper()

        lvl = logging.getLevelName(lvl_name)
        try:
            root.setLevel(lvl)
        except ValueError as e:
            raise ConfigurationError("Invalid log_level %s: %s" % (lvl_name, str(e)))

        # Read logging-specific levels from configuration file. The same functionality is present in
        # c++ version with Boost Log channels.
        for logger_name, level in self.get_config_section('loggers', (self.app_short_name, 'system')).items():
            # Our global level controls "maximum output level" rather than "root" level
            # to mimic the way C++ agoapp does. Thus, cap each of the loggers to that level.
            lvl = logging.getLevelName(level)
            level = max(root.level, lvl)
            try:
                logging.getLogger(logger_name).setLevel(level)
            except ValueError as e:
                raise ConfigurationError("Invalid log level for logger %s: %s" % (logger_name, str(e)))

        # Find log method..
        if self.args.trace or self.args.debug:
            log_method = 'console'
        elif self.args.log_method:
            log_method = self.args.log_method
        else:
            log_method = self.get_config_option("log_method", "console",
                                                section=[None, "system"])

            if log_method not in ['console', 'syslog']:
                raise ConfigurationError("Invalid log_method %s" % log_method)

        # ..and set it
        if log_method == 'console':
            self.log_handler = logging.StreamHandler(None)
            self.log_formatter = logging.Formatter("%(asctime)-15s %(name)-10s %(levelname)-5s %(message)s")
        elif log_method == 'syslog':
            if self.args.syslog_facility:
                syslog_fac = self.args.syslog_facility
            else:
                syslog_fac = self.get_config_option("syslog_facility",
                                                    "local0", section=[None, "system"])

                if syslog_fac not in SysLogHandler.facility_names:
                    raise ConfigurationError("Invalid syslog_facility %s" % syslog_fac)

            # Try to autodetect OS syslog unix socket. For example in docker
            # this wont be found.
            syslog_kwargs = dict(facility=SysLogHandler.facility_names[syslog_fac])
            for syslog_path in ["/dev/log", "/var/run/log"]:
                if os.path.exists(syslog_path):
                    syslog_kwargs['address'] = syslog_path
                    break
            else:
                print("Warning: no well-known syslog file found; logging via UDP", file=sys.stderr)

            self.log_handler = SysLogHandler(**syslog_kwargs)
            self.log_formatter = logging.Formatter(self.app_name.lower() + " %(name)-10s %(levelname)-5s %(message)s")

        self.log_handler.setFormatter(self.log_formatter)

        root.addHandler(self.log_handler)
        self.log = logging.getLogger(self.app_name)

    def setup_connection(self):
        """Create an AgoConnection instance, assigned to self.connection"""
        try:
            self.connection = AgoConnection(self.app_short_name)
        except AgoTransportConfigError as e:
            return False

        return self.connection.start()

    def cleanup_connection(self):
        """Shutdown and clean up our AgoConnection instance"""
        if self.connection:
            self.connection.shutdown()
            self.connection = None

    def setup_signals(self):
        """Setup signal handlers"""
        signal.signal(signal.SIGINT, self._sighandler)
        signal.signal(signal.SIGQUIT, self._sighandler)

    def _sighandler(self, signal, frame):
        """Internal method called when catched signals are received"""
        self.log.debug("Exit signal catched, shutting down")
        self.signal_exit()

    def signal_exit(self):
        """Call this to begin shutdown procedures.

        This can be called by the application if it wants to shut down the app.
        """
        self.exit_signaled = True
        self._do_shutdown()

    def is_exit_signaled(self):
        """Check if we have been asked to exit"""
        return self.exit_signaled

    def _do_shutdown(self):
        if self.connection:
            self.connection.prepare_shutdown()

    def setup_app(self):
        """This should be overriden by the application to setup app specifics"""
        pass

    def cleanup_app(self):
        """This should be overriden by the application to cleanup app specifics"""
        pass

    def main(self, argv=None):
        """Main entrypoint, called by the application

        Arguments:
            argv -- Command line arguments. Defaults to sys.argv if not set.
        """
        if not argv:
            argv = sys.argv

        ret = self._main(argv)

        if self.log:
            if ret == 0:
                self.log.info("Exiting %s", self.app_name)
            else:
                self.log.warn("Exiting %s (code %d)", self.app_name, ret)

        sys.exit(ret)

    def _main(self, argv):
        """Internal main function.

        This is where we launch the app. It will:
            - parse command line
            - setup
            - run app main loop [forever]
            - cleanup
            - exit

        Arguments:
            argv -- Command line arguments, including script name in [0].

        Returns:
            OS Exit code

        """

        if faulthandler:
            faulthandler.enable()
            faulthandler.register(signal.SIGINFO)

        if not self.parse_command_line(argv):
            return 1

        try:
            try:
                if not self.setup():
                    return 1
            except StartupError:
                return 1

            except ConfigurationError as e:
                if self.log:
                    self.log.error("Failed to start %s due to configuration error: %s",
                                   self.app_name, e)
                else:
                    # Print to stderr, in case logging setup failed
                    print("Failed to start %s due to configuration error: %s" % (self.app_name, e),
                          file=sys.stderr)
                return 1

            self.log.info("Starting %s", self.app_name)
            ret = self.app_main()
            self.log.debug("Shutting down %s", self.app_name)
            return ret
        except:
            if self.log:
                self.log.critical("Unhandled exception, crashing",
                                  exc_info=True)
            else:
                print("Unhandled exception, crashing", file=sys.stderr)
                raise
            return 1
        finally:
            # Always execute cleanup!
            # For example, if connection.close() is not called, the app will not shutdown
            # properly, even if we have not called connection.run() yet.
            try:
                self.cleanup()
            except:
                if self.log:
                    self.log.error("Unhandled exception while cleaning up", exc_info=True)
                else:
                    print("Unhandled exception, crashing", file=sys.stderr)

    def app_main(self):
        """Main entry point for application.

        By default this calls the run method of the AgoConnection object, which will
        block until shut down.

        This CAN be overriden, but generally it is sufficient to do your setup in
        setup_app.
        """

        self.connection.run()
        return 0

    def get_config_option(self, option, default_value=None, section=None, app=None):
        """Read a config option from the configuration subsystem.

        The system is based on per-app configuration files, which has sections
        and options.

        Note that "option not set" means not set, or empty value!

        Arguments:
            option -- The name of the option to retreive

            default_value -- If the option can not be found in any of the specified
                sections, fall back to this value.

            section -- A string section to look for the option in, or an iterable
                with multiple sections to try in the defined order, in case the option
                was not set.

                If not set, it defaults to the applications short name,
                which is the class name in lower-case, with the leading "Ago" removed.
                (AgoExample becomes example).
                If you want to use the default value, but fall back on other, you
                can use None:

                    section = [None, 'system']

                This will look primarly in the default section, falling back to the
                'system' section.

            app -- A string identifying the configuration storage unit to look in.
                Can also be an iterable with multiple units to look at. If the option
                was not found in any of the sections specified, the next available
                'app' value will be tried.

                If not set, it defaults to the same value as section.
                If the list contains None, this is too replaced by the default section
                value (NOT the value of the passed section argument).


        Returns:
            A unicode object with the value found in the data store, if found.
            If not found, default_value is passed through unmodified.
        """
        if section is None:
            section = self.app_short_name

        if app is None:
            if type(section) == str:
                app = [self.app_short_name, section]
            else:
                app = [self.app_short_name] + section

        config._iterable_replace_none(section, self.app_short_name)
        config._iterable_replace_none(app, self.app_short_name)

        return config.get_config_option(section, option, default_value, app)

    def get_config_section(self, section=None, app=None):
        """Read all options in the given section(s) from the configuration subsystem.

        Same as get_config_option, but will return a dict with all defined values under the given
        section(s). If more than one section/app is defined, all will be looked at, in the order
        specified. If an option is set in multiple sections/app, the last one seen will be used.

        Returns a dict with key/values.
        """
        if section is None:
            section = self.app_short_name

        if app is None:
            if type(section) == str:
                app = [self.app_short_name, section]
            else:
                app = [self.app_short_name] + section

        config._iterable_replace_none(section, self.app_short_name)
        config._iterable_replace_none(app, self.app_short_name)

        return config.get_config_section(section, app)

    def set_config_option(self, option, value, section=None, app=None):
        """Write a config option to the configuration subsystem.

        The system is based on per-app configuration files, which has sections
        and options.

        Arguments:
            option -- The name of the option to set

            value -- The value of the option.

            section -- A string section in which to store the option in.
                If not set, it defaults to the applications short name,
                which is the class name in lower-case, with the leading "Ago" removed.
                (AgoExample becomes example).

            app -- A string identifying the configuration storage unit to store in.
                If omited, it defaults to the same as section.

        Returns:
            True if succesfully stored, False otherwise.
            Please refer to the error log for failure indication.

        """
        if section is None:
            section = self.app_short_name

        return config.set_config_option(section, option, value, app)

    def check_command_param(self, content, param, type=None, empty=False):
        """
        Check if content is containing specified parameters
        @param: parameter name
        @type: if specified (!=None) check parameter type. Converted type is returned instead of raw parameter.
        @empty: check if value is empty (only for string)
        @return res,param: res=True if success/False if something failed. param=parameter or converted parameter
        """
        # check presence
        if param not in content:
            self.log.trace('Parameter "%s" not in %s' % (param, content))
            return False, param

        # check type
        value = content[param]
        if type is not None:
            try:
                if type == 'string':
                    new_value = str(value)
                    # check if str is empty
                    self.log.debug('new_value=%s isinstance=%s len=%d' % (
                        new_value, str(isinstance(new_value, str)), len(new_value)))
                    if empty and isinstance(new_value, str) and len(new_value) == 0:
                        self.log.debug('Parameter "%s" is empty' % param)
                        return False, value
                    else:
                        return True, new_value
                elif type == 'int':
                    return True, int(value)
                elif type == 'float':
                    return True, float(value)
                elif type == 'bool':
                    return True, bool(value)
            except:
                # conversion exception
                self.log.trace('Conversion exception for "%s" [%s] to %s' % (value, type(value), type))
                return False, value
        else:
            # check if str is empty
            if empty and isinstance(value, str) and len(value) == 0:
                self.log.trace('Parameter "%s" is empty' % param)
                return False, value

        return True, param
