// reach - stand-in for <xcb/xcb.h> in src/platform/vulkan_hooks.cpp only (see
// ../wayland-client.h). The types match libxcb's.
#pragma once

#include <stdint.h>

typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_window_t;
typedef uint32_t xcb_visualid_t;
