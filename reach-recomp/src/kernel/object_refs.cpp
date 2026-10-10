// reach - ObDereferenceObject that releases the object the guest means, and XAM task
// threads that are freed when they end.
//
// The runtime finds the object behind a guest pointer through the handle it stashed in
// the object's dispatcher header (+8 'REX\0', +12 handle), hands out the lowest free
// handle again at once, and keeps one reference count per handle for the guest's handles
// and its own references (a thread holds its handle until it exits).
//
// Reach's XEnumerate for content aggregate enumerators (XAPI code at 0x828058B8) runs every
// call as a XamTaskSchedule thread whose body (0x828057B0) ends with ObDereferenceObject on
// the enumerator object it got from XamGetPrivateEnumStructureFromHandle. That object
// (X_KENUMERATOR) has no dispatcher header: its close message and user index overwrite the
// stashed handle. So the runtime's ObDereferenceObject binds a new event to the
// enumerator's memory (app id 0xFE leaves type 0 in the first byte), releases that event at
// once and leaves its dead handle there, and every later dereference of the enumerator
// releases whatever object took that handle since: normally the task thread itself, at
// times another task's thread that is still running. Loading a Firefight mission runs this
// enumeration a few thousand times, so task threads were freed while running (they then
// fault forever in XThread::Exit or in their start code, and whoever waits for them waits
// forever) or never freed (the guest heap ran out of thread blocks and XAM tasks could no
// longer be created), and Firefight froze while loading a mission.
//
// - ObDereferenceObject finds the enumerator through the pointer
//   XamGetPrivateEnumStructureFromHandle returned, uses a stashed handle only if its object
//   owns the pointer, and ignores pointers no runtime object owns instead of making a new
//   object for them.
// - XamTaskSchedule creates the task thread as the runtime does but drops the handle
//   reference creation gave it (the guest only gets a dummy task handle), so the thread
//   object is freed when the task returns.
//
// REACH_SDK_OBJECT_REFS=1 restores the runtime's functions; with REACH_OBJTRACE=1 as well it
// logs what each such dereference releases.

#include "../platform/guest_memory.h"
#include "../platform/sdk_import.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

using reach::GuestPtr;
using rex::system::object_ref;
using rex::system::XObject;
using rex::system::XThread;

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

bool EnvOn(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && *v != '0';
}
bool UseSdk() {
  static const bool on = EnvOn("REACH_SDK_OBJECT_REFS");
  return on;
}
bool Trace() {
  static const bool on = EnvOn("REACH_OBJTRACE");
  return on;
}
// The first 200 messages of a kind, then every 1000th.
bool Sample(std::atomic<uint64_t>& count) {
  const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  return n < 200 || n % 1000 == 0;
}

GuestFunc Sdk(const char* name) { return reach::SdkImport(name); }

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Return addresses up the guest stack, starting with the import's caller.
std::string Backtrace(const PPCContext& ctx, const uint8_t* base) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), " %08X", uint32_t(ctx.lr));
  std::string out = buf;
  uint32_t frame = ctx.r1.u32;
  for (int i = 0; i < 8 && frame; ++i) {
    const uint32_t parent = Load32(GuestPtr(base, frame));
    if (parent <= frame || parent - frame > 0x10000 || (parent & 7)) break;
    const uint32_t lr = Load32(GuestPtr(base, parent - 8));
    if (lr < 0x82000000 || lr >= 0x8C000000) break;
    std::snprintf(buf, sizeof(buf), " %08X", lr);
    out += buf;
    if (!(parent & 0xFFFF)) break;
    frame = parent;
  }
  return out;
}

// XObject::StashHandle's signature at +8 of a bound object.
constexpr uint32_t kSignature = 0x52455800;  // 'REX\0'

// Enumerator objects handed out by XamGetPrivateEnumStructureFromHandle.
std::mutex enumerators_mutex;
std::unordered_map<uint32_t, uint32_t> enumerators;  // guest object -> handle

