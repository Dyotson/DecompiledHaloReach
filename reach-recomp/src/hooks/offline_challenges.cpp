// reach - daily and weekly challenges without Bungie's servers.
//
// On a 360 the rewards sync (`/gameapi_omaha/UserUpdateRewards.ashx`) returns a `dcha`
// chunk that selects the active challenges; their definitions are local tag data
// (`chdg`). Offline nothing selects them, a profile not signed in to Live never gets
// challenges enabled, and progress only counts while the rewards are "synced". The hooks
// in hints/offline_challenges.toml:
//
// - ReachChallengeSigninFilter: the challenge listener also attaches local profiles.
// - ReachChallengeSyncGate, ReachChallengeUiSynced: progress counts, and the START menu
//   and the challenge list open, without a server sync.
// - ReachChallengeTick: each rewards tick, if the day or week changed, builds a `dcha` for
//   it (four dailies: one bounty, campaign, Firefight and multiplayer challenge; one
//   weekly), picked deterministically from the date, and applies it with the game's own
//   Challenges_ApplyDchaChunk. Challenges marked "matchmaking only" are made to count
//   outside matchmaking. Progress is saved to
//   $XDG_DATA_HOME/reach/4D53085B/challenges_<xuid>.bin (default ~/.local/share) and
//   given back to the game after a restart as a `chpr` chunk, as the server would.
//
// Days start at REACH_CHALLENGE_RESET_UTC_HOUR (default 10) UTC, weeks on Tuesdays.
// Debugging: REACH_CHALLENGE_DUMP=1 logs every definition (category, index, target, cR,
// modes, matchmaking flags, map); REACH_CHALLENGE_PICK="cat:idx,...[;cat:idx]" forces the
// daily picks and optionally the weekly one (category 1 only: other categories in the
// weekly set made the map loader fault), with set ids derived from the string.
// REACH_OFFLINE_CHALLENGES=0 turns all of this off. Addresses: docs/progression_re.md.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>

static_assert(offsetof(PPCContext, r3) == 0, "hooks recover ctx from &ctx.r3");

REX_EXTERN(sub_824BBF98);  // Challenges_ApplyDchaChunk(ctrl, chunk)
REX_EXTERN(sub_8258E2D0);  // challenge definition (category, index) or 0

namespace {

constexpr uint32_t kChallengeState = 0x8373EE38;  // u8 mask: progress loaded, per controller
constexpr uint32_t kDailySet = 0x8373EE40;        // u32 id, u64 expiry, u32 count, entries
constexpr uint32_t kSetStride = 0x630;            // weekly set follows the daily set
constexpr uint32_t kEntryStride = 0x9C;
constexpr uint32_t kSetsLoaded = 0x8373FAA0;      // u8
constexpr uint32_t kEnabled = 0x8373FB10;         // u8 per controller
constexpr uint32_t kSavedPending = 0x8373FB18;    // u8 mask: saved progress to apply
constexpr uint32_t kSavedProgress = 0x8373FB19;   // chpr chunk, 100 bytes per controller

constexpr int kMaxEntries = 10;
constexpr int32_t kCompleted = 0x7FFFFFFF;

bool Enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("REACH_OFFLINE_CHALLENGES");
    return !(v && *v == '0');
  }();
  return enabled;
}

uint8_t* Base() { return rex::system::kernel_state()->memory()->virtual_membase(); }

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Store16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void Store32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
void Store64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i));
}

uint64_t SplitMix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

struct Period {
  uint32_t daily_id, weekly_id;
  uint64_t daily_end, weekly_end;  // FILETIME of the next reset
};

