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
//   XNetXnAddrToInAddr maps a peer's XNADDR to a virtual IP 0.77.0.N (a 360
//   secure address: the game connects only to addresses whose first byte is 0) that
//   sendto/recvfrom translate back to the peer's host endpoint (this instance
//   is 0.77.0.1).
// - XNetRandom returns random bytes (the SDK's are constant). Keys are random
//   system-link keys; registering, connecting and QoS listen
//   succeed. VDP is sent unencrypted: only copies of this port talk to it.
//
// REACH_SERVER=host[:port] (default port 21100) connects to a Reach Live server
// (server/reach_live_server.py) and implies REACH_NET=1. Everyone on the server, or
// in the same REACH_ROOM on it, is on one virtual network: broadcasts go to all of
// them through the server, and datagrams for one player go directly once UDP hole
// punching opened a path (REACH_SERVER_RELAY=1 disables that), through the server
// until then. The XNADDR of a Live player carries the id the server gave it
// (0xF0000000 | id) instead of a host address. REACH_NET_PORT fixes the host UDP port,
// for a port forward.
//
// Other socket types (TCP) still go to the SDK.
//
// REACH_NETTRACE=1 logs every networking import with its raw argument
// registers r3-r8 and the result; busy calls are logged for their first 20
// calls and then every 1000th.

#include "identity.h"
#include "live.h"
#include "live_tcp.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xthread.h>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netdb.h>
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
  static const bool enabled = EnvFlag("REACH_NET") || reach::LiveMode();
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
  REXLOG_INFO("REACH_NET {} {} bytes {}:{} -> port {} vip {:08X} [{}] #{}", dir, size, (vip >> 16) == 0x004D ? "vip" : "ip",
              source_port, port, vip, hex, n);
}

constexpr uint32_t kHandleBase = 0x52450000;  // our guest SOCKET handles: 'RE' + index
constexpr uint32_t kMagic = 0x524E4554;       // 'RNET', first word of every LAN datagram
constexpr size_t kHeaderSize = 8;             // magic, guest source port, guest destination port
constexpr uint16_t kHostPortFirst = 21000;
constexpr uint16_t kHostPortCount = 8;
// Secure addresses as XNetXnAddrToInAddr returns them on a 360: first byte 0.
// Reach only connects to such addresses (sub_82273FD0).
constexpr uint32_t kSelfVip = 0x004D0001;  // 0.77.0.1
constexpr uint32_t kVipNet = 0x004D0000;   // 0.77.0.0/16
constexpr uint32_t kBroadcast = 0xFFFFFFFF;

// Reach Live server protocol (server/reach_live_server.py, docs/online_plan.md): every
// packet starts with 'RLV1' and a type byte.
constexpr uint32_t kLiveMagic = 0x524C5631;
constexpr uint16_t kLiveVersion = 1;
constexpr uint16_t kLiveDefaultPort = 21100;
enum LiveType : uint8_t {
  kHello = 1,    // client -> server: register / keep alive
  kWelcome,      // server -> client: our id, the server epoch, our public address
  kError,        // server -> client
  kBroadcastMsg, // client -> server: a broadcast datagram for everyone in the room
  kRelay,        // client -> server: a datagram for one player, through the server
  kForward,      // server -> client: a broadcast or relayed datagram and who sent it
  kPunch,        // player -> player: open a direct path through NATs
  kPunchAck,     // player -> player
  kData,         // player -> player: a datagram on the direct path
  kBye,
  kList,         // client -> server: who is in the room
  kPeers,        // server -> client: the room's players with their presence
  kPresence,     // client -> server: our presence (friend state, joinable session, status)
};
// XNADDR.ina of a player on a Reach Live server: 0xF0000000 | the id the server gave
// it. 240.0.0.0/8 is reserved, so it never collides with a LAN address.
constexpr uint32_t kLiveInaTag = 0xF0000000;

using Clock = std::chrono::steady_clock;

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
  uint32_t ip = 0;  // host order
  uint16_t port = 0;
  bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
  bool operator!=(const Endpoint& o) const { return !(*this == o); }
};

// Another player on the Reach Live server.
struct LivePeer {
  uint32_t vip = 0;
  Endpoint public_ep, local_ep;  // as the server saw it / as it reported its LAN side
  Endpoint direct;               // the path that answered a punch; ip 0: relay via the server
  Clock::time_point punch_until{}, next_punch{}, punch_started{};
};

