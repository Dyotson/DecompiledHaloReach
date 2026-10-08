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

rexglue_setup_target(reach GPU_PLUGINS xenos)

# The SDK runtime dlopen()s guest-DLL modules by bare target name on Linux
# (no "lib" prefix, no ".so" suffix), so emit the libraries under that name.
if(UNIX AND NOT APPLE)
    foreach(_module reach_waveShell-Xbox reach_wavesLibDLL reach_L360 reach_Q10)
        if(TARGET ${_module})
            set_target_properties(${_module} PROPERTIES PREFIX "" SUFFIX "")
        endif()
    endforeach()
endif()

# Guest-DLL -> guest-DLL import thunks (codegen only implements kernel/XAM ones).
# Regenerate with tools/gen_xdll_thunks.py after re-running codegen.
if(TARGET reach_L360)
    target_sources(reach_L360 PRIVATE src/xdll/l360_thunks.cpp)
endif()
if(TARGET reach_Q10)
    target_sources(reach_Q10 PRIVATE src/xdll/q10_thunks.cpp)
endif()
