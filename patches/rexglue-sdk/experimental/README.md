# Experimental SDK patches

Not applied by `tools/build_rexglue_sdk.sh` (it only takes `patches/rexglue-sdk/*.patch`).

- `0014-tessellated-triangle-strips-and-fans.patch`: Reach's campaign issues indexed
  triangle strips with discrete tessellation (thousands of draws fail with "Unsupported
  tessellation mode 0 for primitive type 6"). This converts strips and fans to triangle
  lists for the tessellator (Xenia 3eab2b8b39 does it for auto-indexed draws only). With it
  the draws are submitted, but the GPU hangs (amdgpu ring gfx timeout) within ~2 minutes
  of campaign start on RADV, so the tessellation path itself needs work first.
