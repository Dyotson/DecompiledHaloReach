// reach - stand-in for <wayland-client.h> in src/platform/vulkan_hooks.cpp only.
//
// The SDK's Vulkan header (rex/ui/vulkan/api.h) includes the window-system headers, but the
// Vulkan declarations only use pointers to these opaque types and vulkan_hooks.cpp never
// calls Wayland, so the game builds without the Wayland development package.
#pragma once

struct wl_display;
struct wl_surface;
