# Like windows-clang-cl.cmake, but with the GNU-style clang/clang++ driver targeting the
# MSVC ABI, which is how the ReXGlue SDK builds on Windows (its presets use clang++):
# tools/build_windows.sh builds the patched SDK (runtime and GPU plugin) with it.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(XWIN_DIR "$ENV{HOME}/.local/opt/xwin/sdk" CACHE PATH "xwin splat output")
set(LLVM_BIN_DIR "/home/linuxbrew/.linuxbrew/opt/llvm/bin" CACHE PATH "LLVM tools")
set(LLD_BIN_DIR "/home/linuxbrew/.linuxbrew/opt/lld/bin" CACHE PATH "lld-link")

set(CMAKE_C_COMPILER "${LLVM_BIN_DIR}/clang")
set(CMAKE_CXX_COMPILER "${LLVM_BIN_DIR}/clang++")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER "${LLD_BIN_DIR}/lld-link")
# The GNU-style driver archives the ar way (qc + ranlib).
set(CMAKE_AR "${LLVM_BIN_DIR}/llvm-ar")
set(CMAKE_RANLIB "${LLVM_BIN_DIR}/llvm-ranlib")
set(CMAKE_RC_COMPILER "${LLVM_BIN_DIR}/llvm-rc")
set(CMAKE_MT "")

# The MSVC headers go after clang's own (-idirafter, as clang-cl's /imsvc does): MSVC's
# emmintrin.h and friends declare the SSE intrinsics as external functions, which then
# fail to link.
set(_flags "-fuse-ld=lld-link -B${LLD_BIN_DIR}")
foreach(_dir crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt)
    string(APPEND _flags " -idirafter \"${XWIN_DIR}/${_dir}\"")
endforeach()
# Mixed-case names some sources use (ObjBase.h) for headers the SDK ships in lower case;
# tools/build_windows.sh makes the symlinks.
string(APPEND _flags " -idirafter \"${XWIN_DIR}/../casefix\"")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
set(_link "-L${XWIN_DIR}/crt/lib/x86_64 -L${XWIN_DIR}/sdk/lib/um/x86_64 -L${XWIN_DIR}/sdk/lib/ucrt/x86_64 -Wl,/manifest:embed")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_link}")

set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
