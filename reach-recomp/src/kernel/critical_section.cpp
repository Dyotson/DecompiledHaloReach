// reach - critical sections that cannot lose a wakeup.
//
// The SDK implements RtlEnterCriticalSection / RtlLeaveCriticalSection as Xenia does:
// a lock count in the guest X_RTL_CRITICAL_SECTION plus the auto-reset event in its
// dispatcher header, which the runtime binds to a host event through a handle it stashes
// in the header itself (wait_list_flink = 'REX\0', wait_list_blink = handle). A solo
// Firefight game froze after four minutes with the main thread waiting forever in
// RtlEnterCriticalSection on one of the game's named locks (table at 0x8394DFD0, entry 14)
// that nobody held: lock count 0, no owner. Its header's handle (0xF8000250) was also
// stashed in a heap object at 0x30655018, so two guest objects shared one host event and
// the release's signal never reached the waiter.
//
// Here the guest-visible fields (lock count at +0x10, recursion count at +0x14, owning
// thread at +0x18, spin count / 256 in the header's second byte) keep their meaning, but
// a contended waiter blocks on a host semaphore keyed by the critical section's address,
// which no guest memory can alias. REACH_SDK_CRITICAL_SECTIONS=1 restores the SDK's.

#include "../platform/sdk_import.h"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xthread.h>


#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

bool UseSdk() {
  static const bool on = [] {
    const char* v = std::getenv("REACH_SDK_CRITICAL_SECTIONS");
    return v && *v && *v != '0';
  }();
  return on;
}

GuestFunc Sdk(const char* name) { return reach::SdkImport(name); }

// Guest memory is big-endian; the lock count is changed atomically in place.
int32_t AtomicAdd(uint8_t* p, int32_t delta) {
  auto* word = reinterpret_cast<uint32_t*>(p);
  uint32_t old_raw = __atomic_load_n(word, __ATOMIC_SEQ_CST);
  for (;;) {
    const int32_t value = int32_t(__builtin_bswap32(old_raw)) + delta;
    const uint32_t new_raw = __builtin_bswap32(uint32_t(value));
    if (__atomic_compare_exchange_n(word, &old_raw, new_raw, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST)) {
      return value;
    }
  }
}

bool AtomicSwap(uint8_t* p, int32_t expected, int32_t desired) {
  auto* word = reinterpret_cast<uint32_t*>(p);
  uint32_t expected_raw = __builtin_bswap32(uint32_t(expected));
  return __atomic_compare_exchange_n(word, &expected_raw, __builtin_bswap32(uint32_t(desired)),
                                     false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Hand-offs from a releasing thread to waiters of one critical section: a counting
// semaphore, so a release that comes before the waiter sleeps is not lost.
struct Waiters {
  std::mutex mutex;
  std::condition_variable cv;
  uint32_t permits = 0;
};

Waiters& WaitersOf(uint32_t cs) {
  static std::mutex table_mutex;
  static std::unordered_map<uint32_t, std::unique_ptr<Waiters>> table;
  std::lock_guard<std::mutex> lock(table_mutex);
  auto& slot = table[cs];
  if (!slot) slot = std::make_unique<Waiters>();
  return *slot;
}

constexpr uint32_t kLockCount = 0x10, kRecursion = 0x14, kOwner = 0x18;

uint32_t CurrentThread() { return rex::system::XThread::GetCurrentThread()->guest_object(); }

void Enter(uint8_t* base, uint32_t cs) {
  uint8_t* p = base + cs;
  const uint32_t self = CurrentThread();
  if (Load32(p + kOwner) == self) {
    AtomicAdd(p + kLockCount, 1);
    Store32(p + kRecursion, Load32(p + kRecursion) + 1);
    return;
  }
  for (uint32_t spin = uint32_t(p[1]) * 256 + 1; spin; --spin) {
    if (AtomicSwap(p + kLockCount, -1, 0)) {
      Store32(p + kOwner, self);
      Store32(p + kRecursion, 1);
      return;
    }
  }
  if (AtomicAdd(p + kLockCount, 1) != 0) {
    Waiters& w = WaitersOf(cs);
    std::unique_lock<std::mutex> lock(w.mutex);
    w.cv.wait(lock, [&w] { return w.permits > 0; });
    --w.permits;
  }
  Store32(p + kOwner, self);
  Store32(p + kRecursion, 1);
}

void Leave(uint8_t* base, uint32_t cs) {
  uint8_t* p = base + cs;
  const int32_t recursion = int32_t(Load32(p + kRecursion)) - 1;
  Store32(p + kRecursion, uint32_t(recursion > 0 ? recursion : 0));
  if (recursion > 0) {
    AtomicAdd(p + kLockCount, -1);
    return;
  }
  Store32(p + kOwner, 0);
  if (AtomicAdd(p + kLockCount, -1) != -1) {  // someone is waiting: hand the lock over
    Waiters& w = WaitersOf(cs);
    {
      std::lock_guard<std::mutex> lock(w.mutex);
      ++w.permits;
    }
    w.cv.notify_one();
  }
}

uint32_t TryEnter(uint8_t* base, uint32_t cs) {
  uint8_t* p = base + cs;
  const uint32_t self = CurrentThread();
  if (AtomicSwap(p + kLockCount, -1, 0)) {
    Store32(p + kOwner, self);
    Store32(p + kRecursion, 1);
    return 1;
  }
  if (Load32(p + kOwner) == self) {
    AtomicAdd(p + kLockCount, 1);
    Store32(p + kRecursion, Load32(p + kRecursion) + 1);
    return 1;
  }
  return 0;
}

}  // namespace

// void RtlEnterCriticalSection(RTL_CRITICAL_SECTION* cs)
extern "C" REX_FUNC(__imp__RtlEnterCriticalSection) {
  if (UseSdk()) {
    static GuestFunc sdk = Sdk("__imp__RtlEnterCriticalSection");
    sdk(ctx, base);
    return;
  }
  Enter(base, ctx.r3.u32);
}

// void RtlLeaveCriticalSection(RTL_CRITICAL_SECTION* cs)
extern "C" REX_FUNC(__imp__RtlLeaveCriticalSection) {
  if (UseSdk()) {
    static GuestFunc sdk = Sdk("__imp__RtlLeaveCriticalSection");
    sdk(ctx, base);
    return;
  }
  Leave(base, ctx.r3.u32);
}

// BOOL RtlTryEnterCriticalSection(RTL_CRITICAL_SECTION* cs)
extern "C" REX_FUNC(__imp__RtlTryEnterCriticalSection) {
  if (UseSdk()) {
    static GuestFunc sdk = Sdk("__imp__RtlTryEnterCriticalSection");
    sdk(ctx, base);
    return;
  }
  ctx.r3.u64 = TryEnter(base, ctx.r3.u32);
}
