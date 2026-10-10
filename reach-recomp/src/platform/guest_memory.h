// reach - host pointers for guest addresses, as the recompiled code computes them.
//
// Guest memory is `base + address`, except that on Windows the physical heap at
// 0xE0000000 and up sits 0x1000 bytes further on (the 64 KB mapping granularity
// rounds that view's 4 KB offset away; the generated code adds it back with
// REX_PHYS_HOST_OFFSET and the runtime with rex::memory::GuestPtr). Code here uses
// GuestPtr(base, address) for every guest pointer: a raw `base + address` reads other
// memory than the game and the runtime there. The runtime allocates its own guest
// objects in that heap (VdHSIOCalibrationLock, a critical section, is at 0xFFCAB000).

#pragma once

#include <rex/system/xmemory.h>

#include <cstdint>

namespace reach {

inline uint8_t* GuestPtr(uint8_t* base, uint32_t address) {
  return rex::memory::GuestPtr(base, address);
}

inline const uint8_t* GuestPtr(const uint8_t* base, uint32_t address) {
  return rex::memory::GuestPtr(const_cast<uint8_t*>(base), address);
}

}  // namespace reach
