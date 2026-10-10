// reach - Xbox Live services from a Reach Live server: the XAM messages a title sends
// to the XLiveBase (0xFC) and XGI (0xFB) apps, and the logon queries.
//
// With REACH_LIVE_SIGNIN the game believes it is signed in to Xbox Live and asks for:
//
// - XStringVerify (0xFC/0x5000C): the profanity check of user text. Every string is
//   accepted (the SDK failed it, and the game retried every frame).
// - XFriendsCreateEnumerator (0xFC/0x58020): the friends list. Every other player in our
//   room on the server is a friend, with the presence it reported (online, playing,
//   joinable session). The SDK failed it.
// - XSessionCreate (0xFB/0xB0010) for a session we host: the SDK leaves XSESSION_INFO
//   empty. We fill it (an online peer session id, our XNADDR, a random key) and, for a
//   presence session, publish it, so friends see it as joinable. Delete and modify
//   update that.
// - XNetLogonGetTitleID / XNetLogonGetMachineID: the SDK stubs return garbage.
//
// Everything else goes to the SDK. REACH_NETTRACE=1 logs every XMsg call (as net.cpp
// did before) and what we answered. Message layouts follow the Xenia netplay fork
// (AdrianCassar/xenia-canary, BSD), see docs/online_plan.md.

#include "identity.h"
#include "live.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kAppXgi = 0xFB;
constexpr uint32_t kAppXLiveBase = 0xFC;
constexpr uint32_t kTitleId = 0x4D53085B;
constexpr uint32_t kNotOurs = 0xFFFFFFFF;  // handler result: let the SDK answer
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kInvalidArg = 0x80070057;  // X_E_INVALIDARG
constexpr uint32_t kIoPending = 0x3E5;

// X_ONLINE_FRIENDSTATE flags.
constexpr uint32_t kFriendOnline = 0x1;
constexpr uint32_t kFriendPlaying = 0x2;
constexpr uint32_t kFriendJoinable = 0x10;
constexpr uint32_t kFriendJoinableFriendsOnly = 0x100;

// XSESSION_CREATE flags.
constexpr uint32_t kSessionHost = 0x1;
constexpr uint32_t kSessionUsesPresence = 0x2;
constexpr uint32_t kSessionJoinViaPresenceDisabled = 0x200;
constexpr uint32_t kSessionJoinViaPresenceFriendsOnly = 0x800;

constexpr size_t kSessionInfoSize = 0x3C;  // XSESSION_INFO
constexpr size_t kFriendSize = 0xC4;       // X_ONLINE_FRIEND

bool Trace() {
  static const bool on = [] {
    const char* v = std::getenv("REACH_NETTRACE");
    return v && *v && *v != '0' && std::string(v) != "packets";
  }();
  return on;
}

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
void Store64(uint8_t* p, uint64_t v) {
  Store32(p, uint32_t(v >> 32));
  Store32(p + 4, uint32_t(v));
}

uint64_t RandomU64() {
  static std::mutex mutex;
  static std::mt19937_64 rng(std::random_device{}());
  std::lock_guard<std::mutex> lock(mutex);
  return rng();
}

uint64_t FileTimeNow() {
  using namespace std::chrono;
  const uint64_t unix_100ns =
      duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100;
  return unix_100ns + 116444736000000000ull;
}

// The value of argument `index` of an X_ARGUMENT_LIST-style block: 16-byte entries
// {native size u32, pad, value pointer u64}.
uint32_t ArgPointer(const uint8_t* base, uint32_t args, int index) {
  return Load32(base + args + 16 * index + 12);
}

// The presence our player shows when not hosting a joinable session.
void SetIdlePresence() {
  reach::LiveSetPresence(kFriendOnline | kFriendPlaying, nullptr, "Halo: Reach");
}

// --- XLiveBase -------------------------------------------------------------------

