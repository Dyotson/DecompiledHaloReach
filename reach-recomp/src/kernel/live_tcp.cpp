// reach - the game's TCP sockets, with Winsock behaviour.
//
// Reach talks HTTP/1.0 to its title servers over nonblocking TCP: socket, ioctlsocket
// (FIONBIO), connect (expects WSAEWOULDBLOCK), select for writability, send, recv,
// closesocket (the transport code around sub_822A4C58). The SDK's host sockets pass
// the Windows ioctl code to Linux and report no Winsock errors, so every connection
// failed before connect. With the virtual network on (REACH_NET / REACH_SERVER),
// net.cpp hands stream sockets to this file instead: host TCP sockets, Winsock error
// codes, and connections to the Reach Live server's address sent to its HTTP port
// (reach::LiveServerHttp), whatever LSP port the game picked.

#include "live_tcp.h"

#include "live.h"

#include <rex/logging.h>
#include <rex/system/xthread.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace reach::tcp {

namespace {

constexpr uint32_t kHandleBase = 0x52540000;  // 'RT' + index
constexpr uint32_t kFionbio = 0x8004667E;
constexpr uint32_t kFionread = 0x4004667F;

// Winsock errors as the guest sees them.
constexpr uint32_t kWsaEBadf = 10009;
constexpr uint32_t kWsaEInval = 10022;
constexpr uint32_t kWsaEWouldBlock = 10035;
constexpr uint32_t kWsaEAlready = 10037;
constexpr uint32_t kWsaENotSock = 10038;
constexpr uint32_t kWsaEMsgSize = 10040;
constexpr uint32_t kWsaENetUnreach = 10051;
constexpr uint32_t kWsaEConnAborted = 10053;
constexpr uint32_t kWsaEConnReset = 10054;
constexpr uint32_t kWsaENoBufs = 10055;
constexpr uint32_t kWsaEIsConn = 10056;
constexpr uint32_t kWsaENotConn = 10057;
constexpr uint32_t kWsaEShutdown = 10058;
constexpr uint32_t kWsaETimedOut = 10060;
constexpr uint32_t kWsaEConnRefused = 10061;
constexpr uint32_t kWsaEHostUnreach = 10065;

uint32_t WsaError(int e) {
  switch (e) {
    case EAGAIN:
    case EINPROGRESS:  // Winsock reports a pending nonblocking connect as WOULDBLOCK
      return kWsaEWouldBlock;
    case EALREADY:
      return kWsaEAlready;
    case EBADF:
      return kWsaEBadf;
    case EMSGSIZE:
      return kWsaEMsgSize;
    case ENETUNREACH:
      return kWsaENetUnreach;
    case ECONNABORTED:
      return kWsaEConnAborted;
    case ECONNRESET:
      return kWsaEConnReset;
    case ENOBUFS:
    case ENOMEM:
      return kWsaENoBufs;
    case EISCONN:
      return kWsaEIsConn;
    case ENOTCONN:
      return kWsaENotConn;
    case EPIPE:
      return kWsaEShutdown;
    case ETIMEDOUT:
      return kWsaETimedOut;
    case ECONNREFUSED:
      return kWsaEConnRefused;
    case EHOSTUNREACH:
      return kWsaEHostUnreach;
    default:
      return kWsaEInval;
  }
}

int32_t Fail(uint32_t error) {
  rex::system::XThread::SetLastError(error);
  return -1;
}

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

std::mutex mutex;
std::unordered_map<uint32_t, int> fds;  // guest handle -> host fd
uint32_t next_handle = 1;

int Fd(uint32_t handle) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = fds.find(handle);
  return it == fds.end() ? -1 : it->second;
}

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

}  // namespace

bool IsOurs(uint32_t handle) {
  return (handle & 0xFFFF0000) == kHandleBase && Fd(handle) >= 0;
}

uint32_t Socket() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return uint32_t(Fail(WsaError(errno)));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  std::lock_guard<std::mutex> lock(mutex);
  uint32_t handle = kHandleBase + (next_handle++ & 0xFFFF);
  fds[handle] = fd;
  return handle;
}

int32_t Connect(uint32_t handle, uint32_t ip, uint16_t port) {
  int fd = Fd(handle);
  if (fd < 0) return Fail(kWsaENotSock);
  uint32_t server_ip;
  uint16_t http_port;
  if (reach::LiveServerHttp(server_ip, http_port) && ip == server_ip) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 10) {
      REXLOG_INFO("REACH_LIVE: title server connection (LSP port {}) goes to HTTP port {}", port,
                  http_port);
    }
    port = http_port;
  }
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(ip);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) return 0;
  return Fail(WsaError(errno));
}

