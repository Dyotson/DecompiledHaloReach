// reach - what other Reach Live code needs from the virtual network (net.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reach {

// The virtual network is on (REACH_NET or a Reach Live server).
bool NetworkOn();

// The Reach Live server's IPv4 address (host order) and the TCP port of its title
// servers (HTTP). False without REACH_SERVER, or before the server welcomed us.
// TCP connections the game opens to this address are sent to that port.
bool LiveServerHttp(uint32_t& ip, uint16_t& port);

// Another player in our room on the Reach Live server: a "friend" of a Live player.
struct LiveFriend {
  uint32_t id = 0;  // server id
  uint64_t xuid = 0;
  std::string gamertag;
  uint32_t state = 0;           // X_ONLINE_FRIENDSTATE flags it reported
  uint8_t session[0x3C] = {};   // XSESSION_INFO of its joinable session, zeros if none
  std::string status;           // presence line
  std::vector<uint8_t> extra;   // its hosted session's slots and QoS data (live_xmsg.cpp)
};

// The other players in the room, as of the server's last answer (refreshed every 3 s).
std::vector<LiveFriend> LiveRoster();

// Our presence: X_ONLINE_FRIENDSTATE flags, the XSESSION_INFO friends can join (null:
// none) and a status line. Sent to the server now and with every keep-alive.
void LiveSetPresence(uint32_t state, const uint8_t* session_info, const std::string& status,
                     const std::vector<uint8_t>& extra = {});

// This instance's XNADDR (36 bytes, guest layout).
void LiveSelfXnAddr(uint8_t* xnaddr);

// Game invites through the server: send one to a player in the room (with our joinable
// session), and take the last one received (false when none is waiting).
struct LiveInvite {
  uint64_t inviter_xuid = 0;
  std::string inviter;
  uint8_t session[0x3C] = {};
};
void LiveSendInvite(uint64_t xuid, const uint8_t* session_info);
bool LiveTakeInvite(LiveInvite& out);

// Called by the network thread when an invite arrives (live_xmsg.cpp).
void LiveInviteReceived(const LiveInvite& invite);

// Called by the network thread when the roster changed (live_xmsg.cpp): tells the
// game its friends' presence changed, or that friends came or went.
void LiveRosterChanged(bool membership_changed);

}  // namespace reach
