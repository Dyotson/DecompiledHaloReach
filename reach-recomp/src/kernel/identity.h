// reach - who the local player is (see identity.cpp).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace reach {

// Directory for per-installation data: $XDG_DATA_HOME/reach/4D53085B
// (default ~/.local/share/reach/4D53085B; %LOCALAPPDATA%\reach\4D53085B on Windows).
std::filesystem::path DataDir();

// The Reach Live server, host[:port]: REACH_SERVER, else the live_server setting.
const std::string& LiveServerSpec();

// The room on the server: REACH_ROOM, else the live_room setting.
const std::string& LiveRoom();

// A Reach Live server is configured.
bool LiveMode();

// Live mode and the live_signin setting (on by default; REACH_LIVE_SIGNIN=0/1
// overrides): the profile reports "signed in to Xbox Live".
bool LiveSignin();

// XUID of the signed-in player: REACH_XUID, else the online XUID of the saved Live
// identity in Live mode, else 0 (keep the SDK's profile).
uint64_t IdentityXuid();

// Gamertag of the signed-in player: REACH_GAMERTAG, else the saved Live identity's in
// Live mode, else empty (keep the SDK's profile).
const std::string& IdentityGamertag();

}  // namespace reach
