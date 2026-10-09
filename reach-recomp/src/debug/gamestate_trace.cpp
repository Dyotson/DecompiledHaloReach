// reach - trace game state save/load and its integrity check.
//
// Campaign checkpoints copy the 0xA70000-byte game state and protect it with a
// keyed SHA-1 stored at +0x1E708. sub_82429300 verifies that hash and raises
// the fatal "game_state: failed to verify hash for in-place gamestate" error
// when it fails for the live state. With REACH_GSTRACE=1 the verify, the hash
// helper and the load paths that lead to it are logged with their callers and
// results, then forwarded to the recompiled functions.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

REX_EXTERN(__imp__sub_82429300);  // game_state verify(buffer, allow_unset)
REX_EXTERN(__imp__sub_8242E968);  // keyed SHA-1 of (data, size, mode, expected, out)
REX_EXTERN(__imp__sub_824288F0);  // core load (content "mmiof.bmf" or cache slot)
REX_EXTERN(__imp__sub_824B9A90);  // read a file from the user's save content
REX_EXTERN(__imp__sub_82429858);  // load game state from a compressed buffer
REX_EXTERN(__imp__sub_824285C0);  // checkpoint revert
REX_EXTERN(__imp__sub_82554848);  // load a checkpoint from a cache1:\savegames slot
REX_EXTERN(__imp__sub_826F5660);  // fatal error: (format, argument)
REX_EXTERN(__imp__sub_82428D98);  // post-load fixups
REX_EXTERN(__imp__sub_824DDA88);  // before replacing the game state
REX_EXTERN(__imp__sub_824DDBE8);  // after replacing the game state

namespace {

bool Enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("REACH_GSTRACE");
    return v && *v && *v != '0';
  }();
  return enabled;
}

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

std::string Hex(const uint8_t* p, size_t n) {
  static const char kDigits[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += kDigits[p[i] >> 4];
    s += kDigits[p[i] & 15];
  }
  return s;
}

// Globals of the game state system.
constexpr uint32_t kLiveStatePtr = 0x83597B04;
constexpr uint32_t kHeaderStatePtr = 0x83597B14;
constexpr uint32_t kHashOffset = 0x1E708;

}  // namespace

extern "C" REX_FUNC(sub_82429300) {
  if (!Enabled()) return __imp__sub_82429300(ctx, base);
  const uint32_t buffer = ctx.r3.u32, flag = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t live = LoadBE32(base + kLiveStatePtr);
  const uint32_t header = LoadBE32(base + kHeaderStatePtr);
  const uint32_t hash_at = (buffer ? buffer : header) + kHashOffset;
  const std::string stored = Hex(base + hash_at, 20);
  __imp__sub_82429300(ctx, base);
  REXLOG_INFO("GSTRACE verify(buffer={:#x}, flag={}) lr={:#x} live={:#x} header={:#x} stored={} -> {}",
              buffer, flag, lr, live, header, stored, ctx.r3.u32 & 0xFF);
}

extern "C" REX_FUNC(sub_8242E968) {
  if (!Enabled()) return __imp__sub_8242E968(ctx, base);
  const uint32_t data = ctx.r3.u32, size = ctx.r4.u32, mode = ctx.r5.u32;
  const uint32_t expected = ctx.r6.u32, out = ctx.r7.u32, lr = static_cast<uint32_t>(ctx.lr);
  const std::string want = expected ? Hex(base + expected, 20) : "-";
  __imp__sub_8242E968(ctx, base);
  REXLOG_INFO("GSTRACE sha(data={:#x}, size={:#x}, mode={}) lr={:#x} expected={} computed={} -> {}",
              data, size, mode, lr, want, out ? Hex(base + out, 20) : "-", ctx.r3.u32 & 0xFF);
}

#define REACH_GSTRACE_PASSTHROUGH(addr, what)                                               \
  extern "C" REX_FUNC(sub_##addr) {                                                         \
    if (!Enabled()) return __imp__sub_##addr(ctx, base);                                    \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;     \
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);                                      \
    REXLOG_INFO("GSTRACE " what " enter r3={:#x} r4={:#x} r5={:#x} r6={:#x} lr={:#x}", r3, r4, \
                r5, r6, lr);                                                                \
    __imp__sub_##addr(ctx, base);                                                           \
    REXLOG_INFO("GSTRACE " what " -> {:#x}", ctx.r3.u32);                                   \
  }

REACH_GSTRACE_PASSTHROUGH(824288F0, "core_load")
REACH_GSTRACE_PASSTHROUGH(824B9A90, "content_read")
REACH_GSTRACE_PASSTHROUGH(82429858, "load_from_buffer")
REACH_GSTRACE_PASSTHROUGH(824285C0, "checkpoint_revert")
REACH_GSTRACE_PASSTHROUGH(82554848, "cache_slot_load")

REACH_GSTRACE_PASSTHROUGH(82428D98, "post_load_fixups")
REACH_GSTRACE_PASSTHROUGH(824DDA88, "pre_replace")
REACH_GSTRACE_PASSTHROUGH(824DDBE8, "post_replace")

// Logs the message of a game fatal error before the game halts.
extern "C" REX_FUNC(sub_826F5660) {
  if (Enabled()) {
    auto str = [&](uint32_t addr) {
      if (addr < 0x80000000u || addr >= 0x90000000u) return std::string("?");
      return std::string(reinterpret_cast<const char*>(base + addr), 0, 200);
    };
    REXLOG_ERROR("GSTRACE fatal_error lr={:#x} format=\"{}\" arg=\"{}\"",
                 static_cast<uint32_t>(ctx.lr), str(ctx.r3.u32), str(ctx.r4.u32));
  }
  __imp__sub_826F5660(ctx, base);
}