Period CurrentPeriod() {
  static const int64_t reset_hour = [] {
    const char* v = std::getenv("REACH_CHALLENGE_RESET_UTC_HOUR");
    return int64_t(v && *v ? std::atoi(v) : 10);
  }();
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const int64_t day_offset = reset_hour * 3600;
  const int64_t week_offset = 5 * 86400 + day_offset;  // 1970-01-01 was a Thursday
  const int64_t day = (now - day_offset) / 86400;
  const int64_t week = (now - week_offset) / 604800;
  auto filetime = [](int64_t unix_seconds) {
    return uint64_t(unix_seconds + 11644473600ll) * 10000000ull;
  };
  Period period{0x44000000u | uint32_t(day & 0xFFFFFF), 0x57000000u | uint32_t(week & 0xFFFFFF),
                filetime((day + 1) * 86400 + day_offset),
                filetime((week + 1) * 604800 + week_offset)};
  if (const char* pick = std::getenv("REACH_CHALLENGE_PICK"); pick && *pick) {
    uint32_t hash = uint32_t(SplitMix(std::hash<std::string>()(pick)));
    period.daily_id = 0x50000000u | (hash & 0xFFFFFF);
    period.weekly_id = 0x51000000u | (hash & 0xFFFFFF);
  }
  return period;
}

// Calls a guest function with r3/r4 and returns r3, on a copy of the hooked context.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3, uint32_t r4) {
  PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.lr = 0x8258EB78;
  fn(ctx, base);
  uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// Picks a challenge of `category` from the tag data: one with a game mode (+0x30) and a
// cR reward (+0x18; the tag also has empty placeholder entries), preferring ones valid on
// any map (+0x34 == 0).
int PickChallenge(PPCContext& ctx, uint8_t* base, uint8_t category, uint64_t seed) {
  int any_map[64], usable[64], count_any = 0, count = 0;
  for (int index = 0; index < 64; ++index) {
    uint32_t def = CallGuest(ctx, base, sub_8258E2D0, category, uint32_t(index));
    if (!def) break;
    if (!base[def + 0x30] || int32_t(Load32(base + def + 0x18)) <= 0) continue;
    usable[count++] = index;
    if (Load32(base + def + 0x34) == 0) any_map[count_any++] = index;
  }
  if (count_any) return any_map[SplitMix(seed) % uint64_t(count_any)];
  if (count) return usable[SplitMix(seed) % uint64_t(count)];
  return -1;
}

std::filesystem::path SavePath() {
  std::string root;
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    root = xdg;
  } else if (const char* home = std::getenv("HOME")) {
    root = std::string(home) + "/.local/share";
  }
  uint64_t xuid = 0;
  if (const char* v = std::getenv("REACH_XUID"); v && *v) {
    xuid = std::strtoull(v, nullptr, 16);
  } else if (auto* profile = rex::system::kernel_state()->user_profile()) {
    xuid = profile->xuid();
  }
  char name[64];
  std::snprintf(name, sizeof(name), "challenges_%016llX.bin", (unsigned long long)xuid);
  return std::filesystem::path(root) / "reach" / "4D53085B" / name;
}

// Saved progress: ids and the progress of each entry of both sets.
struct SavedProgress {
  char magic[4] = {'R', 'C', 'H', 'L'};
  uint32_t daily_id = 0, weekly_id = 0;
  std::array<int32_t, kMaxEntries> daily{}, weekly{};
};

bool LoadSaved(SavedProgress& out) {
  FILE* f = std::fopen(SavePath().c_str(), "rb");
  if (!f) return false;
  bool ok = std::fread(&out, sizeof(out), 1, f) == 1 && std::memcmp(out.magic, "RCHL", 4) == 0;
  std::fclose(f);
  return ok;
}

void WriteSaved(const SavedProgress& in) {
  std::filesystem::path path = SavePath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(&in, sizeof(in), 1, f);
    std::fclose(f);
  }
}

// Progress of the controller's entries in a set; completed ones as kCompleted.
std::array<int32_t, kMaxEntries> ReadProgress(const uint8_t* base, uint32_t set, uint32_t ctrl) {
  std::array<int32_t, kMaxEntries> progress{};
  uint32_t count = std::min<uint32_t>(Load32(base + set + 0x10), kMaxEntries);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = base + set + 0x14 + i * kEntryStride;
    int32_t value = int32_t(Load32(entry + 0xC + 8 * ctrl));
    int32_t target = int32_t(Load32(entry + 0x40));
    progress[i] = target > 0 && value >= target ? kCompleted : value;
  }
  return progress;
}

