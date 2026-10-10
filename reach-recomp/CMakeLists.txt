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
    src/debug/frame_dump.cpp
    src/debug/gamestate_trace.cpp
    src/debug/texture_trace.cpp
    src/hooks/mouse_look.cpp
    src/hooks/offline_challenges.cpp
    src/hooks/switch_tailcalls.cpp
    src/kernel/critical_section.cpp
    src/kernel/file_delete_on_close.cpp
    src/kernel/file_size_refresh.cpp
    src/kernel/identity.cpp
    src/kernel/input_autopress.cpp
    src/kernel/live_xmsg.cpp
    src/kernel/live_lsp.cpp
    src/kernel/live_tcp.cpp
    src/kernel/net.cpp
    src/kernel/xam_enumerate.cpp
    src/kernel/xam_keyboard.cpp
    src/kernel/xam_signin.cpp
    src/kernel/xecrypt_rsa.cpp
    src/platform/sdk_import.cpp
    src/platform/socket.cpp
    src/input/kbm.cpp
)

if(WIN32)
    add_executable(reach WIN32 ${REACH_SOURCES})
else()
    add_executable(reach ${REACH_SOURCES})
endif()

rexglue_setup_target(reach GPU_PLUGINS xenos)

# The SDK's SPIR-V translator takes the texture-fetch exponent bias from the
# wrong fetch-constant word, which scales Reach's textures by 1/256 (black
# title menu). tools/build_rexglue_sdk.sh builds a patched plugin; this copies
# it over the SDK one that rexglue_setup_target() stages next to the binary.
set(REACH_XENOS_PLUGIN "$ENV{HOME}/rexglue-sdk-patched/0.10.0.24/linux-amd64/lib/librexgpu-xenos.so"
    CACHE FILEPATH "Patched librexgpu-xenos.so to ship instead of the SDK's")
if(UNIX AND NOT APPLE)
    if(EXISTS "${REACH_XENOS_PLUGIN}")
        add_custom_command(TARGET reach POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${REACH_XENOS_PLUGIN}" "$<TARGET_FILE_DIR:reach>/librexgpu-xenos.so"
            VERBATIM)
    else()
        message(WARNING "Patched GPU plugin not found at ${REACH_XENOS_PLUGIN}; "
                        "textures will render dark. Run tools/build_rexglue_sdk.sh.")
    endif()
endif()

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
