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

`experimental/0014` (tessellated triangle strips/fans as lists) is not applied; see its
README.

Audit method (worth repeating for the rest of the GPU code): normalize SDK and Xenia
sources (strip comments/whitespace, `xe::`/`rex::`), diff statement by statement, and look
for near-identical statements with one token changed; then list Xenia commits after the
fork point that touch the same files and check which the SDK has. RenderDoc scripts for
confirming a suspect on a live capture are in `tools/renderdoc/`.

Open GPU issues in the campaign (Winter Contingency):

- Red/green speckled patches on terrain and foliage, changing per frame. The combine pass
  samples a `k_2_10_10_10_AS_16_16_16_16` texture at 0x02354000 that, in our command
  stream, only ever receives an 8888 resolve of the gamma albedo target. Not fixed by any
  patch above. Next step: compare the resolve sequence with Xenia on the same scene (Xenia
  needs a profile to start the campaign; `tools/virtual_pad.py` can drive it).
- About 4 indexed, discrete-tessellated triangle strip draws per frame are dropped
  ("Unsupported tessellation mode 0 for primitive type 6"); Xenia 82d0cd1 drops them too.
- GPU hang (amdgpu `ring gfx timeout`, `VK_ERROR_DEVICE_LOST`) about 5 minutes into the
  campaign, during the Falcon flight; the kernel recovers the GPU.