object_ref<XObject> EnumeratorByGuestObject(uint32_t ptr) {
  uint32_t handle;
  {
    std::lock_guard<std::mutex> lock(enumerators_mutex);
    auto it = enumerators.find(ptr);
    if (it == enumerators.end()) return {};
    handle = it->second;
  }
  auto e = rex::system::kernel_state()->object_table()->LookupObject<rex::system::XEnumerator>(
      handle);
  if (!e || e->guest_object() != ptr) return {};
  return object_ref<XObject>(e.release());
}

}  // namespace

// void ObDereferenceObject(void* object)
extern "C" REX_FUNC(__imp__ObDereferenceObject) {
  static GuestFunc sdk = Sdk("__imp__ObDereferenceObject");
  const uint32_t ptr = ctx.r3.u32;
  if (!ptr || ptr == 0xDEADF00D) {  // the runtime's dummy object pointer
    sdk(ctx, base);
    return;
  }
  uint8_t* header = GuestPtr(base, ptr);
  const bool bound = Load32(header + 8) == kSignature;
  object_ref<XObject> object;
  if (bound) object = XObject::GetNativeObject(rex::system::kernel_state(), header);
  // An object the runtime bound to guest memory has no guest object of its own; one it
  // allocated (XObject::CreateNative) has this pointer.
  const bool owner = object && (!object->guest_object() || object->guest_object() == ptr);

  if (UseSdk()) {
    static std::atomic<uint64_t> traced{0};
    if (Trace() && !owner && Sample(traced)) {
      REXLOG_WARN(
          "OBJTRACE ObDereferenceObject({:08X}), type byte {:02X}: {}; from{}", ptr, header[0],
          !bound   ? std::string("no stashed handle, the runtime makes a new object")
          : object ? fmt::format("releases {:08X}, another object (type {}, guest object {:08X})",
                                 object->handle(), uint32_t(object->type()),
                                 object->guest_object())
                   : std::string("stale handle"),
          Backtrace(ctx, base));
    }
    sdk(ctx, base);
    return;
  }

  ctx.r3.u64 = 0;
  if (!owner) object = bound ? nullptr : EnumeratorByGuestObject(ptr);
  if (!object) {
    static std::atomic<uint64_t> warned{0};
    if (Sample(warned)) {
      REXLOG_WARN("ObDereferenceObject({:08X}): no runtime object owns it (type byte {:02X}, {}); "
                  "ignored. From{}",
                  ptr, header[0], bound ? "stale handle" : "no stashed handle",
                  Backtrace(ctx, base));
    }
    return;
  }
  object->ReleaseHandle();
}

// DWORD XamGetPrivateEnumStructureFromHandle(HANDLE enumerator, void** object)
extern "C" REX_FUNC(__imp__XamGetPrivateEnumStructureFromHandle) {
  static GuestFunc sdk = Sdk("__imp__XamGetPrivateEnumStructureFromHandle");
  const uint32_t handle = ctx.r3.u32, out = ctx.r4.u32;
  const uint32_t lr = ctx.lr;
  const std::string bt = Trace() ? Backtrace(ctx, base) : std::string();
  sdk(ctx, base);
  if (Trace()) {
    static std::atomic<uint64_t> n{0};
    if (Sample(n)) REXLOG_WARN("ENUMTRACE GetPrivateEnum({:08X}) lr {:08X} from{}", handle, lr, bt);
  }
  if (ctx.r3.u32 || !out) return;
  const uint32_t ptr = Load32(GuestPtr(base, out));
  std::lock_guard<std::mutex> lock(enumerators_mutex);
  if (enumerators.size() > 4096) enumerators.clear();  // mostly closed enumerators by then
  enumerators[ptr] = handle;
}

