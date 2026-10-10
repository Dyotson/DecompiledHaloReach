// reach - the SDK's own implementation of an import we override (see sdk_import.h).

#include "sdk_import.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace reach {

GuestFunc SdkImport(const char* name) {
#ifdef _WIN32
  static HMODULE runtime = GetModuleHandleA("rexruntime.dll");
  return runtime ? reinterpret_cast<GuestFunc>(GetProcAddress(runtime, name)) : nullptr;
#else
  return reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, name));
#endif
}

}  // namespace reach
