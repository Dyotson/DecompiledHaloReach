// reach - Bungie's title servers ("LSP") on a Reach Live server.
//
// Reach finds its title servers with XTitleServerCreateEnumerator (XAM enumerator
// app 0xFC, open message 0x58039): items are X_TITLE_SERVER {inaServer, dwFlags,
// szServerInfo[200]}. The game's LSP manager splits each szServerInfo at ',' and
// matches the tokens against its service names (eight 4-byte strings at 0x8325114C,
// filled from the network configuration; Function_82271658), so a server offers the
// services its description names. The game then resolves the server with
// XNetServerToInAddr(ina, 0x4D530064) and opens an HTTP/1.0 connection on a port of
// the configured LSP range (Lsp_ResolveServerAddress 0x82271E38).
//
// With Live sign-in (REACH_SERVER + REACH_LIVE_SIGNIN=1) the enumeration returns one
// title server: the Reach Live server, offering every service. net.cpp resolves its
// address to itself and sends TCP connections to it to the server's HTTP port, where
// server/reach_live_lsp.py answers. The marketplace asset enumeration (0x58042) is
// empty. Every other enumerator goes to the SDK.

#include "identity.h"
#include "live.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>

#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kXLiveBaseApp = 0xFC;
constexpr uint32_t kTitleServerOpen = 0x58039;     // XTitleServerCreateEnumerator
constexpr uint32_t kMarketplaceAssetOpen = 0x58042;  // XMarketplaceCreateAssetEnumerator
constexpr uint32_t kTitleServerSize = 0xD0;        // X_TITLE_SERVER
constexpr uint32_t kServiceNames = 0x8325114C;     // char[8][4], see the top comment
constexpr uint32_t kServiceNameCount = 8;

void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Every service name the game knows, comma separated: the server offers them all.
std::string AllServices(const uint8_t* base) {
  std::string out;
  for (uint32_t i = 0; i < kServiceNameCount; ++i) {
    const char* name = reinterpret_cast<const char*>(base + kServiceNames + 4 * i);
    size_t size = strnlen(name, 4);
    if (!size || size == 4) continue;
    if (!out.empty()) out += ',';
    out.append(name, size);
  }
  return out;
}

}  // namespace

// DWORD XamCreateEnumeratorHandle(DWORD user_index, DWORD app_id, DWORD open_message,
//     DWORD close_message, DWORD extra_size, DWORD item_count, DWORD flags, HANDLE* handle)
extern "C" REX_FUNC(__imp__XamCreateEnumeratorHandle) {
  const uint32_t user_index = ctx.r3.u32, app_id = ctx.r4.u32, open = ctx.r5.u32,
                 close = ctx.r6.u32, extra_size = ctx.r7.u32, item_count = ctx.r8.u32,
                 flags = ctx.r9.u32, out_handle = ctx.r10.u32;
  const bool title_servers = open == kTitleServerOpen;
  if (app_id != kXLiveBaseApp || (!title_servers && open != kMarketplaceAssetOpen) ||
      !reach::LiveSignin()) {
    static GuestFunc sdk =
        reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, "__imp__XamCreateEnumeratorHandle"));
    if (sdk) sdk(ctx, base);
    return;
  }
  using namespace rex;
  using namespace rex::system;
  const uint32_t item_size = title_servers ? kTitleServerSize : extra_size;
  auto e = object_ref<XStaticUntypedEnumerator>(
      new XStaticUntypedEnumerator(REX_KERNEL_STATE(), item_count ? item_count : 1, item_size));
  X_STATUS status = e->Initialize(user_index, app_id, open, close, flags, extra_size, nullptr);
  if (XFAILED(status)) {
    ctx.r3.u64 = status;
    return;
  }
  uint32_t ip;
  uint16_t port;
  if (title_servers && reach::LiveServerHttp(ip, port)) {
    uint8_t* item = e->AppendItem();
    std::memset(item, 0, kTitleServerSize);
    Store32(item, ip);
    const std::string services = AllServices(base);
    std::strncpy(reinterpret_cast<char*>(item + 8), services.c_str(), 199);
    REXLOG_INFO("REACH_LIVE: title server {}.{}.{}.{} (HTTP port {}) for services \"{}\"",
                ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port, services);
  }
  if (out_handle) Store32(base + out_handle, e->handle());
  ctx.r3.u64 = 0;
}
