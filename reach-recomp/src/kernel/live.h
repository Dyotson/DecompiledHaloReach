// reach - what other Reach Live code needs from the virtual network (net.cpp).

#pragma once

#include <cstdint>

namespace reach {

// The Reach Live server's IPv4 address (host order) and the TCP port of its title
// servers (HTTP). False without REACH_SERVER, or before the server welcomed us.
// TCP connections the game opens to this address are sent to that port.
bool LiveServerHttp(uint32_t& ip, uint16_t& port);

}  // namespace reach
