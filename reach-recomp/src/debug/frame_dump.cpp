// reach - dump presented frames from guest memory, independent of window focus.
//
// REACH_FRAMEDUMP="55,65.5" dumps the front buffer handed to VdSwap at those
// seconds after the first swap, into $REACH_FRAMEDUMP_DIR (default /tmp) as
// reach_frame_<secs>.bin plus a .json sidecar describing the layout. Convert
// with tools/frame_to_png.py. Run with --vulkan_readback_resolve=true so GPU
// resolves are written back to guest memory; otherwise the dump shows whatever
// the CPU last wrote there.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

// Physical memory is mirrored into the guest virtual space at 0xA0000000.
constexpr uint32_t kPhysicalMirror = 0xA0000000;

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

struct Schedule {
  std::vector<double> times;
  size_t next = 0;
  std::string dir = "/tmp";
};

Schedule& GetSchedule() {
  static Schedule schedule;
  static std::once_flag once;
  std::call_once(once, [] {
    if (const char* dir = std::getenv("REACH_FRAMEDUMP_DIR")) schedule.dir = dir;
    if (const char* spec = std::getenv("REACH_FRAMEDUMP")) {
      std::string s(spec);
      size_t pos = 0;
      while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        schedule.times.push_back(std::atof(s.substr(pos, end - pos).c_str()));
        pos = end + 1;
      }
      std::sort(schedule.times.begin(), schedule.times.end());
      REXLOG_INFO("REACH_FRAMEDUMP: {} dumps scheduled into {}", schedule.times.size(),
                  schedule.dir);
    }
  });
  return schedule;
}

void DumpFrontBuffer(uint8_t* base, uint32_t fetch_addr, double seconds, const std::string& dir) {
  // Texture fetch constant (6 big-endian dwords).
  const uint8_t* fetch = base + fetch_addr;
  uint32_t dw0 = LoadBE32(fetch), dw1 = LoadBE32(fetch + 4), dw2 = LoadBE32(fetch + 8);
  uint32_t address = dw1 & 0xFFFFF000u;
  uint32_t format = dw1 & 0x3F;
  uint32_t width = (dw2 & 0x1FFF) + 1;
  uint32_t height = ((dw2 >> 13) & 0x1FFF) + 1;
  uint32_t pitch_texels = ((dw0 >> 22) & 0x1FF) << 5;
  bool tiled = (dw0 >> 31) & 1;
  uint32_t aligned_width = std::max(pitch_texels, (width + 31) & ~31u);
  uint32_t aligned_height = (height + 31) & ~31u;
  size_t bytes = size_t(aligned_width) * aligned_height * 4;

  char name[64];
  std::snprintf(name, sizeof(name), "reach_frame_%07.2f", seconds);
  std::string stem = dir + "/" + name;
  if (FILE* f = std::fopen((stem + ".bin").c_str(), "wb")) {
    std::fwrite(base + kPhysicalMirror + (address & 0x1FFFFFFF), 1, bytes, f);
    std::fclose(f);
  }
  if (FILE* f = std::fopen((stem + ".json").c_str(), "w")) {
    std::fprintf(f,
                 "{\"width\": %u, \"height\": %u, \"aligned_width\": %u, \"aligned_height\": %u, "
                 "\"format\": %u, \"tiled\": %s, \"address\": %u, \"seconds\": %.2f}\n",
                 width, height, aligned_width, aligned_height, format, tiled ? "true" : "false",
                 address, seconds);
    std::fclose(f);
  }
  REXLOG_INFO("REACH_FRAMEDUMP: t={:.2f}s {}x{} fmt={} tiled={} addr={:#x} -> {}.bin", seconds,
              width, height, format, tiled, address, stem);
}

}  // namespace

// void VdSwap(buffer_ptr, fetch_ptr, unk2, unk3, unk4, frontbuffer_ptr,
//             texture_format_ptr, color_space_ptr, width_ptr, height_ptr)
extern "C" REX_FUNC(__imp__VdSwap) {
  static GuestFunc sdk = reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, "__imp__VdSwap"));
  static const auto start = std::chrono::steady_clock::now();
  const uint32_t fetch_addr = ctx.r4.u32;

  Schedule& schedule = GetSchedule();
  if (schedule.next < schedule.times.size() && fetch_addr) {
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (now >= schedule.times[schedule.next]) {
      DumpFrontBuffer(base, fetch_addr, now, schedule.dir);
      while (schedule.next < schedule.times.size() && schedule.times[schedule.next] <= now) {
        ++schedule.next;
      }
    }
  }
  if (sdk) sdk(ctx, base);
}
