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

#include "input/kbm.h"

#include <filesystem>
#include <string>

// GPU backend on Windows (the gpu_backend setting, src/main.cpp).
std::string ReachGpuBackend();

class ReachApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ReachApp>(new ReachApp(ctx, "reach",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!config.graphics && config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // Keyboard and mouse on guest user 0, next to the SDK's pads (docs/input.md).
    config.input_factory = reach::kbm::CreateInputSystem;
#ifdef _WIN32
    // The plugin's "any" backend picks D3D12 first on Windows; this project's GPU fixes
    // (patches/rexglue-sdk) are in the Vulkan backend.
    if (!config.graphics && !config.gpu_plugin.empty()) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, ReachGpuBackend());
    }
#endif
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
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
