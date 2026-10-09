// reach - scripted controller input for unattended boot tests.
//
// REACH_AUTOPRESS="45:A,50:START,52.5:DOWN,60:LSUP:3" presses each input at
// the given number of seconds after the first XamInputGetState call, for 0.3 s
// or for the optional third field in seconds. Buttons: A B X Y START BACK UP
// DOWN LEFT RIGHT LB RB LS RS. Full deflection of a stick: LSUP LSDOWN LSLEFT
// LSRIGHT RSUP RSDOWN RSLEFT RSRIGHT. Triggers: LT RT. When the variable is
// unset the hook only forwards to the SDK implementation. Input is applied to
// user 0 even if no physical controller is connected.

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
  double hold_seconds;
  uint16_t buttons;
  // Stick axes (left x/y, right x/y) and triggers (left, right); 0 = untouched.
  int16_t sticks[4];
  uint8_t triggers[2];
};

constexpr double kDefaultHoldSeconds = 0.3;
constexpr uint32_t kErrorSuccess = 0;

uint16_t ButtonMask(const std::string& name) {
  static const struct {
    const char* name;
    uint16_t mask;
  } kButtons[] = {
      {"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
      {"START", 0x0010}, {"BACK", 0x0020}, {"LB", 0x0100},   {"RB", 0x0200},
      {"LS", 0x0040},    {"RS", 0x0080},
      {"A", 0x1000},     {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000},
  };
  for (const auto& b : kButtons) {
    if (name == b.name) return b.mask;
  }
  return 0;
}

// Fills the stick/trigger fields of `press` for an analog input name.
bool ApplyAnalog(const std::string& name, Press& press) {
  static const struct {
    const char* name;
    int axis;  // 0 lx, 1 ly, 2 rx, 3 ry
    int16_t value;
  } kSticks[] = {
      {"LSUP", 1, 32767},  {"LSDOWN", 1, -32768}, {"LSLEFT", 0, -32768}, {"LSRIGHT", 0, 32767},
      {"RSUP", 3, 32767},  {"RSDOWN", 3, -32768}, {"RSLEFT", 2, -32768}, {"RSRIGHT", 2, 32767},
  };
  for (const auto& s : kSticks) {
    if (name == s.name) {
      press.sticks[s.axis] = s.value;
      return true;
    }
  }
  if (name == "LT" || name == "RT") {
    press.triggers[name == "LT" ? 0 : 1] = 255;
    return true;
  }
  return false;
}

std::vector<Press> ParseSchedule(const char* spec) {
  std::vector<Press> presses;
  std::string s(spec);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    if (end == std::string::npos) end = s.size();
    std::string item = s.substr(pos, end - pos);
    pos = end + 1;
    size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    size_t colon2 = item.find(':', colon + 1);
    std::string name = item.substr(colon + 1, colon2 == std::string::npos ? std::string::npos
                                                                         : colon2 - colon - 1);
    Press press{};
    press.at_seconds = std::atof(item.substr(0, colon).c_str());
    press.hold_seconds = colon2 == std::string::npos
                             ? kDefaultHoldSeconds
                             : std::atof(item.substr(colon2 + 1).c_str());
    press.buttons = ButtonMask(name);
    if (press.buttons || ApplyAnalog(name, press)) presses.push_back(press);
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
  int16_t sticks[4] = {};
  uint8_t triggers[2] = {};
  bool any = false;
  for (const Press& p : schedule) {
    if (now < p.at_seconds || now >= p.at_seconds + p.hold_seconds) continue;
    any = true;
    injected |= p.buttons;
    for (int i = 0; i < 4; ++i) {
      if (p.sticks[i]) sticks[i] = p.sticks[i];
    }
    for (int i = 0; i < 2; ++i) {
      if (p.triggers[i]) triggers[i] = p.triggers[i];
    }
  }
  if (!any) return;

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
  for (int i = 0; i < 2; ++i) {
    if (triggers[i]) state[6 + i] = triggers[i];
  }
  for (int i = 0; i < 4; ++i) {
    if (!sticks[i]) continue;
    uint16_t be = __builtin_bswap16(static_cast<uint16_t>(sticks[i]));
    std::memcpy(state + 8 + 2 * i, &be, 2);
  }
  uint32_t pkt = __builtin_bswap32(++packet | 0x80000000u);
  std::memcpy(state, &pkt, 4);
  REXLOG_DEBUG("REACH_AUTOPRESS: t={:.2f}s buttons={:#06x} sticks={},{},{},{}", now, injected,
               sticks[0], sticks[1], sticks[2], sticks[3]);
}
