// reach - who the local player is (see identity.cpp).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace reach {

// Directory for per-installation data: $XDG_DATA_HOME/reach/4D53085B
// (default ~/.local/share/reach/4D53085B).
std::filesystem::path DataDir();

// REACH_SERVER is set: the game is connected to a Reach Live server.
bool LiveMode();

// Live mode and REACH_LIVE_SIGNIN=1: the profile reports "signed in to Xbox Live".
bool LiveSignin();

// XUID of the signed-in player: REACH_XUID, else the online XUID of the saved Live
// identity in Live mode, else 0 (keep the SDK's profile).
uint64_t IdentityXuid();

// Gamertag of the signed-in player: REACH_GAMERTAG, else the saved Live identity's in
// Live mode, else empty (keep the SDK's profile).
const std::string& IdentityGamertag();

}  // namespace reach
