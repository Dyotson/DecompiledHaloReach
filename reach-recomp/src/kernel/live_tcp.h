// reach - the game's TCP sockets (live_tcp.cpp), for the title servers ("LSP").

#pragma once

#include <cstdint>

namespace reach::tcp {

// True for a guest SOCKET handle from Socket().
bool IsOurs(uint32_t handle);

// Each returns what the Winsock call returns to the guest (handle or 0/-1, byte counts)
// and sets the guest's last error on failure.
uint32_t Socket();
int32_t Connect(uint32_t handle, uint32_t ip, uint16_t port);
int32_t Ioctl(uint32_t handle, uint32_t cmd, uint8_t* arg);  // arg: guest memory (big-endian)
int32_t Send(uint32_t handle, const uint8_t* data, uint32_t size);
int32_t Recv(uint32_t handle, uint8_t* data, uint32_t size);
int32_t Shutdown(uint32_t handle, uint32_t how);
int32_t Close(uint32_t handle);

// select() over guest fd_sets (u32 count, SOCKET handles[64]) that hold only our
// sockets; `timeout` is a guest timeval address or 0 (wait forever).
int32_t Select(uint8_t* base, uint32_t readfds, uint32_t writefds, uint32_t exceptfds,
               uint32_t timeout);

// True when every handle in the given guest fd_sets is ours and at least one is set.
bool AllOurs(uint8_t* base, uint32_t readfds, uint32_t writefds, uint32_t exceptfds);

}  // namespace reach::tcp
