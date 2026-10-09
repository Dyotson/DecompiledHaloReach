// reach - keyboard and mouse (src/input/kbm.cpp, docs/input.md).

#pragma once

#include <rex/system/interfaces/input.h>

#include <memory>
#include <string_view>

namespace reach::kbm {

// The SDK's default input system (SDL pads, its own keyboard driver, the NOP stand-in) plus
// Reach's keyboard and mouse device on guest user 0. Installed from ReachApp::OnPreSetup.
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

// Mouse counts moved since the last call, for the player-control hooks
// (src/hooks/mouse_look.cpp), already scaled to radians: yaw left and pitch up are positive.
// Returns false when there is nothing to apply. Calling it also marks the camera as under
// player control, which is when the mouse is captured.
bool TakeMouseLook(float& yaw, float& pitch);

// Scripted input for tests (REACH_AUTOPRESS_FIFO): the same paths as the real devices.
void InjectMouse(float dx, float dy);
bool InjectKey(std::string_view name, double hold_seconds);

}  // namespace reach::kbm