// XStringVerify: the message is an XLIVEBASE_ASYNC_MESSAGE whose task holds a
// marshalled request (title id, flags, locale size u16, string count u16 — the wire
// format is little-endian) and a results buffer: STRING_VERIFY_RESPONSE {count u16,
// results pointer u32} followed by one HRESULT per string.
uint32_t XStringVerify(uint8_t* base, uint32_t message) {
  if (!message) return kInvalidArg;
  const uint32_t task = Load32(base + message);
  if (!task) return kInvalidArg;
  const uint32_t request = Load32(base + task + 0x18), request_size = Load32(base + task + 0x1C);
  const uint32_t results = Load32(base + task + 0x2C), results_size = Load32(base + task + 0x30);
  if (!request || request_size < 12 || !results || results_size < 6) return kInvalidArg;
  uint32_t count = base[request + 10] | base[request + 11] << 8;
  count = std::min<uint32_t>(count, (results_size - 6) / 4);
  std::memset(base + results, 0, results_size);
  base[results] = uint8_t(count >> 8);
  base[results + 1] = uint8_t(count);
  Store32(base + results + 2, results + 6);  // all results stay S_OK
  return kSuccess;
}

// XFriendsCreateEnumerator(user, first, count, buffer size out, handle out): arguments
// in an X_ARGUMENT_LIST. Items are X_ONLINE_FRIEND.
uint32_t XFriendsCreateEnumerator(uint8_t* base, uint32_t args) {
  using namespace rex;
  using namespace rex::system;
  if (!args) return kInvalidArg;
  const uint32_t first_ptr = ArgPointer(base, args, 1), count_ptr = ArgPointer(base, args, 2);
  const uint32_t size_out = ArgPointer(base, args, 3), handle_out = ArgPointer(base, args, 4);
  if (!handle_out || !size_out || !count_ptr) return kInvalidArg;
  Store32(base + handle_out, 0);
  const uint32_t first = first_ptr ? Load32(base + first_ptr) : 0;
  const uint32_t count = std::min<uint32_t>(Load32(base + count_ptr), 100);
  if (!count) return kInvalidArg;
  Store32(base + size_out, count * kFriendSize);

  auto e = make_object<XStaticUntypedEnumerator>(REX_KERNEL_STATE(), count, kFriendSize);
  if (XFAILED(e->Initialize(0xFFFFFFFF, kAppXLiveBase, 0x58021, 0x58022, 0))) return kInvalidArg;
  const auto roster = reach::LiveRoster();
  const uint64_t now = FileTimeNow();
  for (size_t i = first; i < roster.size() && e->item_count() < count; ++i) {
    const reach::LiveFriend& f = roster[i];
    uint8_t* item = e->AppendItem();
    std::memset(item, 0, kFriendSize);
    Store64(item, f.xuid);
    std::memcpy(item + 8, f.gamertag.data(), std::min<size_t>(f.gamertag.size(), 15));
    Store32(item + 0x18, f.state);
    std::memcpy(item + 0x1C, f.session, 8);  // session id
    if (f.state & kFriendPlaying) Store32(item + 0x24, kTitleId);
    Store64(item + 0x28, now);
    const size_t chars = std::min<size_t>(f.status.size(), 63);
    for (size_t c = 0; c < chars; ++c) item[0x45 + 2 * c] = uint8_t(f.status[c]);  // UTF-16BE
    Store32(item + 0x40, uint32_t(chars ? chars + 1 : 0));
  }
  if (Trace()) {
    REXLOG_INFO("REACH_LIVE: friends list: {} of {} players", e->item_count(), roster.size());
  }
  Store32(base + handle_out, e->handle());
  return kSuccess;
}

// --- XGI sessions ----------------------------------------------------------------

struct HostedSession {
  uint8_t info[kSessionInfoSize];
  uint32_t flags;
};
std::mutex sessions_mutex;
std::unordered_map<uint32_t, HostedSession> hosted_sessions;  // by session object

void PublishSession(const HostedSession& s) {
  const bool joinable = (s.flags & kSessionUsesPresence) &&
                        !(s.flags & kSessionJoinViaPresenceDisabled);
  if (!joinable) {
    SetIdlePresence();
    return;
  }
  uint32_t state = kFriendOnline | kFriendPlaying | kFriendJoinable;
  if (s.flags & kSessionJoinViaPresenceFriendsOnly) state |= kFriendJoinableFriendsOnly;
  reach::LiveSetPresence(state, s.info, "Halo: Reach");
}

