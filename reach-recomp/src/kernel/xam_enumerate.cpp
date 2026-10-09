// reach - XamEnumerate with the console's completion codes for asynchronous calls.
//
// When an overlapped XamEnumerate runs out of items, the console completes it
// with ERROR_FUNCTION_FAILED and puts the real error (ERROR_NO_MORE_FILES) in
// the extended error as an HRESULT, as Xenia's xam_enum.cc does. The SDK
// completes it with the raw error instead. Reach's content enumeration
// (sub_825F4418, success test sub_8226AD40) accepts only the console's form,
// so it treated every finished enumeration as a failure and restarted it about
// five times a second; while it ran, the per-frame network join update
// (sub_82200A00) skipped pending joins, so System Link joins never started.
// Synchronous calls (no XOVERLAPPED) go to the SDK unchanged.

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>

#include <dlfcn.h>

#include <cstdint>

namespace {
using GuestFunc = void (*)(PPCContext&, uint8_t*);
}  // namespace

// DWORD XamEnumerate(HANDLE enumerator, DWORD flags, void* buffer, DWORD buffer_size,
//                    DWORD* items_returned, XOVERLAPPED* overlapped)
extern "C" REX_FUNC(__imp__XamEnumerate) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t buffer = ctx.r5.u32;
  const uint32_t overlapped = ctx.r8.u32;
  if (!overlapped) {
    static GuestFunc sdk = reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, "__imp__XamEnumerate"));
    if (sdk) sdk(ctx, base);
    return;
  }

  using namespace rex;
  using namespace rex::system;
  auto enumerator = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!enumerator) {
    ctx.r3.u64 = X_ERROR_INVALID_HANDLE;
    return;
  }
  auto run = [enumerator, buffer, base](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    uint32_t item_count = 0;
    X_RESULT result = buffer ? enumerator->WriteItems(buffer, base + buffer, &item_count)
                             : X_RESULT(X_ERROR_INVALID_PARAMETER);
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    return result ? X_RESULT(X_ERROR_FUNCTION_FAILED) : X_RESULT(X_ERROR_SUCCESS);
  };
  REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped);
  ctx.r3.u64 = X_ERROR_IO_PENDING;
}
