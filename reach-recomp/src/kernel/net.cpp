// reach - networking: a virtual System Link network and a call trace.
//
// REACH_NET=1 turns on the virtual network (docs/online_plan.md, M1/M2):
//
// - XNetGetEthernetLinkStatus reports a link, so the game offers System Link.
// - The game's UDP sockets (VDP port 1000 and broadcast port 1001) are ours,
//   not the SDK's host sockets: an instance owns one host UDP port, the first
//   free one in 21000-21007, and every datagram carries a header with the guest
//   source and destination ports. Privileged ports and port clashes between
//   instances on one machine never reach the host.
// - A broadcast goes to every port of that range on 127.0.0.1, and with
//   REACH_NET_LAN=1 also to 255.255.255.255 (other machines).
// - XNetGetTitleXnAddr describes this instance by its host IP (REACH_NET_IP,
//   else 127.0.0.1, or the LAN address with REACH_NET_LAN=1) and host port.
//   XNetXnAddrToInAddr maps a peer's XNADDR to a virtual IP 10.77.0.N that
//   sendto/recvfrom translate back to the peer's host endpoint (this instance
//   is 10.77.0.1).
// - XNetRandom returns random bytes (the SDK's are constant). Keys are random
//   system-link keys; registering, connecting and QoS listen
//   succeed. VDP is sent unencrypted: only copies of this port talk to it.
//
// Other socket types (TCP) still go to the SDK.
//
// REACH_NETTRACE=1 logs every networking import with its raw argument
// registers r3-r8 and the result; busy calls are logged for their first 20
// calls and then every 1000th.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xthread.h>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

bool EnvFlag(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && *v != '0';
}

// REACH_NETTRACE=1 traces calls and datagrams, REACH_NETTRACE=packets only datagrams.
bool NetTrace() {
  static const bool enabled = EnvFlag("REACH_NETTRACE") &&
                              std::string(std::getenv("REACH_NETTRACE")) != "packets";
  return enabled;
}

bool PacketTrace() {
  static const bool enabled = EnvFlag("REACH_NETTRACE");
  return enabled;
}

bool NetOn() {
  static const bool enabled = EnvFlag("REACH_NET");
  return enabled;
}

bool ShouldLog(std::atomic<uint64_t>& calls) {
  uint64_t n = calls.fetch_add(1, std::memory_order_relaxed);
  return n < 20 || n % 1000 == 0;
}

void Log(const char* name, std::atomic<uint64_t>& calls, const uint32_t (&in)[6], uint32_t result,
         const char* note, uint32_t lr = 0) {
  if (NetTrace() && ShouldLog(calls)) {
    REXLOG_INFO(
        "NETTRACE {}({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}) -> {:08X}{} [call {}] lr={:08X}",
        name, in[0], in[1], in[2], in[3], in[4], in[5], result, note, calls.load() - 1, lr);
  }
}

// Guest memory is big-endian.
uint16_t Load16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Store16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Guest return addresses of the calling frames: each frame's back chain is at
// r1, and the caller's LR is saved 8 bytes below the parent frame.
std::string GuestBacktrace(const PPCContext& ctx, const uint8_t* base, int depth) {
  std::string out;
  uint32_t frame = ctx.r1.u32;
  for (int i = 0; i < depth && frame; ++i) {
    uint32_t parent = Load32(base + frame);
    if (parent <= frame || parent - frame > 0x10000 || (parent & 7)) break;
    uint32_t lr = Load32(base + parent - 8);
    if (lr < 0x82000000 || lr >= 0x84000000) break;
    char buf[16];
    std::snprintf(buf, sizeof(buf), " %08X", lr);
    out += buf;
    // A 64 KB-aligned frame is the top of the thread's stack: nothing above it is mapped.
    if (!(parent & 0xFFFF)) break;
    frame = parent;
  }
  return out;
}