// DWORD XamContentCreateEnumerator(user, device, type, flags, items, DWORD* buffer_size,
//                                  HANDLE* enumerator)  (trace only)
extern "C" REX_FUNC(__imp__XamContentCreateEnumerator) {
  static GuestFunc sdk = Sdk("__imp__XamContentCreateEnumerator");
  const uint32_t in[7] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                          ctx.r7.u32, ctx.r8.u32, ctx.r9.u32};
  const uint32_t lr = ctx.lr;
  sdk(ctx, base);
  if (Trace()) {
    static std::atomic<uint64_t> n{0};
    if (Sample(n)) {
      REXLOG_WARN("ENUMTRACE CreateEnumerator(user {:X}, device {:X}, type {:X}, flags {:X}, "
                  "items {}) -> {:08X} handle {:08X} lr {:08X}",
                  in[0], in[1], in[2], in[3], in[4], ctx.r3.u32,
                  in[6] ? Load32(GuestPtr(base, in[6])) : 0, lr);
    }
  }
}

// DWORD XamTaskSchedule(void* callback, XTASK_MESSAGE* message, DWORD* unknown, HANDLE* task)
extern "C" REX_FUNC(__imp__XamTaskSchedule) {
  static GuestFunc sdk = Sdk("__imp__XamTaskSchedule");
  if (UseSdk()) {
    sdk(ctx, base);
    return;
  }
  const uint32_t callback = ctx.r3.u32, message = ctx.r4.u32, task = ctx.r6.u32;
  auto* kernel = rex::system::kernel_state();
  uint32_t stack_size = kernel->GetExecutableModule()->stack_size();
  stack_size = std::max(0x4000u, (stack_size + 0xFFF) & 0xFFFFF000);  // as the runtime
  auto thread =
      object_ref<XThread>(new XThread(kernel, stack_size, 0, callback, message, 0, true));
  // The task handle goes in first: the task may finish, and free its message (which holds
  // the handle for Reach), before Create returns.
  if (task) Store32(GuestPtr(base, task), 12345);  // the runtime's dummy task handle
  const uint32_t result = thread->Create();
  if (int32_t(result) < 0) {
    REXLOG_ERROR("XamTaskSchedule({:08X}): thread creation failed: {:08X}", callback, result);
    if (task) Store32(GuestPtr(base, task), 0);  // Reach frees its message when this is 0
    ctx.r3.u64 = result;
    return;
  }
  // The thread keeps its own reference until it exits.
  thread->ReleaseHandle();
  ctx.r3.u64 = 0;
}

namespace reach {
// Called from XMsgCompleteIORequest. Reach's XEnumerate task (0x828057B0) ends with that
// call, and nothing frees the task's message block (XamAlloc'd by 0x828055B8, freed by Reach
// only when XamTaskSchedule fails): the console's task runtime frees it when the callback
// returns, the SDK's does not. Each enumeration pass leaked a block of the system heap
// (one page or more each), and a few thousand passes while loading a mission exhausted it.
// The message is still in r28 at that call (the task's argument), and nothing reads it
// after the call. Also releases the reference the message holds on the overlapped's event.
void XamTaskMessageDone(PPCContext& ctx, uint8_t* base, uint32_t lr) {
  if (lr != 0x828058AC || UseSdk()) return;
  const uint32_t message = ctx.r28.u32;
  if (message < 0x10000) return;
  const uint8_t* m = GuestPtr(base, message);
  const uint32_t event_object = Load32(m + 16);
  PPCContext c = ctx;
  if (event_object) {
    c.r3.u64 = event_object;
    __imp__ObDereferenceObject(c, base);
  }
  static GuestFunc free_block = Sdk("__imp__XamFree");
  c = ctx;
  c.r3.u64 = message;
  free_block(c, base);
  static std::atomic<uint64_t> freed{0};
  if (Trace() && Sample(freed)) REXLOG_WARN("XAM task message {:08X} freed (#{})", message, freed.load());
}
}  // namespace reach
