// reach - host sockets on Linux and Windows (see socket.h).

#include "socket.h"

#ifdef _WIN32
#include <mutex>
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace reach::sock {

#ifdef _WIN32

void Startup() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
}

int Close(Handle s) { return closesocket(s); }

uint32_t LastError() { return uint32_t(WSAGetLastError()); }

bool SetNonblocking(Handle s, bool nonblocking) {
  u_long value = nonblocking ? 1 : 0;
  return ioctlsocket(s, FIONBIO, &value) == 0;
}

bool BytesAvailable(Handle s, uint32_t& bytes) {
  u_long value = 0;
  if (ioctlsocket(s, FIONREAD, &value) != 0) return false;
  bytes = uint32_t(value);
  return true;
}

int Poll(PollFd* fds, size_t count, int timeout_ms) {
  return WSAPoll(fds, ULONG(count), timeout_ms);
}

int64_t Send(Handle s, const void* data, size_t size) {
  return send(s, static_cast<const char*>(data), int(size), 0);
}

int64_t Recv(Handle s, void* data, size_t size) {
  return recv(s, static_cast<char*>(data), int(size), 0);
}

int64_t SendTo(Handle s, const void* data, size_t size, const sockaddr_in& to) {
  return sendto(s, static_cast<const char*>(data), int(size), 0,
                reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

int64_t RecvFrom(Handle s, void* data, size_t size, sockaddr_in& from) {
  int from_len = sizeof(from);
  return recvfrom(s, static_cast<char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(&from),
                  &from_len);
}

#else

void Startup() {}

int Close(Handle s) { return close(s); }

uint32_t LastError() {
  switch (errno) {
    case EAGAIN:
    case EINPROGRESS:  // Winsock reports a pending nonblocking connect as WOULDBLOCK
      return kEWouldBlock;
    case EALREADY:
      return 10037;
    case EBADF:
      return 10009;
    case EMSGSIZE:
      return 10040;
    case ENETUNREACH:
      return 10051;
    case ECONNABORTED:
      return 10053;
    case ECONNRESET:
      return 10054;
    case ENOBUFS:
    case ENOMEM:
      return 10055;
    case EISCONN:
      return 10056;
    case ENOTCONN:
      return kENotConn;
    case EPIPE:
      return 10058;  // WSAESHUTDOWN
    case ETIMEDOUT:
      return 10060;
    case ECONNREFUSED:
      return 10061;
    case EHOSTUNREACH:
      return 10065;
    default:
      return kEInval;
  }
}

bool SetNonblocking(Handle s, bool nonblocking) {
  int flags = fcntl(s, F_GETFL, 0);
  flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return fcntl(s, F_SETFL, flags) == 0;
}

bool BytesAvailable(Handle s, uint32_t& bytes) {
  int value = 0;
  if (ioctl(s, FIONREAD, &value) != 0) return false;
  bytes = uint32_t(value);
  return true;
}

int Poll(PollFd* fds, size_t count, int timeout_ms) { return poll(fds, nfds_t(count), timeout_ms); }

int64_t Send(Handle s, const void* data, size_t size) { return send(s, data, size, MSG_NOSIGNAL); }

int64_t Recv(Handle s, void* data, size_t size) { return recv(s, data, size, 0); }

int64_t SendTo(Handle s, const void* data, size_t size, const sockaddr_in& to) {
  return sendto(s, data, size, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

int64_t RecvFrom(Handle s, void* data, size_t size, sockaddr_in& from) {
  socklen_t from_len = sizeof(from);
  return recvfrom(s, data, size, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
}

#endif

}  // namespace reach::sock