// XSessionCreate {session object, flags, public slots, private slots, user index,
// XSESSION_INFO*, nonce*}: for a session we host, fill the info before the SDK
// completes the request.
void XSessionCreate(uint8_t* base, uint32_t buffer) {
  const uint32_t object = Load32(base + buffer), flags = Load32(base + buffer + 4);
  const uint32_t info_ptr = Load32(base + buffer + 20), nonce_ptr = Load32(base + buffer + 24);
  if (!(flags & kSessionHost) || !info_ptr) return;
  HostedSession s{};
  s.flags = flags;
  Store64(s.info, RandomU64());
  s.info[0] = uint8_t((s.info[0] & 0x0F) | 0x80);  // XNET_XNKID_ONLINE_PEER
  reach::LiveSelfXnAddr(s.info + 8);
  Store64(s.info + 0x2C, RandomU64());
  Store64(s.info + 0x34, RandomU64());
  std::memcpy(base + info_ptr, s.info, kSessionInfoSize);
  if (nonce_ptr) Store64(base + nonce_ptr, RandomU64());
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    hosted_sessions[object] = s;
  }
  REXLOG_INFO("REACH_LIVE: hosting session {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} "
              "(flags {:08X}, {} public slots)",
              s.info[0], s.info[1], s.info[2], s.info[3], s.info[4], s.info[5], s.info[6],
              s.info[7], flags, Load32(base + buffer + 8));
  PublishSession(s);
}

// XSessionDelete {session object, ...}
void XSessionDelete(uint8_t* base, uint32_t buffer) {
  std::lock_guard<std::mutex> lock(sessions_mutex);
  if (hosted_sessions.erase(Load32(base + buffer))) SetIdlePresence();
}

// XSessionModify {session object, flags, public slots, private slots}: only the join
// flags (XSESSION_CREATE_MODIFIERS_MASK) change.
void XSessionModify(uint8_t* base, uint32_t buffer) {
  constexpr uint32_t kModifiers = 0xF00;
  std::lock_guard<std::mutex> lock(sessions_mutex);
  auto it = hosted_sessions.find(Load32(base + buffer));
  if (it == hosted_sessions.end()) return;
  it->second.flags = (it->second.flags & ~kModifiers) | (Load32(base + buffer + 4) & kModifiers);
  PublishSession(it->second);
}

// Our part of a message: kNotOurs lets the SDK answer (after any bookkeeping above).
uint32_t Handle(uint8_t* base, uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2) {
  if (!reach::LiveSignin()) return kNotOurs;
  static std::once_flag presence_once;
  std::call_once(presence_once, SetIdlePresence);
  if (app == kAppXLiveBase) {
    switch (message) {
      case 0x0005000C:
        return XStringVerify(base, arg1);
      case 0x00058020:
        return XFriendsCreateEnumerator(base, arg2);
    }
  } else if (app == kAppXgi && arg1) {
    switch (message) {
      case 0x000B0010:
        XSessionCreate(base, arg1);
        break;
      case 0x000B0011:
        XSessionDelete(base, arg1);
        break;
      case 0x000B0018:
        XSessionModify(base, arg1);
        break;
    }
  }
  return kNotOurs;
}

void LogMessage(const char* name, const uint32_t (&in)[6], uint32_t result, bool ours) {
  if (!Trace() || in[0] == 0xFA) return;  // the music player is noise
  REXLOG_INFO("NETTRACE {}(app {:02X}, msg {:08X}, {:08X}, {:08X}, {:08X}, {:08X}) -> {:08X}{}",
              name, in[0], in[1], in[2], in[3], in[4], in[5], result, ours ? " (REACH_LIVE)" : "");
}

GuestFunc Sdk(const char* name) { return reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, name)); }

}  // namespace

