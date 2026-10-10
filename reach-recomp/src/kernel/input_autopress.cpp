// reach - scripted controller input for unattended boot tests.
//
// REACH_AUTOPRESS="45:A,50:START,52.5:DOWN,60:LSUP:3" presses each input at
// the given number of seconds after the first XamInputGetState call, for 0.3 s
// or for the optional third field in seconds. Buttons: A B X Y START BACK UP
// DOWN LEFT RIGHT LB RB LS RS. Full deflection of a stick: LSUP LSDOWN LSLEFT
// LSRIGHT RSUP RSDOWN RSLEFT RSRIGHT. Triggers: LT RT. When the variable is
// unset the hook only forwards to the SDK implementation. Input is applied to
// user 0 even if no physical controller is connected.
//
// REACH_AUTOPRESS_FIFO=<path> also reads "INPUT[:hold]" lines from that FIFO
// (create it with mkfifo) while the game runs and presses each one as it
// arrives, so menus can be stepped through while watching frame dumps
// (REACH_FRAMEDUMP_TRIGGER). In either mode the game sees a connected, centred
// controller between presses instead of a disconnected one.
//
// The FIFO also drives the keyboard and mouse device (src/input/kbm.cpp) through the same
// paths as the real ones: "KEY:name[:hold]" holds a key or mouse button by its binding name
// ("Space", "LMB", "WheelUp"; 0.3 s by default), "MOUSE:dx,dy" moves the mouse by that many
// counts.

#include "../platform/sdk_import.h"
#include "../input/kbm.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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

std::mutex schedule_mutex;

// Reads "INPUT[:hold]" lines from the FIFO and schedules each one right away.
void ReadFifo(std::string path, std::vector<Press>* schedule,
              std::chrono::steady_clock::time_point start) {
#ifdef _WIN32
  // A POSIX FIFO; scripted test sessions (tools/live_session.sh) run on Linux.
  (void)schedule;
  (void)start;
  REXLOG_WARN("REACH_AUTOPRESS_FIFO is not supported on Windows ({})", path);
  return;
#else
  // O_RDWR keeps the FIFO open (no EOF) when the writer goes away.
  int fd = open(path.c_str(), O_RDWR);
  if (fd < 0) {
    REXLOG_WARN("REACH_AUTOPRESS_FIFO: cannot open {}", path);
    return;
  }
  std::string line;
  char c;
  while (read(fd, &c, 1) == 1) {
    if (c != '\n') {
      line += c;
      continue;
    }
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (line.rfind("MOUSE:", 0) == 0) {
      float dx = 0, dy = 0;
      if (std::sscanf(line.c_str() + 6, "%f,%f", &dx, &dy) == 2) {
        reach::kbm::InjectMouse(dx, dy);
      }
      REXLOG_INFO("REACH_AUTOPRESS_FIFO: t={:.2f}s {}", now, line);
      line.clear();
      continue;
    }
    if (line.rfind("KEY:", 0) == 0) {
      std::string key = line.substr(4);
      double hold = kDefaultHoldSeconds;
      if (size_t colon = key.find(':'); colon != std::string::npos) {
        hold = std::atof(key.c_str() + colon + 1);
        key.resize(colon);
      }
      if (!reach::kbm::InjectKey(key, hold)) {
        REXLOG_WARN("REACH_AUTOPRESS_FIFO: unknown key {}", key);
      }
      REXLOG_INFO("REACH_AUTOPRESS_FIFO: t={:.2f}s {}", now, line);
      line.clear();
      continue;
    }
    std::vector<Press> parsed = ParseSchedule(("0:" + line).c_str());
    for (Press& press : parsed) {
      press.at_seconds = now;
      std::lock_guard<std::mutex> lock(schedule_mutex);
      schedule->push_back(press);
    }
    REXLOG_INFO("REACH_AUTOPRESS_FIFO: t={:.2f}s {}", now, line);
    line.clear();
  }
#endif
}

GuestFunc SdkGetState() {
  static GuestFunc fn = reach::SdkImport("__imp__XamInputGetState");
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
  static bool scripted = false;
  std::call_once(once, [] {
    start = std::chrono::steady_clock::now();
    if (const char* spec = std::getenv("REACH_AUTOPRESS")) {
      schedule = ParseSchedule(spec);
      scripted = !schedule.empty();
      REXLOG_INFO("REACH_AUTOPRESS: {} scheduled presses", schedule.size());
    }
    if (const char* fifo = std::getenv("REACH_AUTOPRESS_FIFO"); fifo && *fifo) {
      scripted = true;
      std::thread(ReadFifo, std::string(fifo), &schedule, start).detach();
    }
  });

  const uint32_t user_index = ctx.r3.u32;
  const uint32_t state_addr = ctx.r5.u32;
  if (GuestFunc sdk = SdkGetState()) {
    sdk(ctx, base);
  }
  if (!scripted || user_index != 0 || state_addr == 0) return;

  double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  uint16_t injected = 0;
  int16_t sticks[4] = {};
  uint8_t triggers[2] = {};
  bool any = false;
  std::lock_guard<std::mutex> lock(schedule_mutex);
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
  uint8_t* state = base + state_addr;
  if (ctx.r3.u32 != kErrorSuccess) {
    // No physical pad: present a connected, centred one, also between presses
    // (a disconnected pad pauses gameplay).
    std::memset(state, 0, 16);
    ctx.r3.u64 = kErrorSuccess;
  }
  if (!any) return;
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
