# DecompiledHaloReach

A native PC port of **Halo: Reach (Xbox 360)** built by statically recompiling the
original PowerPC executable to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk),
plus the reverse-engineering notes and tooling needed to get it running.

> **Status: playable on Linux, work in progress.** The campaign, Firefight, Forge,
> Theater and custom games run natively with keyboard and mouse or a controller.
> Credits, ranks, the Armory and daily/weekly challenges work without Bungie's servers.
> Online play goes through a self-hostable **Reach Live** server that stands in for
> Xbox LIVE and Bungie's services: friends, invites, System Link over the internet and
> File Share. Matchmaking and Windows builds are not done yet.

This repository contains **no game code and no game data**. You need your own copy of
the game (see [Legal](#legal)).

## Goals

1. Run the Xbox 360 build of Halo: Reach natively on x86-64 Linux, then Windows.
2. Bring back the original **Credits (cR), rank and Armory progression**: earn cR by
   playing, rank up, buy armor with cR. No microtransactions.
3. **Peer-to-peer online play** in place of Xbox Live / system link.
4. **Forge plus file share**: upload and download maps, gametypes and mods.

## How it works

- `default.xex` (Title ID `4D53085B`, base version 1.0, Media ID `566C10D3`) and the
  four Waves audio DLLs it loads are translated function by function into C++ by
  `rexglue codegen`, driven by `reach-recomp/reach_manifest.toml`.
- The ReXGlue runtime (derived from [Xenia](https://github.com/xenia-canary/xenia-canary))
  provides the Xbox 360 kernel/XAM high-level emulation, the Xenos GPU on Vulkan,
  audio and the virtual file system.
- This repo supplies what the generic toolchain can't work out on its own:
  - `reach-recomp/hints/`: function boundaries and entry points the code scanner
    missed, plus statically linked XAPI routines mapped to native SDK versions
    (for example, the game's fiber switching).
  - `reach-recomp/src/`: the app class and native overrides of kernel and XAM exports:
    the virtual network and Reach Live client, Xbox LIVE services (friends, presence,
    sessions, invites, title servers), offline challenges, keyboard and mouse,
    runtime fixes (critical sections, file sizes, enumeration codes), RSA public-key
    crypto, scripted input and frame dumps for automated tests, cross-DLL import thunks.
  - `server/`: Reach Live, the self-hostable online server (standard-library Python).
  - `tools/`: an ISO extractor, entry-point discovery against Ghidra, thunk
    generation, run and debug scripts, and Ghidra repair scripts.
  - `docs/`: findings, the status log and debugging recipes.

## Status

| Area | State |
| --- | --- |
| Codegen of `default.xex` + 4 Waves DLLs | Done (~24k functions, about 167 MB of C++) |
| Native build (Linux, Clang) | Builds and links |
| Boot | Kernel init, threads, cache partitions, fibers, RSA signature checks |
| Intro video | Plays |
| Title screen and start menu | Render correctly with the patched GPU plugin (the SDK bug behind the black menu is described in `docs/menu_black_screen.md`) |
| Campaign (Winter Contingency) | Loads, plays the opening and reaches gameplay; rendering matches Xenia after 19 SDK GPU patches ([`docs/sdk_patches.md`](docs/sdk_patches.md)); real occlusion queries, so the game no longer issues the water draw that hangs the GPU |
| Firefight, custom games, Theater | Play locally; films of past games play back in Theater |
| Progression (cR/rank/Armory) | Works offline: cR earned in play, rank-ups, Armory purchases saved in the profile, Spartan preview in the Armory and post-game screens, daily and weekly challenges picked locally each day, counting kills and paying out on completion ([`docs/progression_re.md`](docs/progression_re.md)); with a Reach Live server, progression is also stored server-side |
| Forge | Works locally: editing on Sword Base, object placement, saving map variants |
| Online | Over the internet through a self-hosted **Reach Live** server: an emulated Xbox LIVE (friends roster, presence, joining a friend, invites) and System Link (the game's browser lists everyone's games). Tested with up to three players on one machine: Firefight and Slayer through the postgame, host migration. Direct peer-to-peer with NAT hole punching, relayed by the server when that fails ([`docs/online_plan.md`](docs/online_plan.md) section 5). Matchmaking (in progress) and Xbox LIVE party are not available |
| File share | Through Reach Live: upload maps and game types from the game's File Share UI, browse a friend's share from the roster and download into Local Files ([`docs/online_plan.md`](docs/online_plan.md) section 5.3) |
| Higher resolution | `resolution_scale = 2` in `reach.toml` (or `--resolution_scale=2`) renders at 2304×1440 instead of 1152×720; menus and Firefight checked, no artifacts seen. Frame rate on a dedicated GPU still to be measured |
| Keyboard and mouse | On by default next to pads: Halo-style bindings (rebindable `kbm_bind_*` cvars) and raw mouse look fed straight into player control, no stick emulation ([`docs/input.md`](docs/input.md)) |

See [`docs/PROJECT.md`](docs/PROJECT.md) for the detailed status log and debugging recipes.

## Requirements

- Linux x86-64 with a Vulkan 1.3 GPU (Windows: the SDK ships a win-amd64 build of the same
  nightly and our code is being made portable; not buildable yet)
- Your own Halo: Reach Xbox 360 disc image (base version, no title update)
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) **0.10.0.24 nightly** (`nightly-20261002-bd833a2a`; v0.10.0 lacks
  atomic/fence fixes). Install prefix, for example `~/rexglue-sdk-nightly/0.10.0.24/linux-amd64`
- Clang (tested with 23), CMake ≥ 3.25, Ninja, Python 3
- Optional, for reverse engineering: Ghidra 12.1.4 with
  [XEXLoaderWV](https://github.com/zeroKilo/XEXLoaderWV) and
  [GhidraMCP](https://github.com/bethington/ghidra-mcp)

## Building

```sh
# 1. Extract the game partition from your ISO (read-only on the ISO)
python3 tools/xdvdfs_extract.py "Halo - Reach.iso" extract extracted/xbox360

# 2. Build the patched Xenos GPU plugin (the stock SDK one renders textures 256x too dark)
tools/build_rexglue_sdk.sh

# 3. Configure (codegen runs automatically as part of the build)
cmake -S reach-recomp -B reach-recomp/out/build/linux-nightly -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64

# 4. Build
ninja -C reach-recomp/out/build/linux-nightly

# 5. Run for N seconds (logs go to /tmp/reach_run.log; the intro is skipped automatically)
tools/run_reach.sh 60
```

Game saves and the emulated cache partitions are stored under `~/.local/share/reach/`.

## Playing online (Reach Live)

Anyone can run a server; it needs Python 3 and one open UDP port:

```sh
python3 server/reach_live_server.py --port 21100 --http-port 21101
```

Players point the game at it, in `reach.toml` next to the executable (or the F4 settings
overlay, "Network/Reach Live"):

```toml
live_server = "your.server.org"   # host[:port], port 21100 by default
gamertag = "Noble Six"             # optional
```

or with `REACH_SERVER=your.server.org tools/run_reach.sh 3600`. The game then signs in to an
emulated Xbox LIVE: everyone on the server is your friend, friends in a game show up in the
lobby roster, X joins them, and "Invite to Party" in a friend's player menu brings them to
you. System Link (Y, "Select Network") lists everyone's System
Link games too. Your gamertag is in `~/.local/share/reach/4D53085B/live_identity.txt`
(created on the first online run). `live_room` keeps a group of players to themselves;
`live_signin = false` stays offline-style (System Link only). Details and the protocol:
[`docs/online_plan.md`](docs/online_plan.md) section 5.

## Repository layout

| Path | What |
| --- | --- |
| `reach-recomp/reach_manifest.toml` | Codegen manifest: main executable and guest DLL modules |
| `reach-recomp/hints/` | Function boundary, entry point and native-replacement (`[rexcrt]`) hints |
| `reach-recomp/src/` | Our runtime code: app setup, kernel overrides, cross-DLL thunks |
| `reach-recomp/generated/` | Codegen output. **Not committed** here (translated game code), except the SDK's `rexglue.cmake`. The maintainer keeps a private copy, synced with `tools/sync_generated_repo.sh` |
| `patches/rexglue-sdk/` | Fixes we carry on top of the ReXGlue SDK (built by `tools/build_rexglue_sdk.sh`) |
| `server/` | Reach Live, the self-hostable online server |
| `tools/` | Extraction, analysis, run and debug tooling (`tools/renderdoc/`: GPU capture analysis) |
| `docs/` | Project log, debugging recipes, reverse-engineering notes |
| `docs/symbols/` | Function names recovered in Ghidra (`address,name` CSV) |

## Legal

This project is not affiliated with, endorsed by or sponsored by Microsoft, Xbox Game
Studios, 343 Industries or Bungie. *Halo* and *Halo: Reach* are trademarks of
Microsoft Corporation.

The repository distributes no copyrighted game material: no executables, no
recompiled game code, no maps, videos or other assets. The build only works with
game files you extract from a copy you legally own.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): recompiler and runtime
- [Xenia](https://github.com/xenia-canary/xenia-canary): the emulator the runtime's
  kernel and GPU layers derive from, and our reference for differential debugging
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp): the pioneering Xbox 360
  static recompiler
- [GhidraMCP](https://github.com/bethington/ghidra-mcp) and
  [XEXLoaderWV](https://github.com/zeroKilo/XEXLoaderWV): reverse-engineering tooling

## License

The code in this repository is released under the [MIT License](LICENSE). This does
not cover the game, its assets, or the third-party SDKs and tools it uses.