// DWORD XMsgInProcessCall(DWORD app, DWORD message, void* arg1, void* arg2)
extern "C" REX_FUNC(__imp__XMsgInProcessCall) {
  static GuestFunc sdk = Sdk("__imp__XMsgInProcessCall");
  const uint32_t in[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const uint32_t result = Handle(base, in[0], in[1], in[2], in[3]);
  if (result != kNotOurs) {
    ctx.r3.u64 = result;
  } else if (sdk) {
    sdk(ctx, base);
  }
  LogMessage("XMsgInProcessCall", in, ctx.r3.u32, result != kNotOurs);
}

namespace {
// XMsgStartIORequest[Ex](app, message, XOVERLAPPED*, buffer, buffer size[, unknown]):
// like the SDK, a request is answered at once and its XOVERLAPPED completed with the
// result.
void StartIORequest(const char* name, GuestFunc sdk, PPCContext& ctx, uint8_t* base) {
  const uint32_t in[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const uint32_t overlapped = in[2];
  const uint32_t result = Handle(base, in[0], in[1], in[3], in[4]);
  if (result == kNotOurs) {
    if (sdk) sdk(ctx, base);
    LogMessage(name, in, ctx.r3.u32, false);
    return;
  }
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, result);
    ctx.r3.u64 = kIoPending;
  } else {
    ctx.r3.u64 = result;
  }
  rex::system::XThread::SetLastError(0);
  LogMessage(name, in, ctx.r3.u32, true);
}
}  // namespace

extern "C" REX_FUNC(__imp__XMsgStartIORequest) {
  static GuestFunc sdk = Sdk("__imp__XMsgStartIORequest");
  StartIORequest("XMsgStartIORequest", sdk, ctx, base);
}

extern "C" REX_FUNC(__imp__XMsgStartIORequestEx) {
  static GuestFunc sdk = Sdk("__imp__XMsgStartIORequestEx");
  StartIORequest("XMsgStartIORequestEx", sdk, ctx, base);
}

namespace reach {
// Notifications for the game's friends code (XN_FRIENDS_PRESENCE_CHANGED,
// XN_FRIENDS_FRIEND_ADDED; the parameter is the user index).
void LiveRosterChanged(bool membership_changed) {
  if (!LiveSignin()) return;
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return;
  if (membership_changed) kernel->BroadcastNotification(0x04000002, 0);
  kernel->BroadcastNotification(0x04000001, 0);
  if (Trace()) REXLOG_INFO("REACH_LIVE: friends changed{}", membership_changed ? " (came/went)" : "");
}
}  // namespace reach

// DWORD XamUserAreUsersFriends(DWORD user_index, XUID* xuids, DWORD count,
//                              BOOL* are_friends, XOVERLAPPED* overlapped)
// Players in our room are friends.
extern "C" REX_FUNC(__imp__XamUserAreUsersFriends) {
  static GuestFunc sdk = Sdk("__imp__XamUserAreUsersFriends");
  if (!reach::LiveSignin()) {
    if (sdk) sdk(ctx, base);
    return;
  }
  const uint32_t xuids = ctx.r4.u32, count = ctx.r5.u32, result = ctx.r6.u32;
  const uint32_t overlapped = ctx.r7.u32;
  const auto roster = reach::LiveRoster();
  bool all = count > 0;
  for (uint32_t i = 0; i < count && xuids; ++i) {
    const uint64_t xuid = uint64_t(Load32(base + xuids + 8 * i)) << 32 | Load32(base + xuids + 8 * i + 4);
    bool found = false;
    for (const auto& f : roster) found = found || f.xuid == xuid;
    all = all && found;
  }
  if (result) Store32(base + result, all ? 1 : 0);
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, kSuccess);
    ctx.r3.u64 = kIoPending;
  } else {
    ctx.r3.u64 = kSuccess;
  }
}

// DWORD XNetLogonGetTitleID()
extern "C" REX_FUNC(__imp__XNetLogonGetTitleID) {
  ctx.r3.u64 = kTitleId;
}

// HRESULT XNetLogonGetMachineID(ULONGLONG* machine_id): Live machine ids are
// 0xFA000000xxxxxxxx; ours comes from the XUID.
extern "C" REX_FUNC(__imp__XNetLogonGetMachineID) {
  if (ctx.r3.u32) {
    Store64(base + ctx.r3.u32, 0xFA00000000000000ull | (reach::IdentityXuid() & 0xFFFFFFFFull));
  }
  ctx.r3.u64 = 0;
}
