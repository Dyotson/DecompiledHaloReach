// reach - optional trace of the networking imports (P2P milestone M0).
//
// REACH_NETTRACE=1 logs every call to the Winsock (NetDll_*), XNet, XAM session
// and voice imports, and to the XMsg calls that carry XSession / XUser
// messages, with the raw argument registers r3-r8 and the result. Busy calls
// (select, recvfrom, sendto, ...) are logged for their first 20 calls and then
// every 1000th. Without the variable every call goes to the SDK unchanged.
//
// REACH_NET_LINK=1 (experimental) reports an active 100 Mbit full-duplex
// Ethernet link from XNetGetEthernetLinkStatus instead of the SDK's "no cable",
// which is what keeps System Link out of the game's network menu.
// docs/online_plan.md has the context.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <dlfcn.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

bool NetTrace() {
  static const bool enabled = [] {
    const char* v = std::getenv("REACH_NETTRACE");
    return v && *v && *v != '0';
  }();
  return enabled;
}

bool ShouldLog(std::atomic<uint64_t>& calls) {
  uint64_t n = calls.fetch_add(1, std::memory_order_relaxed);
  return n < 20 || n % 1000 == 0;
}

void Traced(const char* name, GuestFunc sdk, std::atomic<uint64_t>& calls, PPCContext& ctx,
            uint8_t* base) {
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32,
                 r7 = ctx.r7.u32, r8 = ctx.r8.u32;
  if (sdk) sdk(ctx, base);
  if (ShouldLog(calls)) {
    REXLOG_INFO("NETTRACE {}({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}) -> {:08X} [call {}]",
                name, r3, r4, r5, r6, r7, r8, ctx.r3.u32, calls.load() - 1);
  }
}

}  // namespace

#define REACH_NET_TRACE(name)                                                         \
  extern "C" REX_FUNC(__imp__##name) {                                                \
    static GuestFunc sdk = reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, "__imp__" #name)); \
    static std::atomic<uint64_t> calls{0};                                            \
    if (!NetTrace()) {                                                                \
      if (sdk) sdk(ctx, base);                                                        \
      return;                                                                         \
    }                                                                                 \
    Traced(#name, sdk, calls, ctx, base);                                             \
  }

REACH_NET_TRACE(NetDll_WSAStartup)
REACH_NET_TRACE(NetDll_WSACleanup)
REACH_NET_TRACE(NetDll_WSAGetLastError)
REACH_NET_TRACE(NetDll___WSAFDIsSet)
REACH_NET_TRACE(NetDll_socket)
REACH_NET_TRACE(NetDll_bind)
REACH_NET_TRACE(NetDll_connect)
REACH_NET_TRACE(NetDll_listen)
REACH_NET_TRACE(NetDll_accept)
REACH_NET_TRACE(NetDll_select)
REACH_NET_TRACE(NetDll_setsockopt)
REACH_NET_TRACE(NetDll_ioctlsocket)
REACH_NET_TRACE(NetDll_send)
REACH_NET_TRACE(NetDll_recv)
REACH_NET_TRACE(NetDll_sendto)
REACH_NET_TRACE(NetDll_recvfrom)
REACH_NET_TRACE(NetDll_shutdown)
REACH_NET_TRACE(NetDll_closesocket)
REACH_NET_TRACE(NetDll_inet_addr)
REACH_NET_TRACE(NetDll_XNetStartup)
REACH_NET_TRACE(NetDll_XNetCleanup)
REACH_NET_TRACE(NetDll_XNetRandom)

// DWORD XNetGetEthernetLinkStatus()
extern "C" REX_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) {
  static GuestFunc sdk =
      reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, "__imp__NetDll_XNetGetEthernetLinkStatus"));
  static std::atomic<uint64_t> calls{0};
  static const bool link = [] {
    const char* v = std::getenv("REACH_NET_LINK");
    return v && *v && *v != '0';
  }();
  if (!link) {
    if (NetTrace()) {
      Traced("NetDll_XNetGetEthernetLinkStatus", sdk, calls, ctx, base);
    } else if (sdk) {
      sdk(ctx, base);
    }
    return;
  }
  // XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | XNET_ETHERNET_LINK_FULL_DUPLEX
  ctx.r3.u64 = 0x0B;
  if (NetTrace() && ShouldLog(calls)) {
    REXLOG_INFO("NETTRACE NetDll_XNetGetEthernetLinkStatus() -> 0000000B (REACH_NET_LINK)");
  }
}
REACH_NET_TRACE(NetDll_XNetGetTitleXnAddr)
REACH_NET_TRACE(NetDll_XNetCreateKey)
REACH_NET_TRACE(NetDll_XNetRegisterKey)
REACH_NET_TRACE(NetDll_XNetUnregisterKey)
REACH_NET_TRACE(NetDll_XNetXnAddrToInAddr)
REACH_NET_TRACE(NetDll_XNetInAddrToXnAddr)
REACH_NET_TRACE(NetDll_XNetUnregisterInAddr)
REACH_NET_TRACE(NetDll_XNetXnAddrToMachineId)
REACH_NET_TRACE(NetDll_XNetConnect)
REACH_NET_TRACE(NetDll_XNetGetConnectStatus)
REACH_NET_TRACE(NetDll_XNetServerToInAddr)
REACH_NET_TRACE(NetDll_XNetQosListen)
REACH_NET_TRACE(NetDll_XNetQosLookup)
REACH_NET_TRACE(NetDll_XNetQosServiceLookup)
REACH_NET_TRACE(NetDll_XNetQosRelease)
REACH_NET_TRACE(NetDll_XNetQosGetListenStats)
REACH_NET_TRACE(XNetLogonGetMachineID)
REACH_NET_TRACE(XNetLogonGetTitleID)
REACH_NET_TRACE(XamSessionCreateHandle)
REACH_NET_TRACE(XamSessionRefObjByHandle)
REACH_NET_TRACE(XamVoiceCreate)
REACH_NET_TRACE(XamVoiceClose)
REACH_NET_TRACE(XamVoiceSubmitPacket)
REACH_NET_TRACE(XamVoiceHeadsetPresent)
REACH_NET_TRACE(XMsgStartIORequest)
REACH_NET_TRACE(XMsgStartIORequestEx)
REACH_NET_TRACE(XMsgInProcessCall)
REACH_NET_TRACE(XMsgCancelIORequest)
REACH_NET_TRACE(XMsgCompleteIORequest)
