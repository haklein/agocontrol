cmake_minimum_required (VERSION 3.0)

file (GLOB_RECURSE DEVICE_SOURCE_FILES *.cpp
      ../webcam/base64.cpp)

set (DEVICE_EXTRA_LIBRARIES
    ${LIBCURL_LIBRARIES}
    agoclient
)

set (DEVICE "agozmcam")

# add the executable
add_executable (${DEVICE} ${DEVICE_SOURCE_FILES})
target_link_libraries (${DEVICE} ${DEVICE_EXTRA_LIBRARIES})

install (TARGETS ${DEVICE} RUNTIME DESTINATION ${BINDIR})

AgoService(zmcam DESCRIPTION "ago control zoneminder cam device")
