// reach - optional "nobody signed in" mode for the XAM user APIs.
//
// The ReXGlue runtime signs in a default profile ("User") on controller 0.
// Xenia without a created profile has nobody signed in, so the game takes a
// different menu path there. REACH_NO_SIGNIN=1 makes the sign-in queries
// report no user, matching that setup for differential debugging. Without the
// variable every call is forwarded to the SDK implementation unchanged.
//
// The signed-in profile's XUID and gamertag come from identity.cpp when it has
// one (REACH_XUID / REACH_GAMERTAG, or the Live identity with REACH_SERVER); the
// SDK gives every instance 0xB13EBABEBABEBABE "User".
//
// With a Reach Live server and Live sign-in (identity.h: LiveSignin), user 0 is signed in to Live:
// the sign-in state is 2, the info has the Live-enabled flag and every privilege
// is granted.

#include "../platform/guest_memory.h"
#include "../platform/sdk_import.h"
#include "identity.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>


#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using reach::GuestPtr;

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kErrorNoSuchUser = 0x00000525;  // X_ERROR_NO_SUCH_USER
constexpr uint32_t kSigninStateNotSignedIn = 0;
constexpr uint32_t kSigninStateSignedInToLive = 2;

bool NoSignin() {
  static const bool enabled = [] {
    const char* v = std::getenv("REACH_NO_SIGNIN");
    bool on = v && *v && *v != '0';
    if (on) REXLOG_INFO("REACH_NO_SIGNIN: reporting no signed-in users");
    return on;
  }();
  return enabled;
}

GuestFunc Sdk(const char* name) { return reach::SdkImport(name); }

uint64_t XuidOverride() { return reach::IdentityXuid(); }

const char* GamertagOverride() {
  const std::string& name = reach::IdentityGamertag();
  return name.empty() ? nullptr : name.c_str();
}

void StoreXuid(uint8_t* p, uint64_t xuid) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(xuid >> (56 - 8 * i));
}

void StoreName(uint8_t* p, uint32_t size, const char* name) {
  if (!size) return;
  uint32_t n = std::min<uint32_t>(uint32_t(std::strlen(name)), size - 1);
  std::memcpy(p, name, n);
  p[n] = 0;
}

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
extern "C" REX_FUNC(__imp__XamUserGetSigninState) {
  const uint32_t user_index = ctx.r3.u32;
  if (NoSignin()) {
    ctx.r3.u64 = kSigninStateNotSignedIn;
    return;
  }
  static GuestFunc sdk = Sdk("__imp__XamUserGetSigninState");
  if (sdk) sdk(ctx, base);
  if (user_index == 0 && ctx.r3.u32 != kSigninStateNotSignedIn && reach::LiveSignin()) {
    ctx.r3.u64 = kSigninStateSignedInToLive;
  }
}

// DWORD XamUserGetSigninInfo(DWORD user_index, DWORD flags, X_USER_SIGNIN_INFO* info)
extern "C" REX_FUNC(__imp__XamUserGetSigninInfo) {
  const uint32_t user_index = ctx.r3.u32, info = ctx.r5.u32;
  if (NoSignin()) {
    if (info) std::memset(GuestPtr(base, info), 0, 0x28);
    ctx.r3.u64 = kErrorNoSuchUser;
    return;
  }
  static GuestFunc sdk = Sdk("__imp__XamUserGetSigninInfo");
  if (sdk) sdk(ctx, base);
  if (ctx.r3.u32 != 0 || user_index != 0 || !info) return;
  // X_USER_SIGNIN_INFO: xuid at 0, name[16] at 24.
  if (XuidOverride()) StoreXuid(GuestPtr(base, info), XuidOverride());
  if (GamertagOverride()) StoreName(GuestPtr(base, info + 24), 16, GamertagOverride());
  if (reach::LiveSignin()) {
    // dwInfoFlags (+8): XUSER_INFO_FLAG_LIVE_ENABLED; UserSigninState (+12).
    base[info + 11] |= 1;
    std::memset(GuestPtr(base, info + 12), 0, 4);
    base[info + 15] = kSigninStateSignedInToLive;
  }
}

// DWORD XamUserGetXUID(DWORD user_index, DWORD type, XUID* xuid)
extern "C" REX_FUNC(__imp__XamUserGetXUID) {
  const uint32_t user_index = ctx.r3.u32, xuid = ctx.r5.u32;
  if (NoSignin()) {
    if (xuid) std::memset(GuestPtr(base, xuid), 0, 8);
    ctx.r3.u64 = kErrorNoSuchUser;
    return;
  }
  static GuestFunc sdk = Sdk("__imp__XamUserGetXUID");
  if (sdk) sdk(ctx, base);
  if (ctx.r3.u32 == 0 && user_index == 0 && xuid && XuidOverride()) {
    StoreXuid(GuestPtr(base, xuid), XuidOverride());
  }
}

// DWORD XamUserGetName(DWORD user_index, char* buffer, DWORD buffer_size)
extern "C" REX_FUNC(__imp__XamUserGetName) {
  const uint32_t user_index = ctx.r3.u32, buffer = ctx.r4.u32, size = ctx.r5.u32;
  if (NoSignin()) {
    if (buffer && size) base[buffer] = 0;
    ctx.r3.u64 = kErrorNoSuchUser;
    return;
  }
  static GuestFunc sdk = Sdk("__imp__XamUserGetName");
  if (sdk) sdk(ctx, base);
  if (ctx.r3.u32 == 0 && user_index == 0 && buffer && GamertagOverride()) {
    StoreName(GuestPtr(base, buffer), size, GamertagOverride());
  }
}

// DWORD XamUserCheckPrivilege(DWORD user_index, DWORD privilege, BOOL* result)
// The SDK denies every privilege. A Live player may do everything: multiplayer,
// communications, user-created content (file share), profile viewing, presence.
extern "C" REX_FUNC(__imp__XamUserCheckPrivilege) {
  const uint32_t user_index = ctx.r3.u32, result = ctx.r5.u32;
  if (NoSignin()) {
    if (result) std::memset(GuestPtr(base, result), 0, 4);
    ctx.r3.u64 = kErrorNoSuchUser;
    return;
  }
  static GuestFunc sdk = Sdk("__imp__XamUserCheckPrivilege");
  if (sdk) sdk(ctx, base);
  if (ctx.r3.u32 == 0 && result && (user_index == 0 || user_index == 0xFF) &&
      reach::LiveSignin()) {
    std::memset(GuestPtr(base, result), 0, 4);
    base[result + 3] = 1;
  }
}
