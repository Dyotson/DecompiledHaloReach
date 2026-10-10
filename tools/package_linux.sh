#!/usr/bin/env bash
# Package the Linux build in reach-recomp/out/dist/linux so it runs without the SDK or a
# build tree: the game and its guest-DLL modules, the SDK runtime, the patched Xenos GPU
# plugin, a launcher that takes the extracted game files, and the ISO extractor.
#
# Usage: tools/package_linux.sh
#
# Environment: REACH_BUILD (build dir under reach-recomp/out/build, default linux-nightly),
# REXSDK (SDK prefix; default the patched overlay that tools/build_rexglue_sdk.sh makes),
# REXSDK_SRC (SDK source, for its license; default ~/rexglue-sdk-src/sdk),
# DIST (output dir, default reach-recomp/out/dist/linux).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/reach-recomp/out/build/${REACH_BUILD:-linux-nightly}"
SDK="${REXSDK:-$HOME/rexglue-sdk-patched/0.10.0.24/linux-amd64}"
SRC="${REXSDK_SRC:-$HOME/rexglue-sdk-src/sdk}"
DIST="${DIST:-$ROOT/reach-recomp/out/dist/linux}"
MODULES=(reach_waveShell-Xbox reach_wavesLibDLL reach_L360 reach_Q10)

[ -x "$BUILD/reach" ] || { echo "no build at $BUILD (see README: Building)" >&2; exit 1; }
[ -f "$SDK/lib/librexruntime.so" ] || { echo "no SDK at $SDK" >&2; exit 1; }
# The overlay's plugin is the only real file in its lib/ (the rest link into the nightly);
# the stock one renders the game too dark.
[ -f "$SDK/lib/librexgpu-xenos.so" ] && [ ! -L "$SDK/lib/librexgpu-xenos.so" ] ||
    { echo "no patched GPU plugin in $SDK (run tools/build_rexglue_sdk.sh)" >&2; exit 1; }

rm -rf "$DIST"
mkdir -p "$DIST/licenses"
# One flat directory, as in the build tree: reach finds librexruntime.so through its
# $ORIGIN runpath, and the runtime dlopen()s the plugin and the modules from there.
cp "$BUILD/reach" "$DIST/"
for module in "${MODULES[@]}"; do cp "$BUILD/$module" "$DIST/"; done
cp -L "$SDK/lib/librexruntime.so" "$SDK/lib/librexgpu-xenos.so" "$DIST/"
# The plugin is copied out of the SDK's build tree, whose runpath points into that tree
# (and ends in an empty entry, which means the current directory).
if command -v patchelf > /dev/null; then
    patchelf --set-rpath '$ORIGIN' "$DIST/librexgpu-xenos.so"
else
    echo "warning: patchelf not found; librexgpu-xenos.so keeps its build-tree runpath" >&2
fi
cp "$ROOT/tools/xdvdfs_extract.py" "$DIST/"
cp "$ROOT/LICENSE" "$DIST/licenses/RecompiledHaloReach.txt"
[ -f "$SRC/LICENSE" ] && cp "$SRC/LICENSE" "$DIST/licenses/rexglue-sdk.txt"
[ -f "$SDK/share/licenses/SDL3/LICENSE.txt" ] && cp "$SDK/share/licenses/SDL3/LICENSE.txt" "$DIST/licenses/SDL3.txt"

cat > "$DIST/play.sh" <<'EOF'
#!/usr/bin/env bash
# Start the game with the files extracted from your own disc (see README.txt):
#   ./play.sh /path/to/extracted/xbox360 [options]    e.g. --resolution_scale=2
# The path is remembered in game_data_root.txt, so later runs need only ./play.sh.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
SAVED="$DIR/game_data_root.txt"
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    DATA="$1"; shift
elif [ -n "${REACH_DATA:-}" ]; then
    DATA="$REACH_DATA"
elif [ -f "$SAVED" ]; then
    DATA="$(cat "$SAVED")"
else
    echo "usage: $0 /path/to/extracted/xbox360 [options] (README.txt explains the extraction)" >&2
    exit 1
