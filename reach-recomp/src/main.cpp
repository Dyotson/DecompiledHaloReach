// reach - ReXGlue Recompiled Project

#include "generated/default/reach_init.h"

#include "reach_app.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <string_view>

REX_DEFINE_APP(reach, ReachApp::Create)

REXCVAR_DEFINE_STRING(gpu_backend, "vulkan", "GPU",
                      "GPU backend on Windows: vulkan (with this project's fixes) or d3d12")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

std::string ReachGpuBackend() { return REXCVAR_GET(gpu_backend); }

namespace {

// Gives an SDK cvar this game's default. A value from reach.toml, the environment
// (REX_<NAME>) or the command line stays; the settings UI and SaveConfig treat the new
// value as the default.
void SetSdkCvarDefault(std::string_view name, std::string_view value) {
  for (rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
    if (entry.name != name) continue;
    if (entry.source == rex::cvar::Source::kDefault) entry.setter(value);
    entry.default_value = value;
    return;
  }
  REXLOG_WARN("SDK setting {} not found", name);
}

}  // namespace

void ApplyReachCvarDefaults() {
  // Present with FIFO (vsync) only. The SDK prefers IMMEDIATE, which lets a fullscreen game
  // tear (KDE and other Wayland compositors allow it): on the slowly panning menus the tear
  // line reads as a shimmer crawling down the screen. FIFO_RELAXED tears at 30 fps too,
  // since every frame arrives after the vblank it was due for. MAILBOX does not tear, but
  // nothing paces the SDK's UI-thread repaints on Linux except a vsync wait: while an ImGui
  // dialog was always registered, that was thousands of presents a second
  // (src/platform/achievement_toast.h). FIFO keeps a cap on it, at up to one refresh of
  // extra latency. reach.toml can allow the others again; docs/perf.md has the measurements.
  SetSdkCvarDefault("vulkan_allow_present_mode_immediate", "false");
  SetSdkCvarDefault("vulkan_allow_present_mode_mailbox", "false");
  SetSdkCvarDefault("vulkan_allow_present_mode_fifo_relaxed", "false");
}
