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

- ReXGlue SDK v0.10.0: `~/rexglue-sdk/linux-amd64` (`bin/rexglue`, CMake package under `lib/cmake/rexglue`). Docs: https://github.com/rexglue/rexglue-sdk/wiki
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
