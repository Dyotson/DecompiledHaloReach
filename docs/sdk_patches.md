# ReXGlue SDK patches we carry

The ReXGlue SDK's Xenos GPU emulation is a port of Xenia from around early December 2025
(nightly 0.10.0.24, commit `bd833a2`). Upstream does not accept Xenos GPU changes
(rexglue/rexglue-sdk#456), so fixes live in `patches/rexglue-sdk/` and
`tools/build_rexglue_sdk.sh` builds a patched `librexgpu-xenos.so` from them (see
`docs/PROJECT.md` > Tooling). Most of them are Xenia fixes the SDK never got; a few are
porting slips. Every patch says "as in Xenia" in its comments and was checked against
Xenia Canary `82d0cd1f4`, the build we use as the reference.

| Patch | Area | What was wrong | Visible effect in Reach |
| --- | --- | --- | --- |
| 0001 | SPIR-V tfetch | Result exponent bias read from fetch-constant word 4 (LOD bias bits) instead of word 3 | Every texture 256x too dark: black title menu. **Fixed.** |
| 0002 | SPIR-V tfetch | Stacked-texture layer lerp started from the second layer (porting slip) | none seen |
| 0003 | SPIR-V tfetch | Missing clamp of the stacked-texture layer index (later Xenia fix) | none seen |
| 0004 | Vulkan RT cache | `k_2_10_10_10` render targets hosted as 8-bit `A8B8G8R8` (Xenia dc66d67a31 moved to `A2B10G10R10`) | lighting precision |
| 0005 | Vulkan shared memory | Resolve (compute) writes declared as `SHADER_READ` in the next barrier (Xenia a716dc6fac) | possible stale resolve data |
| 0006 | RT ownership transfer | 8888 <-> GAMMA conversion direction inverted (porting slip when gamma-as-UNORM16 was ported) | wrong colors when Reach aliases its gamma albedo target |
| 0007 | Vulkan texture cache | No barrier between two uploads into the same texture (Xenia 1fdbe569e4, c8f214e2a9) | possible mixed texels |
| 0008 | Texture cache | Texture marked up to date even if its memory was invalidated while loading (Xenia 9781a75a22) | possible stale textures |
| 0009 | Shader translator | Scalar ALU operand ignored the Z component when paired with a 3-source vector op (Xenia 92ada8ebc0) | wrong shader math |
| 0011 | Resolve clears | 64bpp clear value halves swapped (Xenia 16e1eb8e28) | 16_16_16_16 / 32_32 clears in wrong colors |
| 0013 | Draw filtering | Draws with `kill_pix_post_hi_z` but no VIZ query were rasterized (Xenia draw_util) | proxy geometry drawn into targets |
| 0016 | Vulkan tessellation | The adaptive tessellation vertex shader converted the edge factor's raw bits to float (`float(value)`) instead of reinterpreting them (`uintBitsToFloat`), so every edge got the maximum factor; 16-in-32 index endianness also applied an 8-in-16 swap (Xenia `tessellation_adaptive.vs`) | needed for water; the GPU hang persists (see below) |
| 0015 | Vulkan RT transfer | Raw gamma bytes decoded to the lower edge of their linear range before being stored in the R16G16B16A16_UNORM gamma target; UNORM16 rounding then re-encodes some bytes one lower (Xenia decodes to the midpoint, `GammaByteToLinearMidpoint`) | red/green speckled terrain, foliage and stars in the campaign. **Fixed.** |

`experimental/0014` (tessellated triangle strips/fans as lists) is not applied; see its
README.

Audit method (worth repeating for the rest of the GPU code): normalize SDK and Xenia
sources (strip comments/whitespace, `xe::`/`rex::`), diff statement by statement, and look
for near-identical statements with one token changed; then list Xenia commits after the
fork point that touch the same files and check which the SDK has. RenderDoc scripts for
confirming a suspect on a live capture are in `tools/renderdoc/`.

## The red terrain (fixed by 0015)

Reach draws its alpha-tested and forward-lit surfaces (foliage, terrain detail, stars) into
the light buffer as `k_2_10_10_10_FLOAT` (7e3, three exponent and seven mantissa bits per
channel), re-aliases those EDRAM tiles as `k_8_8_8_8_GAMMA` and then as `k_8_8_8_8`, and
resolves them to 0x02354000. A full-screen "combine" pass reads that memory as
`k_2_10_10_10_AS_16_16_16_16` with an exponent bias of +3 and decodes the 7e3 bits by hand
(`floor`/`fract`/`exp2`). The bits only survive if every host-side conversion along the way is
exact. Our gamma render targets are stored as linear R16G16B16A16_UNORM: the transfer decoded
each gamma byte to the exact lower edge of its linear range (e.g. 72/1023), UNORM16 storage
rounded some of those just below the edge, and the truncating re-encode in the EDRAM dump
returned the byte minus one. Because the 10-bit fields straddle bytes, one LSB in byte 1 moved
the top bits of red from 68 to 835, which the combine decodes as 2^6: bright red. Xenia
decodes to the midpoint of the range, which survives the round trip; 0015 ports that.

Found by capturing the same cinematic shot in Xenia (profile copied into its content folder,
virtual pad, `trigger_capture.py`) and in our build (`REACH_RDCAPTURE_TRIGGER`), then
`resolves.py` (identical resolve sequences), `pick.py` on the combine draw's inputs at matching
foliage pixels (Xenia: small 7e3 values; ours: R field 835), `phist.py` to follow one pixel's
bytes back to the ownership transfer, and recomputing the expected bytes by hand.

Wrong turns worth knowing about: the combine pass writes garbage-red for every pixel that
does not hold 7e3 data in Xenia too; later material draws (blending off) overwrite it, so a
red combine output alone is not a bug. Xenia 82d0cd1 converts only auto-indexed tessellated
triangle strips/fans to lists; indexed ones still hit `default: return false` there, so the
dropped indexed strips are not a difference between the two.

Open GPU issues in the campaign (Winter Contingency):

- Faint cyan speckles on the near ground in the Falcon landing shot (Xenia: plain dirt).
  Probably another gamma/7e3 round trip; the same-layout 8888 <-> gamma transfer still decodes
  to the lower edge (as Xenia does).
- About 4 indexed, discrete-tessellated triangle strip draws per frame are dropped
  ("Unsupported tessellation mode 0 for primitive type 6"); Xenia 82d0cd1 drops indexed ones
  too (it converts auto-indexed strips/fans only, see `experimental/0014`).
- GPU hang (amdgpu `ring gfx timeout`, `VK_ERROR_DEVICE_LOST`) about 4 minutes into the
  campaign as the Falcons take off. `RADV_DEBUG=hang` (report under `$HOME/radv_dumps_*`)
  points at the first adaptive-tessellation draw (`kTrianglePatch`, 5142 float32 edge factors,
  8-in-32, `VGT_HOS_MAX_TESS_LEVEL` 15; probably water): the very first one hangs, and
  skipping those draws avoids the hang (the run then reaches gameplay). 0016 fixed the factor
  decoding but not the hang; hull shaders and domain-shader register setup match Xenia, so the
  translated domain or pixel shader is the next suspect.
- The SDK's D3D12 backend needs the same midpoint decode (Xenia has it in
  `d3d12_render_target_cache.cc`) before Windows builds.