fi
if ! abs="$(cd "$DATA" 2>/dev/null && pwd)" || [ ! -f "$abs/default.xex" ]; then
    echo "no default.xex in $DATA: point at the extracted game files (README.txt)" >&2
    exit 1
fi
DATA="$abs"
printf '%s\n' "$DATA" > "$SAVED" 2>/dev/null || true
LOG_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/reach"
mkdir -p "$LOG_DIR"
# The game rejects an option given twice.
log=(--log_file="$LOG_DIR/reach.log")
for arg in "$@"; do case "$arg" in --log_file | --log_file=*) log=() ;; esac; done
cd "$DIR"
LD_LIBRARY_PATH="$DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ./reach --game_data_root="$DATA" \
    "${log[@]}" "$@"
status=$?
# Guest memory (/dev/shm/xenia_memory_*, 5 GB) is not unlinked when the game is killed.
for shm in /dev/shm/xenia_memory_*; do
    [ -O "$shm" ] || continue
    grep -qs "$(basename "$shm")" /proc/[0-9]*/maps || rm -f -- "$shm"
done
exit $status
EOF
chmod +x "$DIST/play.sh"

commit="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
# The newest glibc and libstdc++ symbol versions the shipped binaries use: the oldest
# system they run on (the SDK nightly is built on Ubuntu 22.04: glibc 2.35, GCC 13).
newest() { (cd "$DIST" && objdump -T reach librexruntime.so librexgpu-xenos.so "${MODULES[@]}") |
           grep -o "$1[0-9.]*[0-9]" | sort -V | tail -1; }
glibc="$(newest GLIBC_2. | cut -d_ -f2)"
glibcxx="$(newest GLIBCXX_3.)"
distros=""
[ "${glibcxx##*.}" -le 32 ] && distros="
  (GCC 13.2 or later: Ubuntu 24.04, Debian 13, Fedora 39, Arch or newer)"
cat > "$DIST/README.txt" <<EOF
Halo: Reach (Xbox 360) recompiled for Linux - https://github.com/Dyotson/RecompiledHaloReach
Built from commit $commit.

Needs:
- x86-64 Linux with glibc $glibc or later and a libstdc++ with $glibcxx$distros;
- a Vulkan 1.3 GPU driver (tested on AMD with Mesa RADV), the X11 and Wayland client
  libraries and about 8 GB of free RAM;
- your own copy of Halo: Reach for Xbox 360 (disc image of the base version, no title
  update). This package contains no game files.

1. Extract the game files from your disc image (Python 3; the image is only read):

       python3 xdvdfs_extract.py "/path/to/Halo - Reach.iso" extract ~/HaloReach/xbox360

2. Play, pointing at the extracted files (remembered for the next time):

       ./play.sh ~/HaloReach/xbox360
       ./play.sh --resolution_scale=2      # render at 2304x1440 instead of 1152x720

Keyboard and mouse work next to any controller: WASD, mouse look, left click fire, right
click zoom, Space jump, Ctrl crouch, Q melee, G grenade, E/R reload and action, Shift
armor ability, 1/2 or the wheel to switch weapons, Tab scoreboard, Esc menu. Esc skips the
intro video; in menus Enter is A and Backspace is B.

Settings go in reach.toml next to play.sh (or the F4 overlay), one per line, e.g.
    resolution_scale = 2
    live_server = "reach.example.org"
Saves and the log (reach.log) are in ~/.local/share/reach.

If the game dies with SIGBUS at start, /dev/shm is full of guest memory left by killed
games: delete the /dev/shm/xenia_memory_* files no running game uses.
EOF
echo "Linux package: $DIST"

# Every library the game needs must come from this directory or the system.
missing="$(cd "$DIST" && LD_LIBRARY_PATH="$DIST" ldd reach librexgpu-xenos.so "${MODULES[@]}" | grep 'not found' || true)"
[ -z "$missing" ] || { echo "unresolved libraries:" >&2; echo "$missing" >&2; exit 1; }
