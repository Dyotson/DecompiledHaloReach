#!/usr/bin/env bash
# Cross-compile the Windows version from Linux and package it in
# reach-recomp/out/dist/windows:
#   1. the ReXGlue SDK (runtime and Xenos GPU plugin) from source, with this project's
#      patches and the Vulkan backend (the SDK's prebuilt Windows plugin has none), and
#   2. the game against it, reusing the generated code of the Linux build.
#
# Usage: tools/build_windows.sh
#
# Needs, besides what the Linux build needs (run it first: it runs codegen, and
# tools/build_rexglue_sdk.sh checks out and patches the SDK source):
#   - Homebrew's llvm (clang, clang-cl, llvm-lib, llvm-rc) and lld (lld-link);
#   - the MSVC CRT and Windows SDK, unpacked by xwin (https://github.com/Jake-Shadle/xwin).
#     Running it means accepting Microsoft's license:
#       xwin --accept-license splat --output ~/.local/opt/xwin/sdk
#
# Environment: XWIN_DIR (default ~/.local/opt/xwin/sdk), REXSDK_SRC (default
# ~/rexglue-sdk-src/sdk), REXSDK_WIN_PREFIX (where the Windows SDK build is installed,
# default ~/rexglue-sdk-patched/0.10.0.24/win-amd64).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XWIN="${XWIN_DIR:-$HOME/.local/opt/xwin/sdk}"
SRC="${REXSDK_SRC:-$HOME/rexglue-sdk-src/sdk}"
PREFIX="${REXSDK_WIN_PREFIX:-$HOME/rexglue-sdk-patched/0.10.0.24/win-amd64}"
JOBS="${JOBS:-$(nproc)}"

[ -d "$XWIN/crt/include" ] || { echo "no MSVC CRT at $XWIN (run xwin splat, see above)" >&2; exit 1; }
[ -d "$SRC/.git" ] || { echo "no SDK source at $SRC (run tools/build_rexglue_sdk.sh first)" >&2; exit 1; }
[ -f "$ROOT/reach-recomp/generated/default/sources.cmake" ] ||
    { echo "no generated code (build the Linux version first)" >&2; exit 1; }

# The SDK's sources include some Windows headers in mixed case (ObjBase.h, SDKDDKVer.h);
# the Windows SDK ships them in lower case and Linux file systems are case-sensitive.
# The toolchain files search this directory last.
python3 - "$XWIN" "$SRC" <<'EOF'
import os, re, sys
xwin, src = sys.argv[1], sys.argv[2]
dirs = [os.path.join(xwin, d) for d in ("crt/include", "sdk/include/ucrt", "sdk/include/um",
                                        "sdk/include/shared", "sdk/include/winrt")]
index = {}
for d in dirs:
    for root, _, files in os.walk(d):
        for f in files:
            index.setdefault(os.path.relpath(os.path.join(root, f), d).lower(), os.path.join(root, f))
names = set()
for top in ("src", "include", "thirdparty"):
    for root, _, files in os.walk(os.path.join(src, top)):
        for f in files:
            if f.endswith((".h", ".hpp", ".cpp", ".c", ".cc", ".inl")):
                text = open(os.path.join(root, f), errors="ignore").read()
                names.update(re.findall(r'#\s*include\s*<([A-Za-z0-9_./]+)>', text))
casefix = os.path.join(xwin, "..", "casefix")
for name in names:
    if name == name.lower() or any(os.path.exists(os.path.join(d, name)) for d in dirs):
        continue
    target = index.get(name.lower())
    link = os.path.join(casefix, name)
    if target and not os.path.lexists(link):
        os.makedirs(os.path.dirname(link), exist_ok=True)
        os.symlink(target, link)
EOF

# simde (an SDK submodule) stores an MMX value with a builtin that clang >= 23 rejects on
# MSVC's union __m64; take its plain store there instead.
python3 - "$SRC/thirdparty/simde/simde/x86/sse.h" <<'EOF'
import sys
path = sys.argv[1]
text = open(path).read()
old = "#elif HEDLEY_HAS_BUILTIN(__builtin_nontemporal_store) && ( \\\n      defined(SIMDE_ARM_NEON_A32V7_NATIVE) || defined(SIMDE_MIPS_LOONGSON_MMI_NATIVE)"
new = "#elif HEDLEY_HAS_BUILTIN(__builtin_nontemporal_store) && !defined(SIMDE_X86_MMX_USE_NATIVE_TYPE) && ( \\\n      defined(SIMDE_ARM_NEON_A32V7_NATIVE) || defined(SIMDE_MIPS_LOONGSON_MMI_NATIVE)"
if old in text:
    open(path, "w").write(text.replace(old, new, 1))
EOF

# 1. The SDK for Windows: runtime, Xenos plugin (D3D12 and Vulkan), codegen tool.
cmake -S "$SRC" -B "$SRC/out/build/win-cross" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=23 -DREXGLUE_USE_VULKAN=ON -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/cmake/windows-clang.cmake" > /dev/null
ninja -C "$SRC/out/build/win-cross" -j "$JOBS"
cmake --install "$SRC/out/build/win-cross" > /dev/null

# 2. The game.
BUILD="$ROOT/reach-recomp/out/build/windows"
cmake -S "$ROOT/reach-recomp" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/cmake/windows-clang-cl.cmake" \
    -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
    -Drexglue_DIR="$PREFIX/lib/cmake/rexglue" > /dev/null
ninja -C "$BUILD" -j "$JOBS"

# 3. Package: the game, its guest DLLs and the SDK's runtime DLLs.
DIST="$ROOT/reach-recomp/out/dist/windows"
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$BUILD"/reach.exe "$BUILD"/reach_*.dll "$DIST/"
for dll in "$PREFIX"/bin/*.dll; do
    case "$(basename "$dll")" in
        *d.dll | *rd.dll) ;;  # debug and RelWithDebInfo variants
        *) cp "$dll" "$DIST/" ;;
    esac
done
cat > "$DIST/README.txt" <<'EOF'
Halo: Reach (Xbox 360) recompiled for Windows - https://github.com/Dyotson/RecompiledHaloReach

Needs the Microsoft Visual C++ 2015-2022 x64 redistributable and a Vulkan 1.3 GPU driver.
Run from this folder, pointing at the game files extracted from your own disc:

    reach.exe --game_data_root=C:\path\to\extracted\xbox360

Settings go in reach.toml next to reach.exe (or the F4 overlay), e.g.
    resolution_scale = 2
    live_server = "reach.example.org"
Saves and the Reach Live identity are in Documents\reach.
EOF
echo "Windows build: $DIST"