std::string IpString(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
         std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

// Guest XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20].
// LAN form: the host IP and UDP port of the instance.
void WriteLanXnAddr(uint8_t* p, uint32_t ip, uint16_t port) {
  std::memset(p, 0, 36);
  Store32(p, ip);
  Store16(p + 8, port);
  const uint8_t mac[6] = {0x00, 0x22, 0x48, uint8_t(ip), uint8_t(port >> 8), uint8_t(port)};
  std::memcpy(p + 10, mac, 6);
}

// Reach Live form: the server id (tagged), the public IP and the server epoch.
void WriteLiveXnAddr(uint8_t* p, uint32_t id, uint16_t epoch, uint32_t public_ip) {
  std::memset(p, 0, 36);
  Store32(p, kLiveInaTag | id);
  Store32(p + 4, public_ip);
  Store16(p + 8, epoch);
  const uint8_t mac[6] = {0x02, uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id),
                          uint8_t(epoch >> 8), uint8_t(epoch)};
  std::memcpy(p + 10, mac, 6);
}

class VNet {
 public:
  static VNet& Get() {
    static VNet net;
    return net;
  }

  bool Ready() const { return fd_ >= 0; }

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
    LogPacket("send", vip, source_port, port, data, size);
    if (vip == kSelfVip) {
      Deliver(kSelfVip, source_port, port, data, size);
      return 0;
    }
    if (vip == kBroadcast || (vip & 0xFF) == 0xFF) {
      if (!live_ || lan_) {
        std::vector<uint8_t> packet = LanPacket(source_port, port, data, size);
        for (uint16_t p = kHostPortFirst; p < kHostPortFirst + kHostPortCount; ++p) {
          if (p != host_port_) SendHost({INADDR_LOOPBACK, p}, packet);
          if (lan_) SendHost({INADDR_BROADCAST, p}, packet);
        }
      }
      if (live_ && live_id_) {
        std::vector<uint8_t> body(4 + size);
        Store16(body.data(), source_port);
        Store16(body.data() + 2, port);
        std::memcpy(body.data() + 4, data, size);
        SendLive(server_, kBroadcastMsg, body);
      }
      return 0;
    }
    if (live_) {
      uint32_t id = 0;
      Endpoint direct;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = live_id_by_vip_.find(vip);
        if (it != live_id_by_vip_.end()) {
          id = it->second;
          direct = live_peers_[id].direct;
          if (!direct.ip) StartPunchLocked(id);
        }
      }
      if (id) {
        if (direct.ip && !relay_only_) {
          std::vector<uint8_t> body(8 + size);
          Store32(body.data(), live_id_);
          Store16(body.data() + 4, source_port);
          Store16(body.data() + 6, port);
          std::memcpy(body.data() + 8, data, size);
          SendLive(direct, kData, body);
        } else {
          std::vector<uint8_t> body(8 + size);
          Store32(body.data(), id);
          Store16(body.data() + 4, source_port);
          Store16(body.data() + 6, port);
          std::memcpy(body.data() + 8, data, size);
          SendLive(server_, kRelay, body);
        }
        return 0;
      }
    }
    Endpoint endpoint;
    if (!LanEndpointOf(vip, endpoint)) return kWsaENetUnreach;
    SendHost(endpoint, LanPacket(source_port, port, data, size));
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
    auto deadline = Clock::now() + std::chrono::microseconds(timeout_us);
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

  // This instance's XNADDR: the Reach Live form once the server has welcomed us,
  // else the LAN form.
  void WriteSelfXnAddr(uint8_t* xna) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (live_id_) {
      WriteLiveXnAddr(xna, live_id_, live_epoch_, public_ep_.ip);
    } else {
      WriteLanXnAddr(xna, self_ip_, host_port_);
    }
  }

  // The Reach Live server's address and the TCP port of its title servers (HTTP);
  // false without a server or before it told us the port.
  bool LiveServerHttp(uint32_t& ip, uint16_t& port) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!live_ || !server_.ip || !http_port_) return false;
    ip = server_.ip;
    port = http_port_;
    return true;
  }

  std::vector<reach::LiveFriend> Roster() {
    std::lock_guard<std::mutex> lock(mutex_);
    return roster_;
  }

  void SetPresence(uint32_t state, const uint8_t* session_info, const std::string& status) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      presence_.assign(4 + 0x3C, 0);
      Store32(presence_.data(), state);
      if (session_info) std::memcpy(presence_.data() + 4, session_info, 0x3C);
      const std::string line = status.substr(0, 255);
      presence_.push_back(uint8_t(line.size()));
      presence_.insert(presence_.end(), line.begin(), line.end());
    }
    SendPresence();
  }

  // Virtual IP of a peer's XNADDR.
  uint32_t VipOfXnAddr(const uint8_t* xna) {
    const uint32_t ina = Load32(xna);
    if ((ina & 0xFF000000) == kLiveInaTag) {
      const uint32_t id = ina & 0x00FFFFFF;
      std::lock_guard<std::mutex> lock(mutex_);
      if (id == live_id_) return kSelfVip;
      return LivePeerLocked(id).vip;
    }
    return LanVipOf({ina, Load16(xna + 8)});
  }

  // The XNADDR behind a virtual IP; false when the IP is unknown.
  bool XnAddrOfVip(uint32_t vip, uint8_t* xna) {
    if (vip == kSelfVip) {
      WriteSelfXnAddr(xna);
      return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = live_id_by_vip_.find(vip); it != live_id_by_vip_.end()) {
      WriteLiveXnAddr(xna, it->second, live_epoch_, live_peers_[it->second].public_ep.ip);
      return true;
    }
    if (auto it = endpoint_by_vip_.find(vip); it != endpoint_by_vip_.end()) {
      WriteLanXnAddr(xna, it->second.ip, it->second.port);
      return true;
    }
    return false;
  }

 private:
  VNet() {
    lan_ = EnvFlag("REACH_NET_LAN");
    live_ = reach::LiveMode();
    relay_only_ = EnvFlag("REACH_SERVER_RELAY");
    room_ = reach::LiveRoom();
    self_ip_ = INADDR_LOOPBACK;
    if (const char* ip = std::getenv("REACH_NET_IP"); ip && *ip) {
      in_addr a{};
      if (inet_pton(AF_INET, ip, &a) == 1) self_ip_ = ntohl(a.s_addr);
    } else if (lan_ || live_) {
      self_ip_ = LanAddress();
    }
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    const bool any = lan_ || live_;  // reachable from other machines
    uint16_t first = kHostPortFirst, count = kHostPortCount;
    if (const char* port = std::getenv("REACH_NET_PORT"); port && *port) {
      first = uint16_t(std::atoi(port));
      count = 1;
    }
    for (uint16_t p = first; p < first + count; ++p) {
      if (BindHost(any, p)) {
        host_port_ = p;
        break;
      }
    }
    if (!host_port_ && live_ && BindHost(any, 0)) {
      sockaddr_in a{};
      socklen_t len = sizeof(a);
      getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
      host_port_ = ntohs(a.sin_port);
    }
    if (!host_port_) {
      REXLOG_ERROR("REACH_NET: no free host UDP port in {}-{}", first, first + count - 1);
      close(fd_);
      fd_ = -1;
      return;
    }
    REXLOG_INFO("REACH_NET: virtual network on {} UDP {}{}", IpString(self_ip_), host_port_,
                lan_ ? " (LAN broadcast on)" : "");
    std::thread([this] { ReceiveLoop(); }).detach();
    if (live_) {
      std::random_device rd;
      live_token_ = uint64_t(rd()) << 32 | rd();
      std::thread([this] { LiveLoop(); }).detach();
      // Give the server a moment, so the game sees its Live address from the start.
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::seconds(3), [this] { return live_id_ != 0; });
      if (!live_id_) {
        REXLOG_WARN("REACH_LIVE: no answer from {} yet; still trying", ServerName());
      }
    }
  }

  bool BindHost(bool any, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(any ? INADDR_ANY : INADDR_LOOPBACK);
    return bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
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

  static std::vector<uint8_t> LanPacket(uint16_t source_port, uint16_t port, const uint8_t* data,
                                        size_t size) {
    std::vector<uint8_t> packet(kHeaderSize + size);
    Store32(packet.data(), kMagic);
    Store16(packet.data() + 4, source_port);
    Store16(packet.data() + 6, port);
    std::memcpy(packet.data() + kHeaderSize, data, size);
    return packet;
  }

  // Virtual IP of a LAN instance's host endpoint (its XNADDR's ina and wPortOnline).
  uint32_t LanVipOf(Endpoint endpoint) {
    if (endpoint.ip == self_ip_ && endpoint.port == host_port_) return kSelfVip;
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t key = uint64_t(endpoint.ip) << 16 | endpoint.port;
    auto it = vip_by_endpoint_.find(key);
    if (it != vip_by_endpoint_.end()) return it->second;
    uint32_t vip = kVipNet + next_vip_++;
    vip_by_endpoint_[key] = vip;
    endpoint_by_vip_[vip] = endpoint;
    REXLOG_INFO("REACH_NET: peer {}:{} is 0.77.{}.{}", IpString(endpoint.ip), endpoint.port,
                (vip >> 8) & 0xFF, vip & 0xFF);
    return vip;
  }

  bool LanEndpointOf(uint32_t vip, Endpoint& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = endpoint_by_vip_.find(vip);
    if (it == endpoint_by_vip_.end()) return false;
    out = it->second;
    return true;
  }

  // The Reach Live player with server id `id`, created on first sight. Needs mutex_.
  LivePeer& LivePeerLocked(uint32_t id) {
    LivePeer& peer = live_peers_[id];
    if (!peer.vip) {
      peer.vip = kVipNet + next_vip_++;
      live_id_by_vip_[peer.vip] = id;
      REXLOG_INFO("REACH_LIVE: player #{} is 0.77.{}.{}", id, (peer.vip >> 8) & 0xFF,
                  peer.vip & 0xFF);
    }
    return peer;
  }

  // Starts hole punching toward `id` unless a direct path exists or a recent attempt
  // failed. Needs mutex_.
  void StartPunchLocked(uint32_t id) {
    if (relay_only_) return;
    LivePeer& peer = LivePeerLocked(id);
    const auto now = Clock::now();
    if (peer.direct.ip || !peer.public_ep.ip) return;
    if (peer.punch_started != Clock::time_point{} && now - peer.punch_started < std::chrono::seconds(30)) {
      return;
    }
    peer.punch_started = now;
    peer.punch_until = now + std::chrono::seconds(6);
    peer.next_punch = now;
  }

  std::string ServerName() const { return server_host_ + ":" + std::to_string(server_port_); }

  bool ResolveServer() {
    std::string spec = reach::LiveServerSpec();
    server_port_ = kLiveDefaultPort;
    if (auto colon = spec.rfind(':'); colon != std::string::npos) {
      server_port_ = uint16_t(std::atoi(spec.c_str() + colon + 1));
      spec.resize(colon);
    }
    server_host_ = spec;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(spec.c_str(), nullptr, &hints, &result) != 0 || !result) {
      REXLOG_WARN("REACH_LIVE: cannot resolve server {}", spec);
      return false;
    }
    uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(result);
    std::lock_guard<std::mutex> lock(mutex_);
    server_ = {ip, server_port_};
    return true;
  }

  void SendLive(Endpoint to, LiveType type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> packet(5 + body.size());
    Store32(packet.data(), kLiveMagic);
    packet[4] = type;
    std::memcpy(packet.data() + 5, body.data(), body.size());
    SendHost(to, packet);
  }

  void SendHello() {
    const std::string& name = reach::IdentityGamertag();
    std::vector<uint8_t> body(24);
    Store16(body.data(), kLiveVersion);
    Store32(body.data() + 2, uint32_t(live_token_ >> 32));
    Store32(body.data() + 6, uint32_t(live_token_));
    const uint64_t xuid = reach::IdentityXuid();
    Store32(body.data() + 10, uint32_t(xuid >> 32));
    Store32(body.data() + 14, uint32_t(xuid));
    Store32(body.data() + 18, self_ip_);
    Store16(body.data() + 22, host_port_);
    body.push_back(uint8_t(room_.size()));
    body.insert(body.end(), room_.begin(), room_.end());
    body.push_back(uint8_t(name.size()));
    body.insert(body.end(), name.begin(), name.end());
    SendLive(server_, kHello, body);
  }

  void SendPresence() {
    std::vector<uint8_t> body;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!live_id_ || presence_.empty()) return;
      body = presence_;
    }
    SendLive(server_, kPresence, body);
  }

  // Registration, keep-alive, presence, the roster and hole punching.
  void LiveLoop() {
    Clock::time_point next_hello{}, next_resolve{}, next_list{};
    bool resolved = false;
    for (;;) {
      const auto now = Clock::now();
      if (!resolved && now >= next_resolve) {
        resolved = ResolveServer();
        next_resolve = now + std::chrono::seconds(10);
      }
      if (resolved && now >= next_list && reach::LiveSignin()) {
        next_list = now + std::chrono::seconds(3);
        SendLive(server_, kList, {});
      }
      if (resolved && now >= next_hello) {
        SendHello();
        SendPresence();
        std::lock_guard<std::mutex> lock(mutex_);
        next_hello = now + std::chrono::seconds(live_id_ ? 5 : 1);
        if (live_id_ && now - last_welcome_ > std::chrono::seconds(20)) {
          REXLOG_WARN("REACH_LIVE: lost the server {}; reconnecting", ServerName());
          last_welcome_ = now;
        }
      }
      std::vector<std::pair<Endpoint, std::vector<uint8_t>>> punches;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, peer] : live_peers_) {
          if (peer.direct.ip || now >= peer.punch_until || now < peer.next_punch) continue;
          peer.next_punch = now + std::chrono::milliseconds(300);
          std::vector<uint8_t> body(10);
          Store32(body.data(), live_id_);
          Store32(body.data() + 4, id);
          Store16(body.data() + 8, live_epoch_);
          punches.push_back({peer.public_ep, body});
          if (peer.local_ep.ip && peer.local_ep != peer.public_ep) {
            punches.push_back({peer.local_ep, body});
          }
        }
      }
      for (auto& [to, body] : punches) SendLive(to, kPunch, body);
      int roster_changed;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        roster_changed = roster_changed_;
        roster_changed_ = 0;
      }
      if (roster_changed) reach::LiveRosterChanged(roster_changed == 2);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void OnLive(Endpoint from, uint8_t type, const uint8_t* p, size_t n) {
    bool from_server;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      from_server = from == server_;
    }
    switch (type) {
      case kWelcome: {
        if (!from_server || n < 14) return;
        const uint32_t id = Load32(p), public_ip = Load32(p + 6);
        const uint16_t epoch = Load16(p + 4), public_port = Load16(p + 10);
        const size_t motd_size = std::min<size_t>(Load16(p + 12), n - 14);
        bool changed;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (n >= 14 + motd_size + 2) http_port_ = Load16(p + 14 + motd_size);
          changed = id != live_id_ || epoch != live_epoch_;
          live_id_ = id;
          live_epoch_ = epoch;
          public_ep_ = {public_ip, public_port};
          last_welcome_ = Clock::now();
          cv_.notify_all();
        }
        if (changed) {
          REXLOG_INFO("REACH_LIVE: signed in to {} as {} (player #{}), public address {}:{}{}{}",
                      ServerName(), reach::IdentityGamertag(), id, IpString(public_ip),
                      public_port, motd_size ? ": " : "",
                      std::string(reinterpret_cast<const char*>(p + 14), motd_size));
        }
        return;
      }
      case kError: {
        if (!from_server || n < 3) return;
        const size_t size = std::min<size_t>(Load16(p + 1), n - 3);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 20) {
          REXLOG_WARN("REACH_LIVE: server error {}: {}", p[0],
                      std::string(reinterpret_cast<const char*>(p + 3), size));
        }
        if (p[0] == 1) SendHello();  // "not registered": the server restarted
        return;
      }
      case kForward: {
        if (!from_server || n < 20) return;
        const uint32_t id = Load32(p);
        uint32_t vip;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          LivePeer& peer = LivePeerLocked(id);
          peer.public_ep = {Load32(p + 4), Load16(p + 8)};
          peer.local_ep = {Load32(p + 10), Load16(p + 14)};
          vip = peer.vip;
          if (!peer.direct.ip) StartPunchLocked(id);
        }
        Deliver(vip, Load16(p + 16), Load16(p + 18), p + 20, n - 20);
        return;
      }
      case kPunch:
      case kPunchAck: {
        if (n < 10) return;
        const uint32_t id = Load32(p);
        std::vector<uint8_t> ack;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (relay_only_ || Load32(p + 4) != live_id_ || Load16(p + 8) != live_epoch_) return;
          LivePeer& peer = LivePeerLocked(id);
          if (!peer.direct.ip) {  // the first path that works; punches may arrive on several
            REXLOG_INFO("REACH_LIVE: direct path to player #{} via {}:{}", id, IpString(from.ip),
                        from.port);
            peer.direct = from;
          }
          if (type == kPunch) {
            ack.resize(10);
            Store32(ack.data(), live_id_);
            Store32(ack.data() + 4, id);
            Store16(ack.data() + 8, live_epoch_);
          }
        }
        if (!ack.empty()) SendLive(from, kPunchAck, ack);
        return;
      }
      case kData: {
        if (n < 8) return;
        uint32_t vip;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          auto it = live_peers_.find(Load32(p));
          if (it == live_peers_.end()) return;
          LivePeer& peer = it->second;
          if (from != peer.direct && from != peer.public_ep && from != peer.local_ep) return;
          if (!peer.direct.ip) peer.direct = from;
          vip = peer.vip;
        }
        Deliver(vip, Load16(p + 4), Load16(p + 6), p + 8, n - 8);
        return;
      }
      case kPeers: {
        // Part header: total u16, first index u16, count u16; then the entries.
        if (!from_server || n < 6) return;
        const uint16_t total = Load16(p), first = Load16(p + 2), count = Load16(p + 4);
        size_t pos = 6;
        std::lock_guard<std::mutex> lock(mutex_);
        if (first == 0) roster_parts_.clear();
        if (first != roster_parts_seen_ && first != 0) return;  // a part went missing
        roster_parts_seen_ = first;
        for (uint16_t i = 0; i < count; ++i) {
          reach::LiveFriend f;
          if (pos + 13 > n) return;
          f.id = Load32(p + pos);
          f.xuid = uint64_t(Load32(p + pos + 4)) << 32 | Load32(p + pos + 8);
          const size_t name_size = p[pos + 12];
          pos += 13;
          if (pos + name_size + 4 + 0x3C + 1 > n) return;
          f.gamertag.assign(reinterpret_cast<const char*>(p + pos), name_size);
          pos += name_size;
          f.state = Load32(p + pos);
          std::memcpy(f.session, p + pos + 4, 0x3C);
          pos += 4 + 0x3C;
          const size_t status_size = std::min<size_t>(p[pos], n - pos - 1);
          f.status.assign(reinterpret_cast<const char*>(p + pos + 1), status_size);
          pos += 1 + status_size;
          roster_parts_seen_++;
          if (f.id != live_id_) roster_parts_.push_back(std::move(f));
        }
        if (roster_parts_seen_ >= total) {
          bool membership = roster_parts_.size() != roster_.size(), changed = membership;
          for (size_t i = 0; i < roster_parts_.size() && !membership; ++i) {
            const reach::LiveFriend &a = roster_parts_[i], &b = roster_[i];
            membership = a.id != b.id;
            changed = changed || membership || a.state != b.state || a.status != b.status ||
                      std::memcmp(a.session, b.session, sizeof(a.session)) != 0;
          }
          roster_ = roster_parts_;
          roster_parts_seen_ = 0;
          if (changed) roster_changed_ = membership ? 2 : std::max(roster_changed_, 1);
        }
        return;
      }
      default:
        return;
    }
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
      if (n < 5) continue;
      Endpoint endpoint{ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
      const uint32_t magic = Load32(buffer.data());
      if (magic == kLiveMagic) {
        OnLive(endpoint, buffer[4], buffer.data() + 5, size_t(n) - 5);
        continue;
      }
      if (n < ssize_t(kHeaderSize) || magic != kMagic) continue;
      if (endpoint.port == host_port_ && (endpoint.ip == self_ip_ || endpoint.ip == INADDR_LOOPBACK)) {
        continue;  // our own broadcast
      }
      uint32_t vip = LanVipOf(endpoint);
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

  // Reach Live (REACH_SERVER).
  bool live_ = false;
  bool relay_only_ = false;  // REACH_SERVER_RELAY=1: never punch, for testing the relay
  std::string room_;
  std::string server_host_;
  uint16_t server_port_ = kLiveDefaultPort;
  Endpoint server_;
  uint64_t live_token_ = 0;  // tells the server a restarted game from a stale registration
  uint32_t live_id_ = 0;     // our id on the server, 0 until it welcomes us
  uint16_t live_epoch_ = 0;
  Endpoint public_ep_;
  uint16_t http_port_ = 0;  // the server's title servers (HTTP), from its welcome
  Clock::time_point last_welcome_{};
  std::unordered_map<uint32_t, LivePeer> live_peers_;
  std::unordered_map<uint32_t, uint32_t> live_id_by_vip_;
  std::vector<uint8_t> presence_;  // body of our last presence message
  std::vector<reach::LiveFriend> roster_, roster_parts_;
  uint16_t roster_parts_seen_ = 0;
  int roster_changed_ = 0;  // 1: presence changed, 2: players came or went; LiveLoop reports it
};

