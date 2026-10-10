// reach - host sockets on Linux and Windows, with Winsock error codes for the guest.

#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#include <cstddef>
#include <cstdint>

namespace reach::sock {

#ifdef _WIN32
using Handle = SOCKET;
inline constexpr Handle kInvalid = INVALID_SOCKET;
using PollFd = WSAPOLLFD;
inline constexpr short kPollExcept = 0;  // WSAPoll rejects POLLPRI; errors are always reported
#else
using Handle = int;
inline constexpr Handle kInvalid = -1;
using PollFd = pollfd;
inline constexpr short kPollExcept = POLLPRI;
#endif

// Winsock errors as the guest sees them (the same numbers on Windows).
inline constexpr uint32_t kEWouldBlock = 10035;
inline constexpr uint32_t kENotSock = 10038;
inline constexpr uint32_t kEInval = 10022;
inline constexpr uint32_t kENotConn = 10057;

// Initializes the socket library once (Windows); call before the first socket.
void Startup();

int Close(Handle s);

// The last socket error of this thread as a Winsock code.
uint32_t LastError();

bool SetNonblocking(Handle s, bool nonblocking);

// Bytes waiting to be received (FIONREAD).
bool BytesAvailable(Handle s, uint32_t& bytes);

int Poll(PollFd* fds, size_t count, int timeout_ms);

// send() that never raises SIGPIPE; negative on error.
int64_t Send(Handle s, const void* data, size_t size);

int64_t Recv(Handle s, void* data, size_t size);

int64_t SendTo(Handle s, const void* data, size_t size, const sockaddr_in& to);

int64_t RecvFrom(Handle s, void* data, size_t size, sockaddr_in& from);

}  // namespace reach::sock
