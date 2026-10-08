// reach - scripted controller input for unattended boot tests.
//
// REACH_AUTOPRESS="45:A,50:START,52.5:DOWN" presses each button for 0.3 s at
// the given number of seconds after the first XamInputGetState call. Buttons:
// A B X Y START BACK UP DOWN LEFT RIGHT LB RB. When the variable is unset the
// hook only forwards to the SDK implementation. Presses are applied to user 0
// even if no physical controller is connected.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

struct Press {
  double at_seconds;
  uint16_t buttons;
};

constexpr double kHoldSeconds = 0.3;
constexpr uint32_t kErrorSuccess = 0;

uint16_t ButtonMask(const std::string& name) {
  static const struct {
    const char* name;
    uint16_t mask;
  } kButtons[] = {
      {"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
      {"START", 0x0010}, {"BACK", 0x0020}, {"LB", 0x0100},   {"RB", 0x0200},
      {"A", 0x1000},     {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000},
  };
  for (const auto& b : kButtons) {
    if (name == b.name) return b.mask;
  }
  return 0;
}

std::vector<Press> ParseSchedule(const char* spec) {
  std::vector<Press> presses;
  std::string s(spec);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    if (end == std::string::npos) end = s.size();
    std::string item = s.substr(pos, end - pos);
    size_t colon = item.find(':');
    if (colon != std::string::npos) {
      uint16_t mask = ButtonMask(item.substr(colon + 1));
      if (mask) presses.push_back({std::atof(item.substr(0, colon).c_str()), mask});
    }
    pos = end + 1;
  }
  return presses;
}

GuestFunc SdkGetState() {
  static GuestFunc fn = reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, "__imp__XamInputGetState"));
  return fn;
}

}  // namespace

// DWORD XamInputGetState(DWORD user_index, DWORD flags, X_INPUT_STATE* state)
// X_INPUT_STATE: be32 packet_number; be16 buttons; u8 lt; u8 rt; be16 lx, ly, rx, ry.
extern "C" REX_FUNC(__imp__XamInputGetState) {
  static std::once_flag once;
  static std::vector<Press> schedule;
  static std::chrono::steady_clock::time_point start;
  static uint32_t packet = 0;
  std::call_once(once, [] {
    start = std::chrono::steady_clock::now();
    if (const char* spec = std::getenv("REACH_AUTOPRESS")) {
      schedule = ParseSchedule(spec);
      REXLOG_INFO("REACH_AUTOPRESS: {} scheduled presses", schedule.size());
    }
  });

  const uint32_t user_index = ctx.r3.u32;
  const uint32_t state_addr = ctx.r5.u32;
  if (GuestFunc sdk = SdkGetState()) {
    sdk(ctx, base);
  }
  if (schedule.empty() || user_index != 0 || state_addr == 0) return;

  double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  uint16_t injected = 0;
  for (const Press& p : schedule) {
    if (now >= p.at_seconds && now < p.at_seconds + kHoldSeconds) injected |= p.buttons;
  }
  if (!injected) return;

  uint8_t* state = base + state_addr;
  if (ctx.r3.u32 != kErrorSuccess) {
    // No physical pad: present a connected, centred one.
    std::memset(state, 0, 16);
    ctx.r3.u64 = kErrorSuccess;
  }
  uint16_t buttons;
  std::memcpy(&buttons, state + 4, 2);
  buttons = __builtin_bswap16(static_cast<uint16_t>(__builtin_bswap16(buttons) | injected));
  std::memcpy(state + 4, &buttons, 2);
  uint32_t pkt = __builtin_bswap32(++packet | 0x80000000u);
  std::memcpy(state, &pkt, 4);
  REXLOG_INFO("REACH_AUTOPRESS: t={:.2f}s buttons={:#06x}", now, injected);
}