bool Ours(uint32_t handle) {
  return NetOn() && (handle & 0xFFFF0000) == kHandleBase && VNet::Get().IsOurs(handle);
}

void SetError(uint32_t error) { rex::system::XThread::SetLastError(error); }

}  // namespace

namespace reach {
bool LiveServerHttp(uint32_t& ip, uint16_t& port) {
  return NetOn() && VNet::Get().LiveServerHttp(ip, port);
}

std::vector<LiveFriend> LiveRoster() {
  if (!NetOn()) return {};
  return VNet::Get().Roster();
}

void LiveSetPresence(uint32_t state, const uint8_t* session_info, const std::string& status) {
  if (NetOn()) VNet::Get().SetPresence(state, session_info, status);
}

void LiveSelfXnAddr(uint8_t* xnaddr) { VNet::Get().WriteSelfXnAddr(xnaddr); }
}  // namespace reach

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
  if (ctx.r4.u32) VNet::Get().WriteSelfXnAddr(base + ctx.r4.u32);
  // XNET_GET_XNADDR_STATIC | XNET_GET_XNADDR_ETHERNET, plus XNET_GET_XNADDR_ONLINE
  // when signed in to Live.
  ctx.r3.u64 = reach::LiveSignin() ? 0x86 : 0x06;
})

