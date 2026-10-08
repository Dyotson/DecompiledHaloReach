# reach - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(reach LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(REACH_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(reach WIN32 ${REACH_SOURCES})
else()
    add_executable(reach ${REACH_SOURCES})
endif()

rexglue_setup_target(reach)