void Traced(const char* name, GuestFunc sdk, std::atomic<uint64_t>& calls, PPCContext& ctx,
            uint8_t* base) {
  const uint32_t in[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const uint32_t lr = uint32_t(ctx.lr);
  if (sdk) sdk(ctx, base);
  Log(name, calls, in, ctx.r3.u32, "", lr);
}

// Winsock errors as the guest sees them.
constexpr uint32_t kWsaEWouldBlock = 10035;
constexpr uint32_t kWsaENotSock = 10038;
constexpr uint32_t kWsaEMsgSize = 10040;
constexpr uint32_t kWsaEAddrInUse = 10048;
constexpr uint32_t kWsaENetUnreach = 10051;
constexpr uint32_t kWsaEInval = 10022;

// REACH_NETTRACE also logs datagrams (the first 400, then every 500th).
void LogPacket(const char* dir, uint32_t vip, uint16_t source_port, uint16_t port,
               const uint8_t* data, size_t size) {
  static std::atomic<uint64_t> count{0};
  uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  if (!PacketTrace() || (n >= 2000 && n % 500 != 0)) return;
  char hex[3 * 256 + 1] = {};
  for (size_t i = 0; i < std::min<size_t>(size, 256); ++i) {
    std::snprintf(hex + 3 * i, 4, "%02X ", data[i]);
  }
  REXLOG_INFO("REACH_NET {} {} bytes {}:{} -> port {} vip {:08X} [{}] #{}", dir, size, vip >> 24 == 10 ? "vip" : "ip",
              source_port, port, vip, hex, n);
}

constexpr uint32_t kHandleBase = 0x52450000;  // our guest SOCKET handles: 'RE' + index
constexpr uint32_t kMagic = 0x524E4554;       // 'RNET', first word of every datagram
constexpr size_t kHeaderSize = 8;             // magic, guest source port, guest destination port
constexpr uint16_t kHostPortFirst = 21000;
constexpr uint16_t kHostPortCount = 8;
constexpr uint32_t kSelfVip = 0x0A4D0001;  // 10.77.0.1
constexpr uint32_t kVipNet = 0x0A4D0000;   // 10.77.0.0/16
constexpr uint32_t kBroadcast = 0xFFFFFFFF;

struct Datagram {
  uint32_t vip;
  uint16_t port;
  std::vector<uint8_t> data;
};

struct GuestSocket {
  uint16_t port = 0;  // bound guest port, 0 while unbound
  bool nonblocking = false;
  uint32_t connected_vip = 0;
  uint16_t connected_port = 0;
  std::deque<Datagram> queue;
};

struct Endpoint {
  uint32_t ip;  // host order
  uint16_t port;
  bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
};

class VNet {
 public:
  static VNet& Get() {
    static VNet net;
    return net;
  }

  bool Ready() const { return fd_ >= 0; }
  uint32_t SelfIp() const { return self_ip_; }
  uint16_t HostPort() const { return host_port_; }

  uint32_t CreateSocket() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t handle = kHandleBase + next_handle_++;
    sockets_[handle] = GuestSocket();
    return handle;
  }

  bool IsOurs(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sockets_.count(handle) != 0;
  }

  void Close(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    sockets_.erase(handle);
    cv_.notify_all();
  }

  uint32_t Bind(uint32_t handle, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sockets_.find(handle);
    if (it == sockets_.end()) return kWsaENotSock;
    if (port == 0) port = next_ephemeral_++;
    for (auto& [h, s] : sockets_) {
      if (h != handle && s.port == port) return kWsaEAddrInUse;
    }
    it->second.port = port;
    return 0;
  }

  uint32_t SetNonblocking(uint32_t handle, bool nonblocking) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sockets_.find(handle);
    if (it == sockets_.end()) return kWsaENotSock;
    it->second.nonblocking = nonblocking;
    return 0;
  }

  uint32_t Connect(uint32_t handle, uint32_t vip, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sockets_.find(handle);
    if (it == sockets_.end()) return kWsaENotSock;
    it->second.connected_vip = vip;
    it->second.connected_port = port;
    return 0;
  }

  bool Connected(uint32_t handle, uint32_t& vip, uint16_t& port) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sockets_.find(handle);
    if (it == sockets_.end() || !it->second.connected_vip) return false;
    vip = it->second.connected_vip;
    port = it->second.connected_port;
    return true;
  }

  uint32_t PendingBytes(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sockets_.find(handle);
    if (it == sockets_.end() || it->second.queue.empty()) return 0;
    return uint32_t(it->second.queue.front().data.size());
  }

  // Sends one guest datagram; returns 0 or a Winsock error.
  uint32_t SendTo(uint32_t handle, uint32_t vip, uint16_t port, const uint8_t* data, size_t size) {
    uint16_t source_port;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = sockets_.find(handle);
      if (it == sockets_.end()) return kWsaENotSock;
      if (!it->second.port) it->second.port = next_ephemeral_++;
      source_port = it->second.port;
    }
    std::vector<uint8_t> packet(kHeaderSize + size);
    Store32(packet.data(), kMagic);
    Store16(packet.data() + 4, source_port);
    Store16(packet.data() + 6, port);
    std::memcpy(packet.data() + kHeaderSize, data, size);

    LogPacket("send", vip, source_port, port, data, size);
    if (vip == kSelfVip) {
      Deliver(kSelfVip, source_port, port, data, size);
      return 0;
    }
    if (vip == kBroadcast || (vip & 0xFF) == 0xFF) {
      for (uint16_t p = kHostPortFirst; p < kHostPortFirst + kHostPortCount; ++p) {
        if (p != host_port_) SendHost({INADDR_LOOPBACK, p}, packet);
        if (lan_) SendHost({INADDR_BROADCAST, p}, packet);
      }
      return 0;
    }
    Endpoint endpoint;
    if (!EndpointOf(vip, endpoint)) return kWsaENetUnreach;
    SendHost(endpoint, packet);
    return 0;
  }

  // Receives one datagram. Returns 0, or a Winsock error (kWsaEWouldBlock when
  // a nonblocking socket has nothing queued). `size` is the payload size.
  uint32_t ReceiveFrom(uint32_t handle, Datagram& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      auto it = sockets_.find(handle);
      if (it == sockets_.end()) return kWsaENotSock;
      if (!it->second.queue.empty()) {
        out = std::move(it->second.queue.front());
        it->second.queue.pop_front();
        return 0;
      }
      if (it->second.nonblocking) return kWsaEWouldBlock;
      cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
  }

  // Waits until one of `handles` has a datagram queued or the timeout passes
  // (negative: forever). Returns the handles that are readable.
  std::vector<uint32_t> WaitReadable(const std::vector<uint32_t>& handles, int64_t timeout_us) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
      std::vector<uint32_t> ready;
      for (uint32_t h : handles) {
        auto it = sockets_.find(h);
        if (it != sockets_.end() && !it->second.queue.empty()) ready.push_back(h);
      }
      if (!ready.empty() || timeout_us == 0) return ready;
      if (timeout_us < 0) {
        cv_.wait_for(lock, std::chrono::milliseconds(100));
      } else if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
        timeout_us = 0;
      }
    }
  }

  // Virtual IP of a host endpoint (an XNADDR's ina and wPortOnline).
  uint32_t VipOf(Endpoint endpoint) {
    if (endpoint.ip == self_ip_ && endpoint.port == host_port_) return kSelfVip;
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t key = uint64_t(endpoint.ip) << 16 | endpoint.port;
    auto it = vip_by_endpoint_.find(key);
    if (it != vip_by_endpoint_.end()) return it->second;
    uint32_t vip = kVipNet + next_vip_++;
    vip_by_endpoint_[key] = vip;
    endpoint_by_vip_[vip] = endpoint;
    REXLOG_INFO("REACH_NET: peer {}.{}.{}.{}:{} is 10.77.{}.{}", endpoint.ip >> 24,
                (endpoint.ip >> 16) & 0xFF, (endpoint.ip >> 8) & 0xFF, endpoint.ip & 0xFF,
                endpoint.port, (vip >> 8) & 0xFF, vip & 0xFF);
    return vip;
  }

  bool EndpointOf(uint32_t vip, Endpoint& out) {
    if (vip == kSelfVip) {
      out = {self_ip_, host_port_};
      return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = endpoint_by_vip_.find(vip);
    if (it == endpoint_by_vip_.end()) return false;
    out = it->second;
    return true;
  }

 private:
  VNet() {
    lan_ = EnvFlag("REACH_NET_LAN");
    self_ip_ = INADDR_LOOPBACK;
    if (const char* ip = std::getenv("REACH_NET_IP"); ip && *ip) {
      in_addr a{};
      if (inet_pton(AF_INET, ip, &a) == 1) self_ip_ = ntohl(a.s_addr);
    } else if (lan_) {
      self_ip_ = LanAddress();
    }
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    for (uint16_t p = kHostPortFirst; p < kHostPortFirst + kHostPortCount; ++p) {
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(p);
      a.sin_addr.s_addr = htonl(lan_ ? INADDR_ANY : INADDR_LOOPBACK);
      if (bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        host_port_ = p;
        break;
      }
    }
    if (!host_port_) {
      REXLOG_ERROR("REACH_NET: no free host UDP port in {}-{}", kHostPortFirst,
                   kHostPortFirst + kHostPortCount - 1);
      close(fd_);
      fd_ = -1;
      return;
    }
    REXLOG_INFO("REACH_NET: virtual network on {}.{}.{}.{} UDP {}{}", self_ip_ >> 24,
                (self_ip_ >> 16) & 0xFF, (self_ip_ >> 8) & 0xFF, self_ip_ & 0xFF, host_port_,
                lan_ ? " (LAN broadcast on)" : "");
    std::thread([this] { ReceiveLoop(); }).detach();
  }

  static uint32_t LanAddress() {
    // The source address the kernel picks for an outgoing route; no packet is sent.
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);
    uint32_t ip = INADDR_LOOPBACK;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
      sockaddr_in local{};
      socklen_t len = sizeof(local);
      if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
        ip = ntohl(local.sin_addr.s_addr);
      }
    }
    close(s);
    return ip;
  }

  void SendHost(Endpoint endpoint, const std::vector<uint8_t>& packet) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(endpoint.port);
    a.sin_addr.s_addr = htonl(endpoint.ip);
    sendto(fd_, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  }

  void Deliver(uint32_t vip, uint16_t source_port, uint16_t port, const uint8_t* data,
               size_t size) {
    LogPacket("recv", vip, source_port, port, data, size);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [handle, s] : sockets_) {
      if (s.port != port) continue;
      if (s.queue.size() >= 256) s.queue.pop_front();
      s.queue.push_back({vip, source_port, std::vector<uint8_t>(data, data + size)});
      cv_.notify_all();
      return;
    }
  }

  void ReceiveLoop() {
    std::vector<uint8_t> buffer(65536);
    for (;;) {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      ssize_t n = recvfrom(fd_, buffer.data(), buffer.size(), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n < ssize_t(kHeaderSize) || Load32(buffer.data()) != kMagic) continue;
      Endpoint endpoint{ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
      if (endpoint.port == host_port_ && (endpoint.ip == self_ip_ || endpoint.ip == INADDR_LOOPBACK)) {
        continue;  // our own broadcast
      }
      uint32_t vip = VipOf(endpoint);
      Deliver(vip, Load16(buffer.data() + 4), Load16(buffer.data() + 6),
              buffer.data() + kHeaderSize, size_t(n) - kHeaderSize);
    }
  }

  int fd_ = -1;
  bool lan_ = false;
  uint32_t self_ip_ = 0;
  uint16_t host_port_ = 0;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::unordered_map<uint32_t, GuestSocket> sockets_;
  uint32_t next_handle_ = 1;
  uint16_t next_ephemeral_ = 49152;
  std::unordered_map<uint64_t, uint32_t> vip_by_endpoint_;
  std::unordered_map<uint32_t, Endpoint> endpoint_by_vip_;
  uint32_t next_vip_ = 2;
};