int32_t Ioctl(uint32_t handle, uint32_t cmd, uint8_t* arg) {
  int fd = Fd(handle);
  if (fd < 0) return Fail(kWsaENotSock);
  if (cmd == kFionbio) {
    int flags = fcntl(fd, F_GETFL, 0);
    flags = Load32(arg) ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(fd, F_SETFL, flags) != 0) return Fail(WsaError(errno));
    return 0;
  }
  if (cmd == kFionread) {
    int available = 0;
    if (ioctl(fd, FIONREAD, &available) != 0) return Fail(WsaError(errno));
    Store32(arg, uint32_t(available));
    return 0;
  }
  return Fail(kWsaEInval);
}

int32_t Send(uint32_t handle, const uint8_t* data, uint32_t size) {
  int fd = Fd(handle);
  if (fd < 0) return Fail(kWsaENotSock);
  ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
  return n < 0 ? Fail(WsaError(errno)) : int32_t(n);
}

int32_t Recv(uint32_t handle, uint8_t* data, uint32_t size) {
  int fd = Fd(handle);
  if (fd < 0) return Fail(kWsaENotSock);
  ssize_t n = recv(fd, data, size, 0);
  return n < 0 ? Fail(WsaError(errno)) : int32_t(n);
}

int32_t Shutdown(uint32_t handle, uint32_t how) {
  int fd = Fd(handle);
  if (fd < 0) return Fail(kWsaENotSock);
  // SD_RECEIVE 0, SD_SEND 1, SD_BOTH 2 match SHUT_RD / SHUT_WR / SHUT_RDWR.
  if (shutdown(fd, int(how)) != 0 && errno != ENOTCONN) return Fail(WsaError(errno));
  return 0;
}

int32_t Close(uint32_t handle) {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = fds.find(handle);
    if (it == fds.end()) return Fail(kWsaENotSock);
    fd = it->second;
    fds.erase(it);
  }
  close(fd);
  return 0;
}

bool AllOurs(uint8_t* base, uint32_t readfds, uint32_t writefds, uint32_t exceptfds) {
  size_t total = 0;
  for (uint32_t set : {readfds, writefds, exceptfds}) {
    for (uint32_t h : ReadSet(base, set)) {
      if (!IsOurs(h)) return false;
      ++total;
    }
  }
  return total != 0;
}

int32_t Select(uint8_t* base, uint32_t readfds, uint32_t writefds, uint32_t exceptfds,
               uint32_t timeout) {
  const std::vector<uint32_t> sets[3] = {ReadSet(base, readfds), ReadSet(base, writefds),
                                         ReadSet(base, exceptfds)};
  std::vector<pollfd> polls;
  std::vector<std::pair<int, uint32_t>> owners;  // which set and handle each pollfd is for
  for (int s = 0; s < 3; ++s) {
    for (uint32_t h : sets[s]) {
      int fd = Fd(h);
      if (fd < 0) return Fail(kWsaENotSock);
      short events = s == 0 ? POLLIN : s == 1 ? POLLOUT : POLLPRI;
      polls.push_back({fd, events, 0});
      owners.push_back({s, h});
    }
  }
  int wait_ms = -1;
  if (timeout) {
    int64_t us = int64_t(int32_t(Load32(base + timeout))) * 1000000 +
                 int32_t(Load32(base + timeout + 4));
    wait_ms = int(std::max<int64_t>(0, (us + 999) / 1000));
  }
  int n = poll(polls.data(), polls.size(), wait_ms);
  if (n < 0) return Fail(WsaError(errno));
  std::vector<uint32_t> ready[3];
  for (size_t i = 0; i < polls.size(); ++i) {
    const short r = polls[i].revents;
    const int s = owners[i].first;
    bool hit = false;
    if (s == 0) hit = r & (POLLIN | POLLHUP | POLLERR);
    // A failed nonblocking connect shows in the except set on Winsock, not as writable.
    if (s == 1) hit = (r & POLLOUT) && !(r & POLLERR);
    if (s == 2) hit = r & (POLLPRI | POLLERR);
    if (hit) ready[s].push_back(owners[i].second);
  }
  WriteSet(base, readfds, ready[0]);
  WriteSet(base, writefds, ready[1]);
  WriteSet(base, exceptfds, ready[2]);
  return int32_t(ready[0].size() + ready[1].size() + ready[2].size());
}

}  // namespace reach::tcp
