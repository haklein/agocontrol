cmake_minimum_required (VERSION 3.0)

set_property( DIRECTORY
    APPEND PROPERTY
    COMPILE_OPTIONS
    -Werror)

OPTION(BUILD_GATEWAY_ImperiHome "Build ImperiHome gateway" ON)
if (BUILD_GATEWAY_ImperiHome)
  add_subdirectory (ImperiHome)
endif()

OPTION(BUILD_GATEWAY_meloware "Build Meloware Lightswitchserver Gateway" ON)
if (BUILD_GATEWAY_meloware)
    AgoPythonProgram(meloware DESCRIPTION "ago control Meloware Lightswitchserver Gateway")
endif()

