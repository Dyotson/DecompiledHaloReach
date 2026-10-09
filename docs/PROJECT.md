# Halo Reach PC — native static recompilation of the Xbox 360 build

Goal: run Halo: Reach (Xbox 360, Title ID `4D53085B`, base version 1.0 / no TU,
Media ID `566C10D3`) natively on x86-64 Linux (and later Windows) by statically
recompiling `default.xex` (PowerPC Xenon) to C++ with the ReXGlue SDK, then add:

- the original Credits (cR) / rank / Armory progression (no microtransactions),
- peer-to-peer online play (replacing Xbox Live / system link),
- Forge plus file share (upload/download maps, gametypes, mods).

## Layout (all paths relative to `/home/dyotson/Documents/Halo Reach PC`)

| Path | What |
| --- | --- |
| `Halo - Reach (...).iso` | Original disc image. READ ONLY. Never modify or delete. |
| `extracted/xbox360/` | Game partition extracted from the ISO (`default.xex`, `maps/*.map`, ...). Treat as read only. |
| `tools/xdvdfs_extract.py` | Read-only XDVDFS extractor. |
| `tools/start_ghidra_headless_mcp.sh` | Starts the headless GhidraMCP server (already running in tmux session `ghidra-headless`). |
| `ghidra_projects/HaloReach` | Ghidra project: `/xbox360/default.xex` (PPC BE 64, 32-bit addr) and `/mcc/haloreach.dll` (MCC PC build of Reach, x86-64). Both auto-analyzed. |
| `reach-recomp/` | ReXGlue project. `reach_manifest.toml` drives codegen; `generated/` is codegen output (do not hand-edit); `src/` is our runtime code. |
| `docs/` | Design docs and findings. |

MCC install (READ ONLY): `/home/dyotson/.local/share/Steam/steamapps/common/Halo The Master Chief Collection/haloreach/`.

## Tooling

- ReXGlue SDK: build against the **0.10.0.24 nightly** at `~/rexglue-sdk-nightly/0.10.0.24/linux-amd64`
  (fixes `stwcx.` atomics, sync fences and VMX pack aliasing; regenerate with its `bin/rexglue`).
  Build dir `reach-recomp/out/build/linux-nightly`. v0.10.0 is still at `~/rexglue-sdk/linux-amd64`
  (do not build `linux-release` any more: its codegen step would overwrite `generated/` with 0.10.0 output). (`bin/rexglue`, CMake package under `lib/cmake/rexglue`). Docs: https://github.com/rexglue/rexglue-sdk/wiki
