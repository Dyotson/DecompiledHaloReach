// reach - achievement toasts registered only while shown; see achievement_toast.h.

#include "achievement_toast.h"

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

namespace reach {
namespace {

using Clock = std::chrono::steady_clock;

// The SDK's toast shows each achievement for 4.5 s, one after another
// (AchievementToastDialog::kDisplaySeconds); the margin covers the last frames.
constexpr auto kToastTime = std::chrono::milliseconds(4500);
constexpr auto kMargin = std::chrono::milliseconds(1000);

class OnDemandToast final : public rex::ui::AchievementNotificationDialog {
 public:
  OnDemandToast(std::unique_ptr<rex::ui::AchievementNotificationDialog> toast,
                rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& app_context)
      : AchievementNotificationDialog(drawer),
        drawer_(drawer),
        app_context_(app_context),
        toast_(std::move(toast)),
        self_(std::make_shared<std::atomic<OnDemandToast*>>(this)) {
    // Both constructors registered a dialog. The SDK's toast is drawn through this one, and
    // this one is registered while a toast shows.
    drawer_->RemoveDialog(toast_.get());
    drawer_->RemoveDialog(this);
  }

  ~OnDemandToast() override { self_->store(nullptr); }

  // Any thread.
  void Push(const rex::system::AchievementEvent& event) override {
    toast_->Push(event);
    {
      std::lock_guard lock(mutex_);
      shown_until_ = std::max(shown_until_, Clock::now()) + kToastTime;
    }
    app_context_.CallInUIThread([self = self_] {
      if (OnDemandToast* toast = self->load()) toast->drawer_->AddDialog(toast);
    });
  }

 protected:
  void OnDraw(ImGuiIO&) override {
    toast_->Draw();
    bool done;
    {
      std::lock_guard lock(mutex_);
      done = Clock::now() >= shown_until_ + kMargin;
    }
    // The drawer detaches from the presenter once its last dialog is gone.
    if (done) drawer_->RemoveDialog(this);
  }

 private:
  rex::ui::ImGuiDrawer* drawer_;
  rex::ui::WindowedAppContext& app_context_;
  std::unique_ptr<rex::ui::AchievementNotificationDialog> toast_;
  // Lets a queued UI-thread call see that this dialog is gone.
  std::shared_ptr<std::atomic<OnDemandToast*>> self_;
  std::mutex mutex_;
  Clock::time_point shown_until_{};
};

}  // namespace

std::unique_ptr<rex::ui::AchievementNotificationDialog> MakeOnDemandToast(
    std::unique_ptr<rex::ui::AchievementNotificationDialog> toast, rex::ui::ImGuiDrawer* drawer,
    rex::ui::WindowedAppContext& app_context) {
  if (!toast || !drawer) return toast;
  return std::make_unique<OnDemandToast>(std::move(toast), drawer, app_context);
}

}  // namespace reach
