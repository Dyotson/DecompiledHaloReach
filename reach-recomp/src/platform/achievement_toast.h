// reach - the SDK's achievement toasts, registered with the ImGui drawer only while one shows.
//
// The ImGui drawer asks for another window repaint after every frame while any dialog is
// registered, and the SDK registers its achievement toast dialog for the whole run. On
// Linux nothing paces those repaints but a FIFO swapchain's vsync wait, so the window was
// repainted at the display's refresh rate (FIFO) or thousands of times a second (MAILBOX,
// IMMEDIATE), each time also rebuilding the presenter's output pipeline. With the toast
// registered only while it is on screen, the window is repainted when the game presents a
// frame (docs/perf.md).

#pragma once

#include <memory>

namespace rex::ui {
class AchievementNotificationDialog;
class ImGuiDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace reach {

// Wraps the SDK's toast dialog (null stays null).
std::unique_ptr<rex::ui::AchievementNotificationDialog> MakeOnDemandToast(
    std::unique_ptr<rex::ui::AchievementNotificationDialog> toast, rex::ui::ImGuiDrawer* drawer,
    rex::ui::WindowedAppContext& app_context);

}  // namespace reach