- **Patched GPU plugin (required):** the SDK's Vulkan shader translator scales every texture
  sample by 2^(bits of the LOD bias) (rexglue-sdk#456, closed upstream as "not accepting Xenos GPU
  edits"). `tools/build_rexglue_sdk.sh` builds `librexgpu-xenos.so` from the nightly's commit
  `bd833a2` with `patches/rexglue-sdk/*.patch` into the overlay prefix
  `~/rexglue-sdk-patched/0.10.0.24/linux-amd64` (source checkout `~/rexglue-sdk-src/sdk`); the reach
  build copies it next to the binary after each link (`REACH_XENOS_PLUGIN`). Building the SDK on this
  immutable host uses Homebrew's X11/XCB/Wayland headers (see the script). Upstream will not take GPU
  fixes, so further Xenos fixes go into `patches/rexglue-sdk/` too.
- Clang 23 / CMake / Ninja from Homebrew: `/home/linuxbrew/.linuxbrew/bin`.
- JDK 21: `/home/linuxbrew/.linuxbrew/opt/openjdk@21/libexec`.
- Build: `cmake -S reach-recomp -B reach-recomp/out/build/linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/home/linuxbrew/.linuxbrew/bin/clang++ -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk/linux-amd64` then `ninja -C reach-recomp/out/build/linux-release`.

### Ghidra MCP HTTP API

The headless GhidraMCP server listens on `http://127.0.0.1:8089`. Every endpoint takes
`program=/xbox360/default.xex` or `program=/mcc/haloreach.dll`. Endpoint catalog with
params: `~/ghidra-mcp/tests/endpoints.json`. Useful ones:

- `GET /decompile_function?address=0x82....&program=...`
- `POST /disassemble_bytes` JSON `{start_address,end_address,include_instructions:true,program}`
- `GET /get_xrefs_to?address=...&program=...`
- `GET /search_strings?...`, `GET /list_imports`, `GET /list_exports`, `GET /search_functions_by_name`
- `POST /rename_function`, `POST /set_decompiler_comment` (annotate as you learn things)
- `GET /save_program?program=...` after edits

Check exact param names in `endpoints.json` before calling.

## Hard rules

- Never delete, overwrite, or modify the ISO, the zip, `extracted/`, or anything in the Steam/MCC install.
- Never run `rm`, `find -delete`, etc. on user files. Scratch files go in `/tmp`.
- Don't kill the tmux sessions `ghidra-headless`, `ghidra-gui`, `reach-build`.
- Commit your own work in git (`git -c user.name=agent -c user.email=agent@local commit`), docs in `docs/`.

## Status log

- 2026-10-08: ISO extracted. Both binaries imported + analyzed in Ghidra. ReXGlue codegen of
  `default.xex` succeeds (325 files, 167 MB C++) with one manual function hint; warnings in
  `reach-recomp/codegen-warnings.log`. First native build in progress.
- 2026-10-08 (later): `reach` + the four Waves audio DLLs build and boot (GPU plugin xenos,
  cache0/cache1 mounts, 375+ missed entry points pinned via `hints/`). Boot then died in an
  endless null write on the ASYNC_0 thread (`sub_82602C28`): the statically linked XAPI fiber
  code (`SwitchToFiber` = 0x8295DC48) swaps guest GPRs and "returns" into another fiber, which
  recompiled code cannot do. Fixed by mapping the five XAPI fiber routines to the SDK's native
  `rexcrt_*` fibers (`hints/rexcrt.toml`). Implemented `XeCryptBnQwNeRsaPubCrypt`
  (`src/kernel/xecrypt_rsa.cpp`; the SDK stubs it on Linux, the game verifies RSA-2048
  signatures with it). The game now plays the intro Bink video, then shows the (auto-answered)
  storage device selector and writes profile settings; after that the screen stays black.

- 2026-10-08 (evening): moved to the ReXGlue 0.10.0.24 nightly (atomic `stwcx.`, fences, VMX
  pack fix). Fixed a codegen bug where `bdz`/`bdzf` switch-chain tail calls were emitted as bare
  `return`s (`hints/switch_tailcalls.toml` + `src/hooks/switch_tailcalls.cpp`). Ghidra analysis
  repaired (no-return cascade, ~1.6k merged fragments); function names exported to
  `docs/symbols/default_xex_functions.csv`. Credits/rank/Armory mapped in `docs/progression_re.md`.
  Open blocker: the title menu renders black; see `docs/menu_black_screen.md`.

- 2026-10-09: **title menu fixed.** Root cause was an SDK bug, not game state: the SPIR-V
  translator read the texture-fetch `exp_adjust` from fetch-constant word 4 instead of word 3, so with
  Reach's LOD bias every texture sample came back 1/256 as bright (`docs/menu_black_screen.md`).
  Patched plugin built from source (`tools/build_rexglue_sdk.sh`). Title screen and start menu
  (Armory, player card with rank/cR) render. Also fixed: directories marked delete-on-close are now
  deleted (`cache1:\webcache`), and `REACH_NO_SIGNIN=1` mimics Xenia's no-profile setup.

- 2026-10-09 (later): **campaign starts.** START, A on START SOLO CAMPAIGN, A on Normal loads
  Winter Contingency: the loading cinematic, the in-engine opening cinematic (Warthog, Falcon, Noble Team
  briefing) and Noble Six at the base render. Open issue: terrain/foliage show saturated red/green
  patches that change every frame. Traced to the lighting buffer texture (`k_2_10_10_10_AS_16_16_16_16`
  at 0x02354000) that an EDRAM resolve fills; it contains garbage in our build. Two more SDK porting bugs
  found by diffing against Xenia and patched (stacked-texture layer lerp, missing layer clamp), but they
  are not the red-patch cause. Patch 0004: the SDK hosted `k_2_10_10_10` render targets (Reach's HDR
  lighting) as 8-bit `A8B8G8R8` instead of Xenia's `A2B10G10R10`; fixed, red patches remain.
- Xenia can now be driven in-game with `tools/virtual_pad.py` (virtual Xbox 360 pad over /dev/uinput),
  but its campaign needs a Xenia profile (Profile > Create Profile, once). Don't automate the mouse
  for that: ydotool's absolute moves land at (0,0) on this KDE session and hit other windows.
  `renderdoccmd capture` under `timeout` leaves the captured child running; kill it afterwards.

- 2026-10-09 (night): **checkpoint revert crash fixed.** ~4.5 min into the campaign the game reverted
  to a checkpoint and died with "Call to invalid or unregistered function at 0x82429440": a
  post-revert callback (table 0x82A39BF0) is a real function placed right after a call to the
  no-return fatal-error handler, so codegen merged it into its predecessor. `find_missing_entries.py`
  now also accepts data-referenced addresses after a `bl` (and skips `.reloc`);
  `hints/indirect_entries_2.toml` adds 18 such entries. `REACH_GSTRACE=1`
  (`src/debug/gamestate_trace.cpp`) traces game state save/load/verify. Nine more SDK GPU patches
  (0005-0013, from an audit against Xenia; see `docs/sdk_patches.md`). Open: red terrain patches,
  dropped tessellated strips, GPU hang ~5 min in. `REACH_AUTOPRESS` now drives sticks and triggers
  (`300:LSUP:4`, `RT`).

- 2026-10-09 (morning): **red terrain fixed** (SDK patch 0015). The red/green speckle on terrain,
  foliage and stars was a precision loss in the 7e3 -> 8_8_8_8_GAMMA -> 8_8_8_8 EDRAM round trip
  Reach uses for its forward-lit surfaces; one gamma byte came back one lower and the combine
  pass's hand-written 7e3 decode turned that into 2^6 red. Xenia decodes gamma bytes to the
  midpoint of their linear range; ported (`docs/sdk_patches.md` has the full trace). Found by
  capturing the same cinematic shot in both: Xenia uses a profile copied from the user's EmuDeck
  Xenia (`content/E03000003463994D` into `~/.local/share/Xenia/content` plus
  `logged_profile_slot_0_xuid` in its config), driven with `tools/virtual_pad.py -` (reads one
  input per line from stdin, so menus can be stepped while watching screenshots). New RenderDoc
  scripts: `resolves.py`, `phist.py`, `pick.py`, `blend.py`, `savetex.py`, `bufdump.py`.
  Open: faint cyan speckles on near ground in the Falcon landing, dropped indexed tessellated
  strips, GPU hang ~4-5 min in.
- Scripted runs need every SDL pad ignored: with a pad connected (including Steam Input's and
  Sunshine's virtual ones) the scripted presses never reach the campaign. `run_reach.sh` sets
  `SDL_*_IGNORE_DEVICES_EXCEPT` when `REACH_AUTOPRESS` is set; Xenia needs
  `SDL_GAMECONTROLLER_IGNORE_DEVICES=<vid/pid,...>` for the same reason, or the virtual pad
  becomes player 2 (unsigned profile).

- 2026-10-09 (midday): **gameplay reached** (with adaptive tessellation draws skipped by a local
  debug switch). The next crash, "Call to invalid or unregistered function at 0x82853FB0" ~6 min
  in, was a function only reachable through a callback table the game fills at run time
  (`sub_82874A20` indexes it by type), so no static reference exists. Found the caller with gdb
  (`break rex::runtime::InvalidFunctionTrap`, then `bt`). `tools/find_orphan_functions.py`
  lists code no generated function covers and nothing static reaches; `hints/indirect_entries_3.toml`
  registers 138 such functions (11 with an explicit `end`). Seven CRT entries in
  `indirect_entries_2.toml` that split functions are pruned again. Open: the GPU hang on the
  first adaptive-tessellation draw (patch 0016 is part of it), black 3D world in most gameplay
  frames (HUD fine), cyan tint on foliage.

## Debugging recipes

- Run: `tools/run_reach.sh <secs> [flags]` (logs `/tmp/reach_run.log`, rotates at 5 MB). It skips
  the intro by pressing START at 9 s (`REACH_AUTOPRESS`, the title screen is up by ~15 s) and removes
  orphaned `/dev/shm/xenia_memory_*` (5 GB each; leaked ones fill /dev/shm and every later run dies
  with SIGBUS).
- GPU differential debugging with RenderDoc (how the black menu was solved): capture ours with
  `REACH_RDCAPTURE=<secs>` under `renderdoccmd capture` (layer setup in `docs/menu_black_screen.md`),
  capture Xenia with `tools/renderdoc/trigger_capture.py`, then run the `tools/renderdoc/*.py`
  scripts with `qrenderdoc --python` to match draws, diff constants/textures/interpolators, and
  swap in instrumented SPIR-V (`instrument.py`) to see intermediate register values.
  Load times vary by tens of seconds between runs (more under RenderDoc), so fixed capture times
  miss shots: set `REACH_RDCAPTURE_TRIGGER=<file>` and touch that file to capture (the file is
  polled every 10 swaps and deleted), e.g. from a loop that converts periodic `REACH_FRAMEDUMP`s
  and checks them. `/tmp` is RAM-backed here; delete captures you are done with.
- Frame dumps without screenshots: `REACH_FRAMEDUMP=20,28` + `--vulkan_readback_resolve=true`, then
  `tools/frame_to_png.py`. Texture binding trace: `REACH_TEXTRACE=1`. Live guest memory: `tools/guestmem.py`.
  `--gpu_allow_invalid_fetch_constants=true` silences thousands of fetch-constant warnings.
- Find the guest function behind an "Unhandled guest access violation": run under gdb with
  `catch signal SIGSEGV` conditioned on `$_siginfo._sifields._sigfault.si_addr` (guest address
  `X` is host `0x100000000 + X`); the backtrace shows `__imp__sub_XXXXXXXX` frames with
  generated source lines. Other SIGSEGVs are normal (MMIO / GPU write watches), so pass them.
- Stall diagnosis: `gdb -p $(pgrep -x reach) -batch -ex 'thread apply all bt 30'`; guest
  frames appear as `sub_XXXXXXXX`.
- Screenshots: `spectacle -b -n -a -o shot.png` while the game window is focused.
- Kernel imports can be overridden from `src/` by defining `extern "C" REX_FUNC(__imp__Name)`
  (generated code calls `__imp__Name` directly; the executable's definition wins).
- Shader stores (ours: `~/.local/share/reach/cache/shaders/shareable/4D53085B.xsh`, Xenia:
  `~/.local/share/Xenia/cache_host/...`): `tools/xsh_fetches.py STORE [HASH..]` lists texture/vertex
  fetch slots per shader; `--dump_shaders=<dir>` writes microcode disassembly.
- Ghidra: `tools/ghidra_scripts/ReachFixSaveRestHelpers.java` repairs prologue-truncated
  functions (run via `/run_script_inline`; the headless server must be started with
  `GHIDRA_MCP_ALLOW_SCRIPTS=1`). Decompile endpoint is `/force_decompile`.
