cmake_minimum_required (VERSION 3.0)

file (GLOB_RECURSE AGOTIMER_SOURCE_FILES *.cpp)
set (TIMER_LIBRARIES
    agoclient
    ${Boost_DATE_TIME_LIBRARY}
)

# add the executable
add_executable (agotimer ${AGOTIMER_SOURCE_FILES})
target_link_libraries (agotimer ${TIMER_LIBRARIES})

install (TARGETS agotimer RUNTIME DESTINATION ${BINDIR})
AgoService(timer DESCRIPTION "ago control Timer")
