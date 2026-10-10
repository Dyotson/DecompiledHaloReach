// reach - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

#include "debug/frame_stats.h"
#include "input/kbm.h"
#include "platform/vulkan_hooks.h"

#include <filesystem>
#include <string>

// GPU backend on Windows (the gpu_backend setting, src/main.cpp).
std::string ReachGpuBackend();

// This game's defaults for SDK settings (src/main.cpp); reach.toml still overrides them.
void ApplyReachCvarDefaults();

class ReachApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ReachApp>(new ReachApp(ctx, "reach",
        PPCImageConfig));
  }

  // Runs after reach.toml and the command line are applied, before the window opens.
  void OnPostInitLogging() override { ApplyReachCvarDefaults(); }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!config.graphics && config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // Keyboard and mouse on guest user 0, next to the SDK's pads (docs/input.md).
    config.input_factory = reach::kbm::CreateInputSystem;
    // Loaded here rather than by the SDK so OnCreateDialogs can reach the GPU provider.
    // The plugin's "any" backend picks D3D12 first on Windows; this project's GPU fixes
    // (patches/rexglue-sdk) are in the Vulkan backend.
    if (!config.graphics && !config.gpu_plugin.empty()) {
#ifdef _WIN32
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, ReachGpuBackend());
#else
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
#endif
    }
    graphics_ = config.graphics.get();
  }

  // The GPU provider exists now and the window has no swapchain yet: hook the Vulkan
  // calls (swapchain present-mode log, RECOMP_PERF timing; src/platform/vulkan_hooks.h).
  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override { reach::InstallVulkanHooks(graphics_); }

  void OnPostSetup() override { reach::InstallVulkanHooks(runtime()->graphics_system()); }

  // The SDK hard-exits right after a close request is accepted (SIGINT included), so the
  // frame statistics summary is written here.
  bool OnWindowCloseRequested() override {
    reach::perf::Shutdown();
    return true;
  }

  // Reach keeps preferences and streamed map/tag caches on the console's
  // utility partitions; the game links cache0:/cache1: to these devices.
  void OnPreLaunchModule() override {
    auto* vfs = runtime()->file_system();
    // At boot the game opens cache1:\webcache, marks it delete-on-close and
    // recreates it. The runtime does not delete directories on close, so the
    // recreate fails with STATUS_OBJECT_NAME_COLLISION and the web cache
    // (0x82ABE090) never initializes. Clear it here; the game rebuilds it.
    {
      std::error_code ec;
      std::filesystem::remove_all(cache_root() / "cache1" / "webcache", ec);
    }
    for (const char* name : {"cache0", "cache1"}) {
      std::filesystem::path host = cache_root() / name;
      std::error_code ec;
      std::filesystem::create_directories(host, ec);
      auto device = std::make_unique<rex::filesystem::HostPathDevice>(
          std::string("\\Device\\") + name, host, false, true);
      if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
        REXLOG_ERROR("Failed to mount \\Device\\{} at {}", name, host.string());
      }
    }
  }

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnConfigurePaths(rex::PathConfig& paths) override {}

 private:
  rex::system::IGraphicsSystem* graphics_ = nullptr;  // owned by the SDK
};
