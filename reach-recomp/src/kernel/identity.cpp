// reach - who the local player is.
//
// The ReXGlue runtime has one hard-coded profile (XUID 0xB13EBABEBABEBABE, "User").
// REACH_XUID=<hex> and REACH_GAMERTAG=<name> replace it. With a Reach Live server
// (REACH_SERVER) the player also needs an identity that stays the same between runs
// and is unique among other players: the first run in Live mode creates
// DataDir()/live_identity.txt with an online XUID (0009xxxxxxxxxxxx, the form Xbox
// Live XUIDs take) and a gamertag (the login name, or "Spartan" and 4 digits). Edit the
// file to change the gamertag.

#include "identity.h"

#include <rex/logging.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

namespace reach {

namespace {

struct Identity {
  uint64_t xuid = 0;
  std::string gamertag;
};

std::string CleanGamertag(const std::string& raw) {
  std::string out;
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') out += c;
    if (out.size() == 15) break;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

Identity LoadOrCreateLiveIdentity() {
  Identity id;
  const std::filesystem::path path = DataDir() / "live_identity.txt";
  if (std::ifstream in(path); in) {
    std::string line;
    while (std::getline(in, line)) {
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string key = line.substr(0, eq), value = line.substr(eq + 1);
      if (key == "xuid") id.xuid = std::strtoull(value.c_str(), nullptr, 16);
      if (key == "gamertag") id.gamertag = CleanGamertag(value);
    }
  }
  bool changed = false;
  std::random_device rd;
  if ((id.xuid >> 48) != 0x0009) {
    id.xuid = 0x0009000000000000ull | ((uint64_t(rd()) << 32 | rd()) & 0x0000FFFFFFFFFFFFull);
    changed = true;
  }
  if (id.gamertag.empty()) {
    const char* user = std::getenv("USER");
    id.gamertag = CleanGamertag(user ? user : "");
    if (id.gamertag.size() < 3) id.gamertag = "Spartan" + std::to_string(1000 + rd() % 9000);
    changed = true;
  }
  if (changed) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    char xuid[17];
    std::snprintf(xuid, sizeof(xuid), "%016llX", (unsigned long long)id.xuid);
    out << "# Reach Live identity: your online XUID and gamertag (15 characters max).\n"
        << "xuid=" << xuid << "\ngamertag=" << id.gamertag << "\n";
    REXLOG_INFO("REACH_LIVE: created identity {} ({})", id.gamertag, path.string());
  }
  return id;
}

const Identity& Get() {
  static const Identity identity = [] {
    Identity id;
    if (LiveMode()) id = LoadOrCreateLiveIdentity();
    if (const char* v = std::getenv("REACH_XUID"); v && *v) id.xuid = std::strtoull(v, nullptr, 16);
    if (const char* v = std::getenv("REACH_GAMERTAG"); v && *v) id.gamertag = CleanGamertag(v);
    return id;
  }();
  return identity;
}

}  // namespace

std::filesystem::path DataDir() {
  std::string root;
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    root = xdg;
  } else if (const char* home = std::getenv("HOME")) {
    root = std::string(home) + "/.local/share";
  }
  return std::filesystem::path(root) / "reach" / "4D53085B";
}

bool LiveMode() {
  static const bool on = [] {
    const char* v = std::getenv("REACH_SERVER");
    return v && *v;
  }();
  return on;
}

uint64_t IdentityXuid() { return Get().xuid; }

const std::string& IdentityGamertag() { return Get().gamertag; }

}  // namespace reach
