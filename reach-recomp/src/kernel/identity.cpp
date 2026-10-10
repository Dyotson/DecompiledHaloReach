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

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

// Settings (reach.toml next to the executable, or the F4 settings overlay; they take
// effect on the next start). The environment variables override them.
REXCVAR_DEFINE_STRING(live_server, "", "Network/Reach Live",
                      "Reach Live server, host[:port] (port 21100 by default). Empty: offline "
                      "(REACH_SERVER overrides)");
REXCVAR_DEFINE_BOOL(live_signin, true, "Network/Reach Live",
                    "Sign in to Xbox LIVE through the Reach Live server: friends, Live lobbies "
                    "(REACH_LIVE_SIGNIN overrides)");
REXCVAR_DEFINE_STRING(live_room, "", "Network/Reach Live",
                      "Only players in the same room on the server see each other "
                      "(REACH_ROOM overrides)");
REXCVAR_DEFINE_STRING(gamertag, "", "Network/Reach Live",
                      "Your gamertag, 15 characters. Empty: the one in live_identity.txt "
                      "(REACH_GAMERTAG overrides)");

namespace reach {

namespace {

// An environment variable when set, else a setting.
std::string EnvOr(const char* name, const std::string& setting) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : setting;
}

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
    if (std::string tag = CleanGamertag(EnvOr("REACH_GAMERTAG", REXCVAR_GET(gamertag)));
        !tag.empty()) {
      id.gamertag = tag;
    }
    return id;
  }();
  return identity;
}

}  // namespace

std::filesystem::path DataDir() {
  std::string root;
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    root = xdg;
#ifdef _WIN32
  } else if (const char* app_data = std::getenv("LOCALAPPDATA"); app_data && *app_data) {
    root = app_data;
#endif
  } else if (const char* home = std::getenv("HOME")) {
    root = std::string(home) + "/.local/share";
  }
  return std::filesystem::path(root) / "reach" / "4D53085B";
}

const std::string& LiveServerSpec() {
  static const std::string spec = EnvOr("REACH_SERVER", REXCVAR_GET(live_server));
  return spec;
}

const std::string& LiveRoom() {
  static const std::string room = EnvOr("REACH_ROOM", REXCVAR_GET(live_room)).substr(0, 32);
  return room;
}

bool LiveMode() { return !LiveServerSpec().empty(); }

bool LiveSignin() {
  static const bool on = [] {
    const char* v = std::getenv("REACH_LIVE_SIGNIN");
    const bool want = v ? (*v && *v != '0') : REXCVAR_GET(live_signin);
    return LiveMode() && want;
  }();
  return on;
}

uint64_t IdentityXuid() { return Get().xuid; }

const std::string& IdentityGamertag() { return Get().gamertag; }

}  // namespace reach
