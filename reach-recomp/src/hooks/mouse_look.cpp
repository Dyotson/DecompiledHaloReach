// reach - mouse look straight into player control (hints/mouse_look.toml, docs/input.md).
//
// sub_8247C978 is per-local-player control: r3 = player, r4 = controller (kept in r15),
// f1 = tick length, r7 = the unit-control output (kept in r27). It turns the abstracted right
// stick (input_abstraction, sub_820E8A68) into yaw/pitch rates with the Look Sensitivity
// setting and stick acceleration, divides them by the zoom magnification, adds aim assist
// and stores rate * tick at output+0x14 (yaw, left positive) and +0x18 (pitch, up positive).
// Mouse counts are an angle, not a rate, so they are added to those two stores and skip the
// stick shaping. The hooks only run on the branch where the player has look control, so
// cinematics, dead players and paused games get no mouse motion.

#include "../input/kbm.h"

#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdint>
#include <cstring>

namespace {

struct Look {
  float zoom = 1.0f;  // 1 / magnification
  bool flying = false;
};
Look g_look[4];

// Per-controller settings (0x8382FE28 + 0xA8 * controller, filled from the profile).
constexpr uint32_t kLookInverted = 0x8382FEC2;
constexpr uint32_t kFlightInverted = 0x8382FEC3;
constexpr uint32_t kSettingsStride = 0xA8;

uint8_t* Base() { return rex::system::kernel_state()->memory()->virtual_membase(); }

float LoadFloat(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void StoreFloat(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

}  // namespace

// 0x8247D6D8, right after sub_82484108 ("is the unit flying", r3), which picks the flight
// pitch input. The player has look control this tick.
void ReachMouseLookBegin(PPCRegister& r15, PPCRegister& r3) {
  const uint32_t ctrl = r15.u32;
  if (ctrl > 3) return;
  g_look[ctrl].zoom = 1.0f;
  g_look[ctrl].flying = (r3.u32 & 0xFF) != 0;
}

// 0x8247D8B8: zoomed in, f0 = 1 / magnification about to scale the stick rates.
void ReachMouseLookZoom(PPCRegister& r15, PPCRegister& f0) {
  const uint32_t ctrl = r15.u32;
  if (ctrl > 3) return;
  const double zoom = f0.f64;
  g_look[ctrl].zoom = zoom > 0.0 && zoom <= 1.0 ? float(zoom) : 1.0f;
}

// 0x8247DD20, after the yaw/pitch change of the tick is stored at r27+0x14/+0x18. The
// keyboard and mouse device is guest user 0, controller 0.
void ReachMouseLookApply(PPCRegister& r15, PPCRegister& r27) {
  const uint32_t ctrl = r15.u32;
  if (ctrl != 0) return;
  float yaw = 0, pitch = 0;
  if (!reach::kbm::TakeMouseLook(yaw, pitch)) return;
  uint8_t* base = Base();
  bool inverted = base[kLookInverted + ctrl * kSettingsStride] != 0;
  if (g_look[ctrl].flying && base[kFlightInverted + ctrl * kSettingsStride]) inverted = !inverted;
  if (inverted) pitch = -pitch;
  uint8_t* out = base + r27.u32;
  StoreFloat(out + 0x14, LoadFloat(out + 0x14) + yaw * g_look[ctrl].zoom);
  StoreFloat(out + 0x18, LoadFloat(out + 0x18) + pitch * g_look[ctrl].zoom);
}
