// reach - diagnostics for textures bound with an invalid fetch constant.
//
// sub_8216B0C8 is the statically linked D3DDevice_SetTexture(device, stage,
// texture, dirty_mask): it copies the texture's 6-dword GPU fetch constant
// (D3DBaseTexture +0x1C..+0x33) into the device's shadow. When the texture
// header's type bits are not 2 (texture) the GPU sees an "invalid" fetch.
// With REACH_TEXTRACE=1 this logs each distinct (caller, texture) pair bound
// with an invalid header, and each distinct caller binding NULL, then forwards
// to the recompiled function.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

REX_EXTERN(__imp__sub_8216B0C8);

namespace {

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

bool Enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("REACH_TEXTRACE");
    return v && *v && *v != '0';
  }();
  return enabled;
}

}  // namespace

extern "C" REX_FUNC(sub_8216B0C8) {
  if (Enabled() && ctx.r5.u32 == 0) {
    static std::mutex mutex;
    static std::set<uint32_t> seen;
    std::lock_guard lock(mutex);
    if (seen.size() < 100 && seen.insert(static_cast<uint32_t>(ctx.lr)).second) {
      REXLOG_WARN("REACH_TEXTRACE: SetTexture stage={} tex=NULL caller={:#010x}", ctx.r4.u32,
                  static_cast<uint32_t>(ctx.lr));
    }
  }
  if (Enabled() && ctx.r5.u32 != 0) {
    const uint32_t tex = ctx.r5.u32;
    const uint8_t* fetch = base + tex + 0x1C;
    uint32_t dw[6];
    for (int i = 0; i < 6; ++i) dw[i] = LoadBE32(fetch + i * 4);
    if ((dw[0] & 3) == 2 && std::getenv("REACH_TEXTRACE_VALID")) {
      static std::mutex mutex;
      static std::set<uint32_t> seen;
      std::lock_guard lock(mutex);
      if (seen.size() < 200 && seen.insert(static_cast<uint32_t>(ctx.lr)).second) {
        REXLOG_INFO("REACH_TEXTRACE: valid SetTexture stage={} tex={:#010x} caller={:#010x}",
                    ctx.r4.u32, tex, static_cast<uint32_t>(ctx.lr));
      }
    }
    if ((dw[0] & 3) != 2) {
      static std::mutex mutex;
      static std::set<std::pair<uint32_t, uint32_t>> seen;
      std::lock_guard lock(mutex);
      if (seen.size() < 200 && seen.emplace(static_cast<uint32_t>(ctx.lr), tex).second) {
        REXLOG_WARN(
            "REACH_TEXTRACE: SetTexture stage={} tex={:#010x} caller={:#010x} "
            "fetch={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} common={:08X}",
            ctx.r4.u32, tex, static_cast<uint32_t>(ctx.lr), dw[0], dw[1], dw[2], dw[3], dw[4],
            dw[5], LoadBE32(base + tex));
      }
    }
  }
  __imp__sub_8216B0C8(ctx, base);
}

// Function_8216AD90 is the engine's rasterizer set_texture(stage, ref): it
// skips D3D SetTexture when its per-stage cache at 0x82A8F938 already holds
// the same texture. Report cache hits while D3D's own stage pointer
// (device + (stage + 0xC6C) * 4, device at *0x83150B30) is NULL: that is a
// desync that leaves the stage unbound.
REX_EXTERN(__imp__sub_8216AD90);

extern "C" REX_FUNC(sub_8216AD90) {
  if (Enabled()) {
    const uint32_t stage = ctx.r3.u32;
    const uint32_t handle = LoadBE32(base + ctx.r4.u32);
    if (stage < 26 && handle != 0 && handle != 0xFFFFFFFFu) {
      const uint32_t cached = LoadBE32(base + 0x82A8F938u + stage * 4);
      const uint32_t device = LoadBE32(base + 0x83150B30u);
      const uint32_t bound = LoadBE32(base + device + (stage + 0xC6C) * 4);
      if (cached == handle && bound == 0) {
        static std::mutex mutex;
        static std::set<std::pair<uint32_t, uint32_t>> seen;
        static uint64_t count = 0;
        std::lock_guard lock(mutex);
        ++count;
        if (seen.size() < 60 && seen.emplace(static_cast<uint32_t>(ctx.lr), stage).second) {
          REXLOG_WARN(
              "REACH_TEXTRACE: cache desync stage={} tex={:#010x} caller={:#010x} (hits so far {})",
              stage, handle, static_cast<uint32_t>(ctx.lr), count);
        }
      }
    }
  }
  __imp__sub_8216AD90(ctx, base);
}
