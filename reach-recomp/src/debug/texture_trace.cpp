// reach - diagnostics for textures bound with an invalid fetch constant.
//
// sub_8216B0C8 is the statically linked D3DDevice_SetTexture(device, stage,
// texture, dirty_mask): it copies the texture's 6-dword GPU fetch constant
// (D3DBaseTexture +0x1C..+0x33) into the device's shadow. When the texture
// header's type bits are not 2 (texture) the GPU sees an "invalid" fetch.
// With REACH_TEXTRACE=1 this logs each distinct (caller, texture) pair bound
// with an invalid header, and each distinct caller binding NULL, then forwards
// to the recompiled function.

#include "../platform/guest_memory.h"

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <bit>
#include <chrono>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

using reach::GuestPtr;

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
    const uint8_t* fetch = GuestPtr(base, tex + 0x1C);
    uint32_t dw[6];
    for (int i = 0; i < 6; ++i) dw[i] = LoadBE32(fetch + i * 4);
    if ((dw[0] & 3) == 2 && std::getenv("REACH_TEXTRACE_VALID")) {
      static std::mutex mutex;
      static std::set<uint32_t> seen;
      std::lock_guard lock(mutex);
      if (seen.size() < 400 && seen.insert(tex).second) {
        // dw1: base address (bits 12..31) | format (bits 0..5); dw2: (h-1)<<13 | (w-1) for 2D.
        REXLOG_INFO(
            "REACH_TEXTRACE: valid SetTexture stage={} tex={:#010x} caller={:#010x} base={:#010x} "
            "fmt={} size={}x{}",
            ctx.r4.u32, tex, static_cast<uint32_t>(ctx.lr), dw[1] & 0xFFFFF000u, dw[1] & 0x3F,
            (dw[2] & 0x1FFF) + 1, ((dw[2] >> 13) & 0x1FFF) + 1);
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
            dw[5], LoadBE32(GuestPtr(base, tex)));
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

namespace {
// Per-caller counts of set_texture handles: 0 (texture ref without a texture,
// typically not resident), -1 (deliberate unbind) and real textures.
struct HandleStats {
  uint64_t zero = 0, unbind = 0, real = 0;
};
std::mutex g_handle_mutex;
std::map<uint32_t, HandleStats> g_handle_stats;
uint64_t g_handle_calls = 0;

void RecordHandle(uint32_t caller, uint32_t handle) {
  std::lock_guard lock(g_handle_mutex);
  auto& st = g_handle_stats[caller];
  (handle == 0 ? st.zero : handle == 0xFFFFFFFFu ? st.unbind : st.real)++;
  static auto last = std::chrono::steady_clock::now();
  ++g_handle_calls;
  auto now = std::chrono::steady_clock::now();
  if (now - last > std::chrono::seconds(5)) {
    last = now;
    REXLOG_WARN("REACH_TEXTRACE: set_texture total calls {}", g_handle_calls);
    for (const auto& [lr, s] : g_handle_stats) {
      REXLOG_WARN("REACH_TEXTRACE: set_texture caller={:#010x} zero={} unbind={} real={}", lr,
                  s.zero, s.unbind, s.real);
    }
  }
}
}  // namespace

// Guest pointers below 0x40000000 are not backed by the heaps the engine
// uses; reading them from the host faults.
bool PlausibleGuestPointer(uint32_t addr) { return addr >= 0x40000000u; }

extern "C" REX_FUNC(sub_8216AD90) {
  if (Enabled() && PlausibleGuestPointer(ctx.r4.u32)) {
    RecordHandle(static_cast<uint32_t>(ctx.lr), LoadBE32(GuestPtr(base, ctx.r4.u32)));
  }
  if (Enabled() && PlausibleGuestPointer(ctx.r4.u32)) {
    const uint32_t stage = ctx.r3.u32;
    const uint32_t handle = LoadBE32(GuestPtr(base, ctx.r4.u32));
    if (stage < 26 && handle != 0 && handle != 0xFFFFFFFFu) {
      const uint32_t cached = LoadBE32(GuestPtr(base, 0x82A8F938u + stage * 4));
      const uint32_t device = LoadBE32(GuestPtr(base, 0x83150B30u));
      const uint32_t bound = LoadBE32(GuestPtr(base, device + (stage + 0xC6C) * 4));
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

// Function_8218CA88(stage, global_texture_id) binds engine-global textures
// (render targets, noise, LUTs) to a sampler stage. Log each distinct
// (caller, stage, id) once to see which passes bind what.
REX_EXTERN(__imp__sub_8218CA88);

extern "C" REX_FUNC(sub_8218CA88) {
  if (Enabled()) {
    static std::mutex mutex;
    static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
    std::lock_guard lock(mutex);
    if (seen.size() < 400 &&
        seen.emplace(static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32).second) {
      REXLOG_INFO("REACH_TEXTRACE: bind_global stage={} id={:#x} caller={:#010x}", ctx.r3.u32,
                  ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
    }
  }
  __imp__sub_8218CA88(ctx, base);
}

// Function_821BB700 is the final composite. It picks the composite variant
// from bytes +0x98/+0x9A/+0x9B of the struct at *(0x83150D58) + 0x3A8 (only
// when the byte at 0x8315110C is 0). Log what it sees.
REX_EXTERN(__imp__sub_821BB700);

extern "C" REX_FUNC(sub_821BB700) {
  if (Enabled()) {
    // Histogram of the variant the composite will pick (same logic as the guest).
    static std::map<int, uint64_t> variants;
    static auto last_hist = std::chrono::steady_clock::now();
    const uint32_t v = LoadBE32(GuestPtr(base, 0x83150D58u));
    int variant = 0;
    if (base[0x8315110Cu] == 0) {
      const uint8_t* st = GuestPtr(base, v + 0x3A8);
      if (st[0x98]) variant = st[0x9A] ? 5 : 1;
      else if (st[0x9B]) variant = 0x0B;
    }
    ++variants[variant];
    if (std::chrono::steady_clock::now() - last_hist > std::chrono::seconds(4)) {
      last_hist = std::chrono::steady_clock::now();
      std::string h;
      for (auto& [k, n] : variants) h += fmt::format(" v{}={}", k, n);
      REXLOG_WARN("REACH_TEXTRACE: composite variants:{} (blur amount s+0xAC={})", h,
                  std::bit_cast<float>(LoadBE32(GuestPtr(base, v + 0x3A8 + 0xAC))));
    }
  }
  if (Enabled()) {
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(3)) {
      last = now;
      const uint32_t view = LoadBE32(GuestPtr(base, 0x83150D58u));
      const uint8_t gate = base[0x8315110Cu];
      const uint32_t s = view + 0x3A8;
      REXLOG_WARN(
          "REACH_TEXTRACE: composite view_ptr={:#010x} gate={} s={:#010x} s+98={} s+9A={} s+9B={}",
          view, gate, s, base[s + 0x98], base[s + 0x9A], base[s + 0x9B]);
    }
  }
  __imp__sub_821BB700(ctx, base);
}
