// reach - dump presented frames from guest memory, independent of window focus.
//
// REACH_FRAMEDUMP="55,65.5" dumps the front buffer handed to VdSwap at those
// seconds after the first swap, into $REACH_FRAMEDUMP_DIR (default /tmp) as
// reach_frame_<secs>.bin plus a .json sidecar describing the layout. Convert
// with tools/frame_to_png.py. Run with --vulkan_readback_resolve=true so GPU
// resolves are written back to guest memory; otherwise the dump shows whatever
// the CPU last wrote there.
//
// REACH_FRAMEDUMP_TRIGGER=<path> also dumps a frame whenever that file appears
// (it is deleted again), for stepping through menus with REACH_AUTOPRESS_FIFO.
//
// REACH_INVALIDATE_AT="20" fires the physical-memory write callbacks over the
// whole 0xA0000000 mirror once at that time, forcing the GPU to drop and
// re-upload every watched texture (diagnoses stale GPU texture copies).
//
// REACH_RDCAPTURE="25,90" asks RenderDoc (when the game runs under renderdoccmd)
// to capture a few frames at each of those times, without needing keyboard
// focus. Each capture is a separate .rdc file. REACH_RDCAPTURE_TRIGGER=<path>
// also captures whenever that file appears (it is deleted again), so a scene
// can be caught by hand while watching periodic frame dumps; load times vary
// too much between runs for fixed times to land on the same shot.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include <dlfcn.h>
#include <renderdoc/renderdoc_app.h>

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

// Parses "12,30.5,60" into sorted seconds.
std::vector<double> ParseTimes(const char* spec) {
  std::vector<double> times;
  std::string s(spec ? spec : "");
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    if (end == std::string::npos) end = s.size();
    times.push_back(std::atof(s.substr(pos, end - pos).c_str()));
    pos = end + 1;
  }
  std::sort(times.begin(), times.end());
  return times;
}

Schedule& GetSchedule() {
  static Schedule schedule;
  static std::once_flag once;
  std::call_once(once, [] {
    if (const char* dir = std::getenv("REACH_FRAMEDUMP_DIR")) schedule.dir = dir;
    if (const char* spec = std::getenv("REACH_FRAMEDUMP")) {
      schedule.times = ParseTimes(spec);
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
  // The front buffer constant holds a virtual address in one of the physical
  // mirrors (0xE3044000 for Reach). Read it through that mapping: the 0xE0000000
  // mirror sits 4 KiB higher in physical memory than the 0xA0000000 one, so
  // reading the masked address via 0xA0000000 shifts the image by one tile.
  const uint8_t* src =
      address >= kPhysicalMirror ? base + address : base + kPhysicalMirror + address;
  if (FILE* f = std::fopen((stem + ".bin").c_str(), "wb")) {
    std::fwrite(src, 1, bytes, f);
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

void MaybeInvalidatePhysicalMemory(double now) {
  static const double at = [] {
    const char* v = std::getenv("REACH_INVALIDATE_AT");
    return v ? std::atof(v) : -1.0;
  }();
  static bool done = false;
  if (done || at < 0 || now < at) return;
  done = true;
  auto* memory = rex::system::kernel_state()->memory();
  bool any = memory->TriggerPhysicalMemoryCallbacks(
      rex::thread::global_critical_region::AcquireDirect(), 0xA0000000u, 0x20000000u,
      /*is_write=*/true, /*unwatch_exact_range=*/false);
  REXLOG_INFO("REACH_INVALIDATE_AT: physical write callbacks fired at t={:.2f}s (watched pages: {})",
              now, any);
}

// Captures explicitly with Start/EndFrameCapture spanning a few guest frames:
// TriggerCapture never produced a file with this runtime's presenter.
void MaybeTriggerRenderDocCapture(double now) {
  static const std::vector<double> times = ParseTimes(std::getenv("REACH_RDCAPTURE"));
  static size_t next = 0;
  static int swaps = 0;  // swaps captured so far in the open capture, 0 when idle
  static RENDERDOC_API_1_0_0* api = nullptr;
  if (swaps > 0) {
    if (++swaps > 3) {
      uint32_t ok = api->EndFrameCapture(nullptr, nullptr);
      REXLOG_INFO("REACH_RDCAPTURE: capture ended at t={:.2f}s (result {})", now, ok);
      swaps = 0;
    }
    return;
  }
  static const char* trigger = std::getenv("REACH_RDCAPTURE_TRIGGER");
  static int polls = 0;
  bool due = next < times.size() && now >= times[next];
  while (next < times.size() && times[next] <= now) ++next;
  // The trigger file is polled every 10 swaps.
  if (!due && trigger && *trigger && ++polls % 10 == 0) {
    due = std::remove(trigger) == 0;
  }
  if (!due) return;
  if (!api) {
    void* lib = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    auto get_api =
        lib ? reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(lib, "RENDERDOC_GetAPI")) : nullptr;
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&api)) ||
        !api) {
      REXLOG_WARN("REACH_RDCAPTURE: RenderDoc is not loaded (run under renderdoccmd capture)");
      api = nullptr;
      next = times.size();
      trigger = nullptr;
      return;
    }
  }
  api->StartFrameCapture(nullptr, nullptr);
  REXLOG_INFO("REACH_RDCAPTURE: capture started at t={:.2f}s", now);
  swaps = 1;
}

}  // namespace

// void VdSwap(buffer_ptr, fetch_ptr, unk2, unk3, unk4, frontbuffer_ptr,
//             texture_format_ptr, color_space_ptr, width_ptr, height_ptr)
extern "C" REX_FUNC(__imp__VdSwap) {
  static GuestFunc sdk = reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, "__imp__VdSwap"));
  static const auto start = std::chrono::steady_clock::now();
  const uint32_t fetch_addr = ctx.r4.u32;

  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  MaybeTriggerRenderDocCapture(elapsed);
  MaybeInvalidatePhysicalMemory(elapsed);
  Schedule& schedule = GetSchedule();
  static const char* dump_trigger = std::getenv("REACH_FRAMEDUMP_TRIGGER");
  static int dump_polls = 0;
  if (dump_trigger && *dump_trigger && fetch_addr && ++dump_polls % 10 == 0 &&
      std::remove(dump_trigger) == 0) {
    DumpFrontBuffer(base, fetch_addr, elapsed, schedule.dir);
  }
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
