// reach - correct text from the headless keyboard (--headless=true).
//
// With --headless the SDK answers XamShowKeyboardUI with the default text, but
// it copies that text with copy_and_swap from MappedPtr<char16_t>::value(),
// which views the guest's big-endian string without swapping it first (Xenia's
// lpu16string_t::value() loads and swaps). The game gets the text back
// little-endian: a Forge map saved as "Sword Base" is named with ten CJK
// glyphs, in the lobby and in the saved file. This override copies the guest
// string as it is. Without --headless every call goes to the SDK, whose ImGui
// keyboard converts correctly.

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/flags.h>

#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <functional>

namespace rex::kernel::xam {
// Exported by librexruntime but not declared in a public header.
uint32_t xeXamDispatchHeadless(std::function<uint32_t()> run_callback, uint32_t overlapped);
}  // namespace rex::kernel::xam

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kErrorInvalidParameter = 0x00000057;  // X_ERROR_INVALID_PARAMETER

}  // namespace

// DWORD XamShowKeyboardUI(DWORD user_index, DWORD flags, LPCWSTR default_text,
//                         LPCWSTR title, LPCWSTR description, LPWSTR buffer,
//                         DWORD buffer_length, XOVERLAPPED* overlapped)
extern "C" REX_FUNC(__imp__XamShowKeyboardUI) {
  if (!REXCVAR_GET(headless)) {
    static GuestFunc sdk =
        reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, "__imp__XamShowKeyboardUI"));
    if (sdk) sdk(ctx, base);
    return;
  }
  const uint32_t default_text = ctx.r5.u32;
  const uint32_t buffer = ctx.r8.u32;
  const uint32_t buffer_length = ctx.r9.u32;  // in characters
  const uint32_t overlapped = ctx.r10.u32;
  if (!buffer) {
    ctx.r3.u64 = kErrorInvalidParameter;
    return;
  }
  auto run = [base, default_text, buffer, buffer_length]() -> uint32_t {
    auto* dest = reinterpret_cast<uint16_t*>(base + buffer);
    std::memset(dest, 0, size_t(buffer_length) * 2);
    if (default_text) {
      // Both strings are big-endian guest memory; a NUL is zero either way.
      const auto* src = reinterpret_cast<const uint16_t*>(base + default_text);
      for (uint32_t i = 0; i + 1 < buffer_length && src[i]; ++i) dest[i] = src[i];
    }
    return 0;  // X_ERROR_SUCCESS
  };
  ctx.r3.u64 = rex::kernel::xam::xeXamDispatchHeadless(run, overlapped);
}