void ApplyPeriod(PPCContext& ctx, uint8_t* base, uint32_t ctrl, const Period& period) {
  // The dcha chunk (header + payload, 0x256 bytes) on the guest stack below r1.
  PPCContext saved = ctx;
  ctx.r1.u32 = (ctx.r1.u32 - 0x400) & ~0xFu;
  Store32(base + ctx.r1.u32, saved.r1.u32);  // back chain
  const uint32_t chunk = ctx.r1.u32 + 0x80;
  uint8_t* p = base + chunk;
  std::memset(p, 0, 0x256);
  Store32(p, 0x64636861);  // 'dcha'
  Store32(p + 4, 0x256);
  Store16(p + 8, 3);
  Store32(p + 0x0C, period.daily_id);
  Store32(p + 0x10, period.weekly_id);
  Store64(p + 0x14, period.daily_end);
  Store64(p + 0x1C, period.weekly_end);
  auto add_entry = [&](uint32_t offset, uint8_t category, int index) {
    uint8_t* e = p + offset;
    e[0] = category;
    e[1] = uint8_t(index);
    std::memset(e + 2, 0xFF, 0x1C - 2);  // every override -1: use the tag values
  };
  if (const char* v = std::getenv("REACH_CHALLENGE_DUMP"); v && *v == '1') {
    for (uint8_t category = 0; category < 5; ++category) {
      for (int i = 0; i < 64; ++i) {
        uint32_t def = CallGuest(ctx, base, sub_8258E2D0, category, uint32_t(i));
        if (!def) break;
        const uint8_t* d = base + def;
        REXLOG_INFO("Challenge {}:{} target {} cR {} events {:08X} modes {:02X} mm {:02X} "
                    "difficulty {:02X} players {:02X} map {:08X} param {:08X}",
                    category, i, int32_t(Load32(d + 0x14)), int32_t(Load32(d + 0x18)),
                    Load32(d + 0x2C), d[0x30], d[0x31], d[0x32], d[0x33], Load32(d + 0x34),
                    Load32(d + 0x40));
      }
    }
  }
  int daily = 0, weekly = 0;
  if (const char* pick = std::getenv("REACH_CHALLENGE_PICK"); pick && *pick) {
    std::string spec(pick);
    size_t split = spec.find(';');
    auto add_list = [&](const std::string& list, uint32_t first, int& count) {
      size_t pos = 0;
      while (pos < list.size() && count < 4) {
        size_t end = list.find(',', pos);
        std::string item = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? list.size() : end + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        add_entry(first + count++ * 0x1C, uint8_t(std::atoi(item.c_str())),
                  std::atoi(item.c_str() + colon + 1));
      }
    };
    add_list(spec.substr(0, split), 0x26, daily);
    if (split != std::string::npos) {
      add_list(spec.substr(split + 1), 0x13E, weekly);
    } else {
      int index = PickChallenge(ctx, base, 1, period.weekly_id * 16ull + 1);
      if (index >= 0) add_entry(0x13E + weekly++ * 0x1C, 1, index);
    }
  } else {
    for (uint8_t category : {0, 2, 3, 4}) {
      int index = PickChallenge(ctx, base, category, period.daily_id * 16ull + category);
      if (index >= 0) add_entry(0x26 + daily++ * 0x1C, category, index);
    }
    int index = PickChallenge(ctx, base, 1, period.weekly_id * 16ull + 1);
    if (index >= 0) add_entry(0x13E + weekly++ * 0x1C, 1, index);
  }
  p[0x24] = uint8_t(daily);
  p[0x25] = uint8_t(weekly);
  CallGuest(ctx, base, sub_824BBF98, ctrl, chunk);
  ctx = saved;
  REXLOG_INFO("Offline challenges: daily set {:08X} ({} challenges), weekly set {:08X} ({})",
              period.daily_id, daily, period.weekly_id, weekly);

  // Saved progress for these sets goes back to the game as a `chpr` chunk, which it
  // applies with max(local, saved); completed ones are restored without a second award.
  SavedProgress saved_progress;
  if (LoadSaved(saved_progress) && saved_progress.daily_id == period.daily_id &&
      saved_progress.weekly_id == period.weekly_id) {
    uint8_t* chpr = base + kSavedProgress + ctrl * 100;
    std::memset(chpr, 0, 100);
    Store32(chpr, 0x63687072);  // 'chpr'
    Store32(chpr + 4, 100);
    Store16(chpr + 8, 2);
    Store16(chpr + 10, 1);
    Store32(chpr + 0x0C, period.daily_id);
    Store32(chpr + 0x10, period.weekly_id);
    for (int i = 0; i < kMaxEntries; ++i) {
      Store32(chpr + 0x14 + 4 * i, uint32_t(saved_progress.daily[i]));
      Store32(chpr + 0x3C + 4 * i, uint32_t(saved_progress.weekly[i]));
    }
    base[kSavedPending] |= uint8_t(1u << ctrl);
    REXLOG_INFO("Offline challenges: restoring saved progress");
  }
}

}  // namespace