bool Ours(uint32_t handle) {
  return NetOn() && (handle & 0xFFFF0000) == kHandleBase && VNet::Get().IsOurs(handle);
}

void SetError(uint32_t error) { rex::system::XThread::SetLastError(error); }

// Guest XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20].
void WriteXnAddr(uint8_t* p, uint32_t ip, uint16_t port) {
  std::memset(p, 0, 36);
  Store32(p, ip);
  Store16(p + 8, port);
  const uint8_t mac[6] = {0x00, 0x22, 0x48, uint8_t(ip), uint8_t(port >> 8), uint8_t(port)};
  std::memcpy(p + 10, mac, 6);
}

}  // namespace

#define REACH_NET_SDK(name) \
  static GuestFunc sdk = reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, "__imp__" #name))

#define REACH_NET_TRACE(name)                                          \
  extern "C" REX_FUNC(__imp__##name) {                                 \
    REACH_NET_SDK(name);                                               \
    static std::atomic<uint64_t> calls{0};                             \
    if (!NetTrace()) {                                                 \
      if (sdk) sdk(ctx, base);                                         \
      return;                                                          \
    }                                                                  \
    Traced(#name, sdk, calls, ctx, base);                              \
  }

// Defines an import that the virtual network implements when `cond` holds:
// `body` sets ctx.r3 (and the guest last error); otherwise the SDK runs.
#define REACH_NET_FUNC(name, cond, body)                               \
  extern "C" REX_FUNC(__imp__##name) {                                 \
    REACH_NET_SDK(name);                                               \
    static std::atomic<uint64_t> calls{0};                             \
    const uint32_t in[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,        \
                            ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};       \
    const uint32_t lr = uint32_t(ctx.lr);                              \
    if (cond) {                                                        \
      body;                                                            \
      Log(#name, calls, in, ctx.r3.u32, " (REACH_NET)", lr);           \
      return;                                                          \
    }                                                                  \
    if (sdk) sdk(ctx, base);                                           \
    Log(#name, calls, in, ctx.r3.u32, "", lr);                         \
  }

// --- XNet ------------------------------------------------------------------

// DWORD XNetGetEthernetLinkStatus()
REACH_NET_FUNC(NetDll_XNetGetEthernetLinkStatus, NetOn(), {
  // XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | XNET_ETHERNET_LINK_FULL_DUPLEX
  ctx.r3.u64 = 0x0B;
})

// DWORD XNetGetTitleXnAddr(XNADDR* pxna)
REACH_NET_FUNC(NetDll_XNetGetTitleXnAddr, NetOn() && VNet::Get().Ready(), {
  if (ctx.r4.u32) WriteXnAddr(base + ctx.r4.u32, VNet::Get().SelfIp(), VNet::Get().HostPort());
  ctx.r3.u64 = 0x06;  // XNET_GET_XNADDR_STATIC | XNET_GET_XNADDR_ETHERNET
})

// INT XNetXnAddrToInAddr(const XNADDR* pxna, const XNKID* pxnkid, IN_ADDR* pina)
REACH_NET_FUNC(NetDll_XNetXnAddrToInAddr, NetOn() && VNet::Get().Ready(), {
  const uint8_t* xna = base + ctx.r4.u32;
  uint32_t vip = VNet::Get().VipOf({Load32(xna), Load16(xna + 8)});
  Store32(base + ctx.r6.u32, vip);
  ctx.r3.u64 = 0;
})

// INT XNetInAddrToXnAddr(IN_ADDR ina, XNADDR* pxna, XNKID* pxnkid)
REACH_NET_FUNC(NetDll_XNetInAddrToXnAddr, NetOn() && VNet::Get().Ready(), {
  Endpoint endpoint;
  if (VNet::Get().EndpointOf(ctx.r4.u32, endpoint)) {
    if (ctx.r5.u32) WriteXnAddr(base + ctx.r5.u32, endpoint.ip, endpoint.port);
    ctx.r3.u64 = 0;
  } else {
    ctx.r3.u64 = kWsaEInval;
  }
})

// INT XNetCreateKey(XNKID* pxnkid, XNKEY* pxnkey)
REACH_NET_FUNC(NetDll_XNetCreateKey, NetOn(), {
  static std::mt19937_64 rng(std::random_device{}());
  uint8_t* kid = base + ctx.r4.u32;
  for (int i = 0; i < 8; ++i) kid[i] = uint8_t(rng());
  kid[0] &= 0x0F;  // XNET_XNKID_SYSTEM_LINK
  if (ctx.r5.u32) {
    for (int i = 0; i < 16; ++i) base[ctx.r5.u32 + i] = uint8_t(rng());
  }
  ctx.r3.u64 = 0;
})

REACH_NET_FUNC(NetDll_XNetRegisterKey, NetOn(), ctx.r3.u64 = 0)
REACH_NET_FUNC(NetDll_XNetUnregisterKey, NetOn(), ctx.r3.u64 = 0)
REACH_NET_FUNC(NetDll_XNetUnregisterInAddr, NetOn(), ctx.r3.u64 = 0)
REACH_NET_FUNC(NetDll_XNetConnect, NetOn(), ctx.r3.u64 = 0)
// XNET_CONNECT_STATUS_CONNECTED
REACH_NET_FUNC(NetDll_XNetGetConnectStatus, NetOn(), ctx.r3.u64 = 2)
REACH_NET_FUNC(NetDll_XNetQosListen, NetOn(), ctx.r3.u64 = 0)

// INT XNetXnAddrToMachineId(const XNADDR* pxnaddr, ULONGLONG* pqwMachineId)
REACH_NET_FUNC(NetDll_XNetXnAddrToMachineId, NetOn(), {
  const uint8_t* xna = base + ctx.r4.u32;
  uint8_t* id = base + ctx.r5.u32;
  Store32(id, 0xFA000000 | Load16(xna + 8));
  Store32(id + 4, Load32(xna));
  ctx.r3.u64 = 0;
})

REACH_NET_TRACE(NetDll_XNetStartup)
REACH_NET_TRACE(NetDll_XNetCleanup)
// INT XNetRandom(BYTE* pb, UINT cb): the SDK fills 0xBB, which gives every
// instance the same session nonces, so each takes the others' broadcast
// searches for its own.
REACH_NET_FUNC(NetDll_XNetRandom, NetOn(), {
  static std::mutex rng_mutex;
  static std::mt19937_64 rng(std::random_device{}());
  std::lock_guard<std::mutex> lock(rng_mutex);
  for (uint32_t i = 0; i < ctx.r5.u32; ++i) base[ctx.r4.u32 + i] = uint8_t(rng() >> 24);
  if (NetTrace()) REXLOG_INFO("NETTRACE XNetRandom({}) from{}", ctx.r5.u32, GuestBacktrace(ctx, base, 12));
  ctx.r3.u64 = 0;
})
REACH_NET_TRACE(NetDll_XNetServerToInAddr)
REACH_NET_TRACE(NetDll_XNetQosLookup)
REACH_NET_TRACE(NetDll_XNetQosServiceLookup)
REACH_NET_TRACE(NetDll_XNetQosRelease)
REACH_NET_TRACE(NetDll_XNetQosGetListenStats)
REACH_NET_TRACE(XNetLogonGetMachineID)
REACH_NET_TRACE(XNetLogonGetTitleID)

// --- Sockets ---------------------------------------------------------------

// SOCKET socket(int af, int type, int protocol): UDP and VDP datagram sockets are ours.
REACH_NET_FUNC(NetDll_socket, NetOn() && VNet::Get().Ready() && ctx.r5.u32 == 2, {
  ctx.r3.u64 = VNet::Get().CreateSocket();
})

// int bind(SOCKET s, const sockaddr* name, int namelen)
REACH_NET_FUNC(NetDll_bind, Ours(ctx.r4.u32), {
  uint32_t error = VNet::Get().Bind(ctx.r4.u32, Load16(base + ctx.r5.u32 + 2));
  if (error) SetError(error);
  ctx.r3.u64 = error ? uint64_t(-1) : 0;
})

// int connect(SOCKET s, const sockaddr* name, int namelen)
REACH_NET_FUNC(NetDll_connect, Ours(ctx.r4.u32), {
  const uint8_t* a = base + ctx.r5.u32;
  VNet::Get().Connect(ctx.r4.u32, Load32(a + 4), Load16(a + 2));
  ctx.r3.u64 = 0;
})

// int closesocket(SOCKET s)
REACH_NET_FUNC(NetDll_closesocket, Ours(ctx.r4.u32), {
  VNet::Get().Close(ctx.r4.u32);
  ctx.r3.u64 = 0;
})

// int setsockopt(SOCKET s, int level, int optname, const char* optval, int optlen)
REACH_NET_FUNC(NetDll_setsockopt, Ours(ctx.r4.u32), ctx.r3.u64 = 0)
REACH_NET_FUNC(NetDll_shutdown, Ours(ctx.r4.u32), ctx.r3.u64 = 0)

// int ioctlsocket(SOCKET s, long cmd, u_long* argp)
REACH_NET_FUNC(NetDll_ioctlsocket, Ours(ctx.r4.u32), {
  const uint32_t cmd = ctx.r5.u32;
  uint8_t* arg = base + ctx.r6.u32;
  if (cmd == 0x8004667E) {  // FIONBIO
    VNet::Get().SetNonblocking(ctx.r4.u32, Load32(arg) != 0);
    ctx.r3.u64 = 0;
  } else if (cmd == 0x4004667F) {  // FIONREAD
    Store32(arg, VNet::Get().PendingBytes(ctx.r4.u32));
    ctx.r3.u64 = 0;
  } else {
    SetError(kWsaEInval);
    ctx.r3.u64 = uint64_t(-1);
  }
})

// int sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen)
REACH_NET_FUNC(NetDll_sendto, Ours(ctx.r4.u32), {
  uint32_t vip = 0;
  uint16_t port = 0;
  if (ctx.r8.u32) {
    vip = Load32(base + ctx.r8.u32 + 4);
    port = Load16(base + ctx.r8.u32 + 2);
  } else {
    VNet::Get().Connected(ctx.r4.u32, vip, port);
  }
  uint32_t error = VNet::Get().SendTo(ctx.r4.u32, vip, port, base + ctx.r5.u32, ctx.r6.u32);
  if (error) SetError(error);
  ctx.r3.u64 = error ? uint64_t(-1) : ctx.r6.u32;
})

// int send(SOCKET s, const char* buf, int len, int flags)
REACH_NET_FUNC(NetDll_send, Ours(ctx.r4.u32), {
  uint32_t vip = 0;
  uint16_t port = 0;
  VNet::Get().Connected(ctx.r4.u32, vip, port);
  uint32_t error = VNet::Get().SendTo(ctx.r4.u32, vip, port, base + ctx.r5.u32, ctx.r6.u32);
  if (error) SetError(error);
  ctx.r3.u64 = error ? uint64_t(-1) : ctx.r6.u32;
})

namespace {
// recvfrom/recv: copies one datagram into the guest buffer.
uint64_t Receive(PPCContext& ctx, uint8_t* base, uint32_t from, uint32_t from_len) {
  Datagram d;
  uint32_t error = VNet::Get().ReceiveFrom(ctx.r4.u32, d);
  if (error) {
    SetError(error);
    return uint64_t(-1);
  }
  size_t n = std::min<size_t>(d.data.size(), ctx.r6.u32);
  std::memcpy(base + ctx.r5.u32, d.data.data(), n);
  if (from) {
    uint8_t* a = base + from;
    std::memset(a, 0, 16);
    Store16(a, 2);  // AF_INET
    Store16(a + 2, d.port);
    Store32(a + 4, d.vip);
    if (from_len) Store32(base + from_len, 16);
  }
  if (n < d.data.size()) {
    SetError(kWsaEMsgSize);
    return uint64_t(-1);
  }
  return n;
}
}  // namespace

// int recvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen)
REACH_NET_FUNC(NetDll_recvfrom, Ours(ctx.r4.u32), {
  ctx.r3.u64 = Receive(ctx, base, ctx.r8.u32, ctx.r9.u32);
})

// int recv(SOCKET s, char* buf, int len, int flags)
REACH_NET_FUNC(NetDll_recv, Ours(ctx.r4.u32), ctx.r3.u64 = Receive(ctx, base, 0, 0))

namespace {
// Guest fd_set: u32 fd_count, SOCKET fd_array[64].
std::vector<uint32_t> ReadSet(uint8_t* base, uint32_t set) {
  std::vector<uint32_t> handles;
  if (!set) return handles;
  uint32_t count = std::min<uint32_t>(Load32(base + set), 64);
  for (uint32_t i = 0; i < count; ++i) handles.push_back(Load32(base + set + 4 + 4 * i));
  return handles;
}

void WriteSet(uint8_t* base, uint32_t set, const std::vector<uint32_t>& handles) {
  if (!set) return;
  Store32(base + set, uint32_t(handles.size()));
  for (size_t i = 0; i < handles.size(); ++i) Store32(base + set + 4 + 4 * i, handles[i]);
}

bool AllOurs(uint8_t* base, uint32_t set) {
  for (uint32_t h : ReadSet(base, set)) {
    if (!Ours(h)) return false;
  }
  return true;
}
}  // namespace

// int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, const timeval* timeout)
REACH_NET_FUNC(NetDll_select,
               NetOn() && AllOurs(base, ctx.r5.u32) && AllOurs(base, ctx.r6.u32) &&
                   AllOurs(base, ctx.r7.u32) &&
                   !(ReadSet(base, ctx.r5.u32).empty() && ReadSet(base, ctx.r6.u32).empty()),
               {
                 int64_t timeout_us = -1;
                 if (ctx.r8.u32) {
                   timeout_us = int64_t(int32_t(Load32(base + ctx.r8.u32))) * 1000000 +
                                int32_t(Load32(base + ctx.r8.u32 + 4));
                 }
                 std::vector<uint32_t> writable = ReadSet(base, ctx.r6.u32);  // always writable
                 if (!writable.empty()) timeout_us = 0;
                 std::vector<uint32_t> readable =
                     VNet::Get().WaitReadable(ReadSet(base, ctx.r5.u32), timeout_us);
                 WriteSet(base, ctx.r5.u32, readable);
                 WriteSet(base, ctx.r6.u32, writable);
                 WriteSet(base, ctx.r7.u32, {});
                 ctx.r3.u64 = readable.size() + writable.size();
               })

REACH_NET_TRACE(NetDll_WSAStartup)
REACH_NET_TRACE(NetDll_WSACleanup)
REACH_NET_TRACE(NetDll_WSAGetLastError)
REACH_NET_TRACE(NetDll___WSAFDIsSet)
REACH_NET_TRACE(NetDll_listen)
REACH_NET_TRACE(NetDll_accept)
REACH_NET_TRACE(NetDll_inet_addr)

// --- XAM sessions, voice and messages (trace only) -------------------------

REACH_NET_TRACE(XamSessionCreateHandle)
REACH_NET_TRACE(XamSessionRefObjByHandle)
REACH_NET_TRACE(XamVoiceCreate)
REACH_NET_TRACE(XamVoiceClose)
REACH_NET_TRACE(XamVoiceSubmitPacket)
REACH_NET_TRACE(XamVoiceHeadsetPresent)
// XMsg calls to XAM apps other than the music player (0xFA) carry XSession and
// XUser messages; they are always logged in full.
#define REACH_NET_TRACE_XMSG(name)                                           \
  extern "C" REX_FUNC(__imp__##name) {                                       \
    REACH_NET_SDK(name);                                                     \
    static std::atomic<uint64_t> calls{0};                                   \
    if (!NetTrace()) {                                                       \
      if (sdk) sdk(ctx, base);                                               \
      return;                                                                \
    }                                                                        \
    if (ctx.r3.u32 == 0xFA) {                                                \
      Traced(#name, sdk, calls, ctx, base);                                  \
      return;                                                                \
    }                                                                        \
    const uint32_t in[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,              \
                            ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};             \
    if (sdk) sdk(ctx, base);                                                 \
    REXLOG_INFO("NETTRACE {}(app {:02X}, msg {:08X}, {:08X}, {:08X}, {:08X}, {:08X}) -> {:08X}", \
                #name, in[0], in[1], in[2], in[3], in[4], in[5], ctx.r3.u32); \
  }

REACH_NET_TRACE_XMSG(XMsgStartIORequest)
REACH_NET_TRACE_XMSG(XMsgStartIORequestEx)
REACH_NET_TRACE_XMSG(XMsgInProcessCall)
REACH_NET_TRACE(XMsgCancelIORequest)
REACH_NET_TRACE(XMsgCompleteIORequest)
