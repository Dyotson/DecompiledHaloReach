# Cross-compile for Windows x64 from Linux with clang-cl and lld-link, against the MSVC
# CRT and Windows SDK that `xwin splat` unpacks (by default to ~/.local/opt/xwin/sdk;
# running xwin means accepting Microsoft's license). See tools/build_windows.sh.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(XWIN_DIR "$ENV{HOME}/.local/opt/xwin/sdk" CACHE PATH "xwin splat output")
set(LLVM_BIN_DIR "/home/linuxbrew/.linuxbrew/opt/llvm/bin" CACHE PATH "LLVM tools (clang-cl)")
set(LLD_BIN_DIR "/home/linuxbrew/.linuxbrew/opt/lld/bin" CACHE PATH "lld-link")

set(CMAKE_C_COMPILER "${LLVM_BIN_DIR}/clang-cl")
set(CMAKE_CXX_COMPILER "${LLVM_BIN_DIR}/clang-cl")
set(CMAKE_LINKER "${LLD_BIN_DIR}/lld-link")
set(CMAKE_AR "${LLVM_BIN_DIR}/llvm-lib")
set(CMAKE_RC_COMPILER "${LLVM_BIN_DIR}/llvm-rc")
# No llvm-mt here: lld-link embeds the manifest itself.
set(CMAKE_MT "")

set(_xwin_includes
    "${XWIN_DIR}/crt/include"
    "${XWIN_DIR}/sdk/include/ucrt"
    "${XWIN_DIR}/sdk/include/um"
    "${XWIN_DIR}/sdk/include/shared"
    "${XWIN_DIR}/sdk/include/winrt")
# SSE4.1 as on Linux (the SDK's flags give it to GNU-style drivers only): the generated
# code rounds with roundevenf, which the MSVC CRT lacks; SSE4.1 inlines it.
set(_xwin_flags "--target=x86_64-pc-windows-msvc -fuse-ld=lld-link /clang:-msse4.1")
foreach(_dir ${_xwin_includes})
    string(APPEND _xwin_flags " /imsvc \"${_dir}\"")
endforeach()
# Mixed-case names some sources use (ObjBase.h); tools/build_windows.sh makes the symlinks.
string(APPEND _xwin_flags " /imsvc \"${XWIN_DIR}/../casefix\"")
set(CMAKE_C_FLAGS_INIT "${_xwin_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_xwin_flags}")

set(_xwin_libs
    "/libpath:${XWIN_DIR}/crt/lib/x86_64"
    "/libpath:${XWIN_DIR}/sdk/lib/um/x86_64"
    "/libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64"
    "/manifest:embed")
string(JOIN " " _xwin_link_flags ${_xwin_libs})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_link_flags}")

# xwin unpacks the release CRT only (no msvcrtd.lib).
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
