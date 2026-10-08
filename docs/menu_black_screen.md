# Investigation: title menu renders black

Status as of 2026-10-08. Symptom: after the intro video (or after skipping it with START), the
game reaches the title menu (Xenia shows "REACH", START SOLO CAMPAIGN / MAIN MENU, Bungie logo
at ~55 s), but the recompiled build presents an almost entirely black frame.

## Facts established

- **The game is in the menu, not stuck.** After the storage device selector it reads campaign
  `.mapinfo` files, writes the profile (TITLE_SPECIFIC1-3), polls DLC content every ~230 ms, and
  the GPU runs Reach's full frame every frame: deferred scene, HDR float resolves, bloom
  downsample chain (288x184 → 72x48 → 24x16), an 8x8 luminance resolve, then the final
  composite into a 1152x720 `k_8_8_8_8` front buffer that is presented.
- **The presented image itself is dark** (dumped straight from guest memory with
  `REACH_FRAMEDUMP` + `--vulkan_readback_resolve=true`): pixel values 0-2 of 255 plus dither
  noise. Amplified, it shows one large perspective-skewed quad (where Xenia shows the title art)
  in exact black over dithered background. So the presenter/gamma ramp is not at fault.
- **Texture memory has data.** The bitmaps the menu binds (e.g. 1280x512 and 1024x1024 DXT5,
  696x73) have valid fetch headers and their pixel memory is 50-97 % non-zero. The resource
  streaming path works: the fiber-based LZX decoder (`Lzx_DecompressChunkOnFiber` 0x82602C28)
  returns success with status 0, and `cache000-002.map` are byte-identical to the disc maps
  apart from a 28-byte header.
- **Invalid texture fetches**: ~2 per frame (Xenia: none). They come from texture stages the
  shader samples but nobody bound. Every bind goes through `Rasterizer_SetTextureCached`
  (0x8216AD90) → `D3DDevice_SetTexture` (0x8216B0C8); there are no invalid-header binds, no
  binds of "not resident" (0) handles, and no desync between the engine's per-stage cache
  (0x82A8F938) and the D3D device after the per-frame `D3DDevice_ResetTextureStages`.
- **Shader set differs from Xenia** (compare `cache/shaders/shareable/4D53085B.xsh` stores
  with `tools/xsh_fetches.py`, same boot with no input): Xenia uses 80 shaders, we use those
  80 plus 2 VS/PS pairs. The extra pixel shaders `550395F60470A758` and `7A7C2F62C5C88F99` are
  final-composite variants that write **two** color targets (oC0 + oC1) and conditionally
  sample **slot 6** (`tf0` HDR scene, `tf2` bloom, `tf6` extra, `tf7` dither noise; colour
  grading via c129-c131). Xenia's composite variants (`01483764D7EFCC81`, `BA15FDD2732C203D`)
  sample only slots 0/2/7 and write oC0. Slot 6 is never bound in our build, which matches the
  invalid-fetch count. The DOF globals (`Dof_InitializeGlobals` 0x82256758, three render-state
  copies in physical memory) show DOF disabled, so slot 6 is probably not depth-of-field.

## Ruled out

Intro/input waits (scripted presses change nothing), async pipeline compilation
(`--async_shader_compilation=false`), CPU exposure readback (`--vulkan_readback_resolve`,
`--vulkan_readback_memexport`), physical memory exhaustion (273 allocations, same as Xenia),
XexGetModuleSection resources (all succeed), corrupted cache maps, the SDK version (0.10.0 and
the 0.10.0.24 nightly behave the same), the `bdz` tail-call codegen bug (fixed, no change).

## Leading hypotheses

1. **Engine picks the wrong final-composite permutation.** Something the engine reads (a
   global flag, a profile setting, a screen-effect/UI state, or a float comparison computed by
   recompiled code) differs from real hardware, so it uses the two-output variant that expects
   a texture on slot 6 that this path never provides. Next: find the code that selects the
   composite's explicit shader/entry point (the pixel shader blob sits in a `0x102A1100`
   shader container in physical memory; trace who references that container or hook the D3D
   pixel-shader setter and log the caller when the microcode matches).
2. **GPU texture cache staleness.** If the GPU uploaded menu textures before the CPU finished
   writing them and the physical-memory write watch did not invalidate them, the GPU samples
   stale zeros although guest memory is correct. Next: trace-level log
   (`--log_level=trace --log_max_file_size_mb=4000`) and correlate `Loaded tiled ... base at`
   lines with the bases bound by `REACH_TEXTRACE_VALID=1`; check the SDK's `NtReadFile` and host
   writes into watched physical pages.

## Differential-debugging setup

- Xenia Canary reference: `~/xenia-canary/82d0cd1/xenia_canary_linux.AppImage --mount_cache=true
  --gpu=vulkan --store_shaders=true <iso>` (stores under `~/.local/share/Xenia`; portable mode is
  ignored inside the AppImage). Its guest memory is also mapped at host `0x100000000`, so
  `tools/guestmem.py` works on a running Xenia too, which allows comparing engine state.
- RenderDoc 1.46 in `~/renderdoc/renderdoc_1.46` with a fixed layer manifest in
  `~/renderdoc/layer`; run under `renderdoccmd capture` with `VK_ADD_IMPLICIT_LAYER_PATH=~/renderdoc/layer
  ENABLE_VULKAN_RENDERDOC_CAPTURE=1 REACH_RDCAPTURE=<secs>`. The layer loads, but the triggered
  capture does not produce a file yet (presenter/device mismatch is the likely cause).