void ReachChallengeSigninFilter(PPCRegister& r5) {
  if (Enabled() && (r5.u32 & 2)) r5.u64 |= 1;
}

void ReachChallengeSyncGate(PPCRegister& r6) {
  if (Enabled()) r6.u64 = 1;
}

void ReachChallengeUiSynced(PPCRegister& r3) {
  if (Enabled()) r3.u64 = 1;
}

void ReachChallengeTick(PPCRegister& r3) {
  if (!Enabled()) return;
  PPCContext& ctx = *reinterpret_cast<PPCContext*>(&r3);
  uint8_t* base = Base();
  const uint32_t ctrl = r3.u32;
  if (ctrl > 3 || !base[kEnabled + ctrl]) return;

  const Period period = CurrentPeriod();
  const uint32_t daily_set = kDailySet, weekly_set = kDailySet + kSetStride;
  if (!base[kSetsLoaded] || Load32(base + daily_set) != period.daily_id ||
      Load32(base + weekly_set) != period.weekly_id) {
    // At most one attempt every 5 s if the game does not take the set.
    static auto last_attempt = std::chrono::steady_clock::time_point();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_attempt < std::chrono::seconds(5)) return;
    last_attempt = now;
    ApplyPeriod(ctx, base, ctrl, period);
    if (Load32(base + daily_set) != period.daily_id) return;  // not applied (yet)
  }

  // Keep "progress loaded" set (signing in clears it) and let matchmaking-only
  // challenges count everywhere (definitions are re-copied, so every tick).
  base[kChallengeState] |= uint8_t(1u << ctrl);
  for (uint32_t set : {daily_set, weekly_set}) {
    uint32_t count = std::min<uint32_t>(Load32(base + set + 0x10), kMaxEntries);
    for (uint32_t i = 0; i < count; ++i) base[set + 0x14 + i * kEntryStride + 0x2C + 0x31] |= 1;
  }

  // Save progress when it changes; never below what was saved, and not while the saved
  // progress is still waiting to be applied.
  if (base[kSavedPending] & (1u << ctrl)) return;
  static SavedProgress last;
  SavedProgress now;
  now.daily_id = period.daily_id;
  now.weekly_id = period.weekly_id;
  now.daily = ReadProgress(base, daily_set, ctrl);
  now.weekly = ReadProgress(base, weekly_set, ctrl);
  if (std::memcmp(&now, &last, sizeof(now)) == 0) return;
  SavedProgress saved;
  if (LoadSaved(saved) && saved.daily_id == now.daily_id && saved.weekly_id == now.weekly_id) {
    for (int i = 0; i < kMaxEntries; ++i) {
      now.daily[i] = std::max(now.daily[i], saved.daily[i]);
      now.weekly[i] = std::max(now.weekly[i], saved.weekly[i]);
    }
  }
  last = now;
  WriteSaved(now);
}
