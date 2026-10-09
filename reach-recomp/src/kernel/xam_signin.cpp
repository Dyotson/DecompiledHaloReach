// reach - optional "nobody signed in" mode for the XAM user APIs.
//
// The ReXGlue runtime signs in a default profile ("User") on controller 0.
// Xenia without a created profile has nobody signed in, so the game takes a
// different menu path there. REACH_NO_SIGNIN=1 makes the sign-in queries
// report no user, matching that setup for differential debugging. Without the
// variable every call is forwarded to the SDK implementation unchanged.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kErrorNoSuchUser = 0x00000525;  // X_ERROR_NO_SUCH_USER
constexpr uint32_t kSigninStateNotSignedIn = 0;

bool NoSignin() {
  static const bool enabled = [] {
    const char* v = std::getenv("REACH_NO_SIGNIN");
    bool on = v && *v && *v != '0';
    if (on) REXLOG_INFO("REACH_NO_SIGNIN: reporting no signed-in users");
    return on;
  }();
  return enabled;
}

GuestFunc Sdk(const char* name) { return reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, name)); }

}  // namespace

#define REACH_FORWARD_OR(name, body)                       \
  extern "C" REX_FUNC(__imp__##name) {                     \
    if (NoSignin()) {                                      \
      body;                                                \
      return;                                              \
    }                                                      \
    static GuestFunc sdk = Sdk("__imp__" #name);           \
    if (sdk) sdk(ctx, base);                               \
  }

// DWORD XamUserGetSigninState(DWORD user_index)
REACH_FORWARD_OR(XamUserGetSigninState, ctx.r3.u64 = kSigninStateNotSignedIn)

// DWORD XamUserGetSigninInfo(DWORD user_index, DWORD flags, X_USER_SIGNIN_INFO* info)
REACH_FORWARD_OR(XamUserGetSigninInfo, {
  if (ctx.r5.u32) std::memset(base + ctx.r5.u32, 0, 0x28);
  ctx.r3.u64 = kErrorNoSuchUser;
})

// DWORD XamUserGetXUID(DWORD user_index, DWORD type, XUID* xuid)
REACH_FORWARD_OR(XamUserGetXUID, {
  if (ctx.r5.u32) std::memset(base + ctx.r5.u32, 0, 8);
  ctx.r3.u64 = kErrorNoSuchUser;
})

// DWORD XamUserGetName(DWORD user_index, char* buffer, DWORD buffer_size)
REACH_FORWARD_OR(XamUserGetName, {
  if (ctx.r4.u32 && ctx.r5.u32) base[ctx.r4.u32] = 0;
  ctx.r3.u64 = kErrorNoSuchUser;
})

// DWORD XamUserCheckPrivilege(DWORD user_index, DWORD privilege, BOOL* result)
REACH_FORWARD_OR(XamUserCheckPrivilege, {
  if (ctx.r5.u32) std::memset(base + ctx.r5.u32, 0, 4);
  ctx.r3.u64 = kErrorNoSuchUser;
})