// INT XNetXnAddrToInAddr(const XNADDR* pxna, const XNKID* pxnkid, IN_ADDR* pina)
REACH_NET_FUNC(NetDll_XNetXnAddrToInAddr, NetOn() && VNet::Get().Ready(), {
  Store32(base + ctx.r6.u32, VNet::Get().VipOfXnAddr(base + ctx.r4.u32));
  ctx.r3.u64 = 0;
})

// INT XNetInAddrToXnAddr(IN_ADDR ina, XNADDR* pxna, XNKID* pxnkid)
REACH_NET_FUNC(NetDll_XNetInAddrToXnAddr, NetOn() && VNet::Get().Ready(), {
  uint8_t xna[36];
  if (VNet::Get().XnAddrOfVip(ctx.r4.u32, xna)) {
    if (ctx.r5.u32) std::memcpy(base + ctx.r5.u32, xna, sizeof(xna));
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
// INT XNetServerToInAddr(IN_ADDR ina, DWORD dwServiceId, IN_ADDR* pina): title server
// addresses (from the title server enumeration) are plain IPs here.
REACH_NET_FUNC(NetDll_XNetServerToInAddr, reach::LiveSignin() && ctx.r6.u32, {
  Store32(base + ctx.r6.u32, ctx.r4.u32);
  ctx.r3.u64 = 0;
})
REACH_NET_TRACE(NetDll_XNetQosLookup)
REACH_NET_TRACE(NetDll_XNetQosServiceLookup)
REACH_NET_TRACE(NetDll_XNetQosRelease)
REACH_NET_TRACE(NetDll_XNetQosGetListenStats)
// XNetLogonGetMachineID / XNetLogonGetTitleID: src/kernel/live_xmsg.cpp.

// --- Sockets ---------------------------------------------------------------

// --- TCP sockets (live_tcp.cpp): the title servers' HTTP connections ---------
namespace {
bool Tcp(uint32_t handle) { return NetOn() && reach::tcp::IsOurs(handle); }
uint64_t TcpResult(int32_t result) { return uint64_t(int64_t(result)); }
}  // namespace

// SOCKET socket(int af, int type, int protocol): UDP and VDP datagram sockets are ours;
// stream sockets go to live_tcp.cpp.
REACH_NET_FUNC(NetDll_socket, NetOn() && ((ctx.r5.u32 == 2 && VNet::Get().Ready()) || ctx.r5.u32 == 1), {
  ctx.r3.u64 = ctx.r5.u32 == 1 ? reach::tcp::Socket() : VNet::Get().CreateSocket();
})

// int bind(SOCKET s, const sockaddr* name, int namelen)
REACH_NET_FUNC(NetDll_bind, Ours(ctx.r4.u32), {
  uint32_t error = VNet::Get().Bind(ctx.r4.u32, Load16(base + ctx.r5.u32 + 2));
  if (error) SetError(error);
  ctx.r3.u64 = error ? uint64_t(-1) : 0;
})

// int connect(SOCKET s, const sockaddr* name, int namelen)
// Datagram sockets are ours. A TCP connection to the Reach Live server (the title
// servers, which XNetServerToInAddr resolves to it) goes to its HTTP port, whatever
// port the game asked for.
extern "C" REX_FUNC(__imp__NetDll_connect) {
  REACH_NET_SDK(NetDll_connect);
  static std::atomic<uint64_t> calls{0};
  const uint32_t in[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const uint32_t lr = uint32_t(ctx.lr);
  uint8_t* a = base + ctx.r5.u32;
  if (Tcp(ctx.r4.u32)) {
    ctx.r3.u64 = TcpResult(reach::tcp::Connect(ctx.r4.u32, Load32(a + 4), Load16(a + 2)));
    Log("NetDll_connect", calls, in, ctx.r3.u32, " (TCP)", lr);
    return;
  }
  if (Ours(ctx.r4.u32)) {
    VNet::Get().Connect(ctx.r4.u32, Load32(a + 4), Load16(a + 2));
    ctx.r3.u64 = 0;
    Log("NetDll_connect", calls, in, ctx.r3.u32, " (REACH_NET)", lr);
    return;
  }
  uint32_t server_ip;
  uint16_t http_port;
  const uint16_t port = Load16(a + 2);
  const bool to_server = NetOn() && ctx.r5.u32 && VNet::Get().LiveServerHttp(server_ip, http_port) &&
                         Load32(a + 4) == server_ip;
  if (to_server) {
    REXLOG_INFO("REACH_LIVE: title server connection (port {}) goes to {}:{}", port,
                IpString(server_ip), http_port);
    Store16(a + 2, http_port);
  }
  if (sdk) sdk(ctx, base);
  if (to_server) Store16(a + 2, port);
  Log("NetDll_connect", calls, in, ctx.r3.u32, to_server ? " (title server)" : "", lr);
}

// int closesocket(SOCKET s)
REACH_NET_FUNC(NetDll_closesocket, Ours(ctx.r4.u32) || Tcp(ctx.r4.u32), {
  if (Tcp(ctx.r4.u32)) {
    ctx.r3.u64 = TcpResult(reach::tcp::Close(ctx.r4.u32));
  } else {
    VNet::Get().Close(ctx.r4.u32);
    ctx.r3.u64 = 0;
  }
})

// int setsockopt(SOCKET s, int level, int optname, const char* optval, int optlen)
REACH_NET_FUNC(NetDll_setsockopt, Ours(ctx.r4.u32) || Tcp(ctx.r4.u32), ctx.r3.u64 = 0)
REACH_NET_FUNC(NetDll_shutdown, Ours(ctx.r4.u32) || Tcp(ctx.r4.u32), {
  ctx.r3.u64 = Tcp(ctx.r4.u32) ? TcpResult(reach::tcp::Shutdown(ctx.r4.u32, ctx.r5.u32)) : 0;
})

// int ioctlsocket(SOCKET s, long cmd, u_long* argp)
REACH_NET_FUNC(NetDll_ioctlsocket, Ours(ctx.r4.u32) || Tcp(ctx.r4.u32), {
  const uint32_t cmd = ctx.r5.u32;
  uint8_t* arg = base + ctx.r6.u32;
  if (Tcp(ctx.r4.u32)) {
    ctx.r3.u64 = TcpResult(reach::tcp::Ioctl(ctx.r4.u32, cmd, arg));
  } else if (cmd == 0x8004667E) {  // FIONBIO
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
REACH_NET_FUNC(NetDll_send, Ours(ctx.r4.u32) || Tcp(ctx.r4.u32), {
  if (Tcp(ctx.r4.u32)) {
    ctx.r3.u64 = TcpResult(reach::tcp::Send(ctx.r4.u32, base + ctx.r5.u32, ctx.r6.u32));
    return;
  }
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
REACH_NET_FUNC(NetDll_recv, Ours(ctx.r4.u32) || Tcp(ctx.r4.u32), {
  ctx.r3.u64 = Tcp(ctx.r4.u32)
                   ? TcpResult(reach::tcp::Recv(ctx.r4.u32, base + ctx.r5.u32, ctx.r6.u32))
                   : Receive(ctx, base, 0, 0);
})

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
               NetOn() && (reach::tcp::AllOurs(base, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32) ||
                           (AllOurs(base, ctx.r5.u32) && AllOurs(base, ctx.r6.u32) &&
                            AllOurs(base, ctx.r7.u32) &&
                            !(ReadSet(base, ctx.r5.u32).empty() &&
                              ReadSet(base, ctx.r6.u32).empty()))),
               {
                 if (reach::tcp::AllOurs(base, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32)) {
                   ctx.r3.u64 = TcpResult(reach::tcp::Select(base, ctx.r5.u32, ctx.r6.u32,
                                                             ctx.r7.u32, ctx.r8.u32));
                   return;
                 }
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

REACH_NET_TRACE(XamShowSigninUI)
REACH_NET_TRACE(XamShowFriendRequestUI)
REACH_NET_TRACE(XamShowGamerCardUIForXUID)
REACH_NET_TRACE(XamSessionCreateHandle)
REACH_NET_TRACE(XamSessionRefObjByHandle)
REACH_NET_TRACE(XamVoiceCreate)
REACH_NET_TRACE(XamVoiceClose)
REACH_NET_TRACE(XamVoiceSubmitPacket)
REACH_NET_TRACE(XamVoiceHeadsetPresent)
// XMsgStartIORequest[Ex] and XMsgInProcessCall: src/kernel/live_xmsg.cpp.
REACH_NET_TRACE(XMsgCancelIORequest)
REACH_NET_TRACE(XMsgCompleteIORequest)
