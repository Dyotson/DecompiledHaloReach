// reach - keyboard and mouse.
//
// A synthetic device on guest user 0, merged with any pad there. Keys and mouse buttons press
// pad controls, bound per control by the kbm_bind_* cvars (defaults follow Reach's "Default"
// button layout, docs/input.md). Mouse motion does not go through the right stick: it is
// handed to the player-control hooks (src/hooks/mouse_look.cpp), which add it to the camera's
// yaw/pitch change of the tick, after the stick's acceleration and turn-rate limits.
//
// The mouse is captured (hidden, relative mode) only while the window has focus, no SDK
// overlay wants it and the camera is under player control, so it is free in menus and when
// the game is paused.

#include "kbm.h"

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(kbm, true, "Input/Keyboard and Mouse",
                    "Play with keyboard and mouse (guest user 0, together with any pad)");
REXCVAR_DEFINE_DOUBLE(kbm_sensitivity, 2.5, "Input/Keyboard and Mouse",
                      "Mouse look speed: 0.022 degrees per count times this (zoom divides it "
                      "by the magnification, like the stick)")
    .range(0.05, 30.0);
REXCVAR_DEFINE_DOUBLE(kbm_vertical_ratio, 1.0, "Input/Keyboard and Mouse",
                      "Vertical mouse look speed relative to horizontal")
    .range(0.1, 4.0);
REXCVAR_DEFINE_BOOL(kbm_invert_mouse, false, "Input/Keyboard and Mouse",
                    "Invert vertical mouse look, on top of the game's Look Inversion setting");

#define REACH_KBM_BIND(control, keys, what)                                             \
  REXCVAR_DEFINE_STRING(kbm_bind_##control, keys, "Input/Keyboard and Mouse/Bindings", \
                        what " (comma-separated keys; LMB RMB MMB Mouse4 Mouse5 WheelUp WheelDown)")
REACH_KBM_BIND(a, "Space,Return", "A: jump, menu select");
REACH_KBM_BIND(b, "X,Backspace", "B: switch grenades, menu back");
REACH_KBM_BIND(x, "E,R", "X: action/reload (hold E to pick up)");
REACH_KBM_BIND(y, "1,2,WheelUp,WheelDown", "Y: swap weapons");
REACH_KBM_BIND(lb, "Shift", "LB: armor ability");
REACH_KBM_BIND(rb, "Q,Mouse4", "RB: melee");
REACH_KBM_BIND(lt, "G,Mouse5", "LT: throw grenade");
REACH_KBM_BIND(rt, "LMB", "RT: fire");
REACH_KBM_BIND(ls, "Control,C", "Left stick click: crouch");
REACH_KBM_BIND(rs, "RMB,MMB", "Right stick click: zoom");
REACH_KBM_BIND(back, "Tab", "Back: scoreboard");
REACH_KBM_BIND(start, "Escape", "Start: game menu");
REACH_KBM_BIND(dpad_up, "Up,N", "D-pad up (night vision)");
REACH_KBM_BIND(dpad_down, "Down", "D-pad down");
REACH_KBM_BIND(dpad_left, "Left", "D-pad left");
REACH_KBM_BIND(dpad_right, "Right", "D-pad right");
REACH_KBM_BIND(move_forward, "W", "Left stick up: move forward");
REACH_KBM_BIND(move_back, "S", "Left stick down: move back");
REACH_KBM_BIND(move_left, "A", "Left stick left: strafe left");
REACH_KBM_BIND(move_right, "D", "Left stick right: strafe right");
REACH_KBM_BIND(look_up, "", "Right stick up (keyboard look)");
REACH_KBM_BIND(look_down, "", "Right stick down (keyboard look)");
REACH_KBM_BIND(look_left, "", "Right stick left (keyboard look)");
REACH_KBM_BIND(look_right, "", "Right stick right (keyboard look)");
#undef REACH_KBM_BIND

namespace reach::kbm {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::ui::VirtualKey;
using Clock = std::chrono::steady_clock;

constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x52454B4D);  // 'REKM'

// Key slots: Windows virtual-key codes (mouse buttons included), then the wheel.
constexpr int kWheelUp = 256;
constexpr int kWheelDown = 257;
constexpr int kKeySlots = 258;

// Reach's tick is 1/30 s and it polls pads once per tick, so a wheel notch is held this long,
// then released for as long before the next queued notch.
constexpr auto kWheelPulse = std::chrono::milliseconds(80);

// No player-control tick for this long means menus, a pause or a load: release the mouse and
// drop motion gathered meanwhile.
constexpr auto kLookIdle = std::chrono::milliseconds(300);

constexpr double kDegreesPerCount = 0.022;
constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;

int ParseKey(std::string_view name) {
  static const struct {
    const char* name;
    int slot;
  } kExtra[] = {
      {"Mouse1", int(VirtualKey::kLButton)}, {"Mouse2", int(VirtualKey::kRButton)},
      {"Mouse3", int(VirtualKey::kMButton)}, {"Mouse4", int(VirtualKey::kXButton1)},
      {"Mouse5", int(VirtualKey::kXButton2)}, {"WheelUp", kWheelUp},
      {"WheelDown", kWheelDown},              {"Ctrl", int(VirtualKey::kControl)},
  };
  for (const auto& extra : kExtra) {
    if (name == extra.name) return extra.slot;
  }
  const int vk = int(rex::ui::ParseVirtualKey(name));
  return vk > 0 && vk < 256 ? vk : -1;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return s;
}

enum Control {
  kA, kB, kX, kY, kLB, kRB, kLT, kRT, kLS, kRS, kBack, kStart,
  kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
  kMoveForward, kMoveBack, kMoveLeft, kMoveRight,
  kLookUp, kLookDown, kLookLeft, kLookRight,
  kControlCount
};

const std::string& BindOf(int control) {
  switch (control) {
    case kA: return REXCVAR_GET(kbm_bind_a);
    case kB: return REXCVAR_GET(kbm_bind_b);
    case kX: return REXCVAR_GET(kbm_bind_x);
    case kY: return REXCVAR_GET(kbm_bind_y);
    case kLB: return REXCVAR_GET(kbm_bind_lb);
    case kRB: return REXCVAR_GET(kbm_bind_rb);
    case kLT: return REXCVAR_GET(kbm_bind_lt);
    case kRT: return REXCVAR_GET(kbm_bind_rt);
    case kLS: return REXCVAR_GET(kbm_bind_ls);
    case kRS: return REXCVAR_GET(kbm_bind_rs);
    case kBack: return REXCVAR_GET(kbm_bind_back);
    case kStart: return REXCVAR_GET(kbm_bind_start);
    case kDpadUp: return REXCVAR_GET(kbm_bind_dpad_up);
    case kDpadDown: return REXCVAR_GET(kbm_bind_dpad_down);
    case kDpadLeft: return REXCVAR_GET(kbm_bind_dpad_left);
    case kDpadRight: return REXCVAR_GET(kbm_bind_dpad_right);
    case kMoveForward: return REXCVAR_GET(kbm_bind_move_forward);
    case kMoveBack: return REXCVAR_GET(kbm_bind_move_back);
    case kMoveLeft: return REXCVAR_GET(kbm_bind_move_left);
    case kMoveRight: return REXCVAR_GET(kbm_bind_move_right);
    case kLookUp: return REXCVAR_GET(kbm_bind_look_up);
    case kLookDown: return REXCVAR_GET(kbm_bind_look_down);
    case kLookLeft: return REXCVAR_GET(kbm_bind_look_left);
    default: return REXCVAR_GET(kbm_bind_look_right);
  }
}

constexpr uint16_t kButtonOf[kStart + 1 + 4] = {
    rex::input::X_INPUT_GAMEPAD_A,
    rex::input::X_INPUT_GAMEPAD_B,
    rex::input::X_INPUT_GAMEPAD_X,
    rex::input::X_INPUT_GAMEPAD_Y,
    rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER,
    rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER,
    0,  // LT and RT are triggers
    0,
    rex::input::X_INPUT_GAMEPAD_LEFT_THUMB,
    rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB,
    rex::input::X_INPUT_GAMEPAD_BACK,
    rex::input::X_INPUT_GAMEPAD_START,
    rex::input::X_INPUT_GAMEPAD_DPAD_UP,
    rex::input::X_INPUT_GAMEPAD_DPAD_DOWN,
    rex::input::X_INPUT_GAMEPAD_DPAD_LEFT,
    rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT,
};

class Driver final : public rex::input::InputDriver,
                     public rex::ui::WindowInputListener,
                     public rex::ui::WindowListener {
 public:
  Driver() : InputDriver(nullptr, 0) {}
  ~Driver() override { Detach(); }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    if (!REXCVAR_GET(kbm)) return;
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Keyboard and Mouse";
    info.subtype = rex::input::XINPUT_DEVSUBTYPE_GAMEPAD;
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (!REXCVAR_GET(kbm) || id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = rex::input::XINPUT_DEVTYPE_GAMEPAD;
      caps->sub_type = rex::input::XINPUT_DEVSUBTYPE_GAMEPAD;
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = int16_t(0x7FFF);
      caps->gamepad.thumb_ly = int16_t(0x7FFF);
      caps->gamepad.thumb_rx = int16_t(0x7FFF);
      caps->gamepad.thumb_ry = int16_t(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (!REXCVAR_GET(kbm) || id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    const auto now = Clock::now();
    const bool live = has_focus_ && is_active();
    UpdateCapture(live && now - last_look_tick_.load() < kLookIdle);

    std::lock_guard lock(mutex_);
    StepWheel(now);
    uint32_t held = 0;
    for (int control = 0; control < kControlCount; ++control) {
      if (ControlDown(control, live, now)) held |= 1u << control;
    }
    auto down = [&](int control) { return (held >> control) & 1; };
    uint16_t buttons = 0;
    for (int control = 0; control <= kDpadRight; ++control) {
      if (down(control)) buttons |= kButtonOf[control];
    }
    auto axis = [&](int plus, int minus) {
      return int16_t((down(plus) ? 32767 : 0) - (down(minus) ? 32767 : 0));
    };
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->packet_number = ++packet_;
      out->gamepad.buttons = buttons;
      out->gamepad.left_trigger = down(kLT) ? 0xFF : 0;
      out->gamepad.right_trigger = down(kRT) ? 0xFF : 0;
      out->gamepad.thumb_lx = axis(kMoveRight, kMoveLeft);
      out->gamepad.thumb_ly = axis(kMoveForward, kMoveBack);
      out->gamepad.thumb_rx = axis(kLookRight, kLookLeft);
      out->gamepad.thumb_ry = axis(kLookUp, kLookDown);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return REXCVAR_GET(kbm) && id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE*) override {
    // Reach reads pads with XInputGetState only.
    return REXCVAR_GET(kbm) && id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  void OnWindowAvailable(rex::ui::Window* window) override {
    if (!window) return;
    {
      std::lock_guard lock(mutex_);
      window_ = window;
    }
    has_focus_ = window->HasFocus();
    window->AddInputListener(this, 0);
    window->AddListener(this);
  }

  // WindowListener
  void OnClosing(rex::ui::UIEvent&) override { Detach(); }
  void OnGotFocus(rex::ui::UISetupEvent&) override { has_focus_ = true; }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    has_focus_ = false;
    capture_wanted_ = false;
    {
      std::lock_guard lock(mutex_);
      key_down_.fill(false);
      mouse_dx_ = mouse_dy_ = 0;
    }
    if (window_) ReleaseCapture(window_);
  }

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override { SetKey(int(e.virtual_key()), true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { SetKey(int(e.virtual_key()), false); }
  void OnMouseDown(rex::ui::MouseEvent& e) override { SetKey(SlotOf(e.button()), true); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { SetKey(SlotOf(e.button()), false); }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!REXCVAR_GET(kbm) || !has_focus_ || e.scroll_y() == 0) return;
    std::lock_guard lock(mutex_);
    int& queued = wheel_queued_[e.scroll_y() > 0 ? 0 : 1];
    queued = std::min(queued + 1, 2);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!REXCVAR_GET(kbm) || !has_focus_) return;
    const int32_t x = e.x(), y = e.y();
    if (captured_) {
      std::lock_guard lock(mutex_);
      if (relative_) {
        mouse_dx_ += e.dx();
        mouse_dy_ += e.dy();
      } else {
        mouse_dx_ += float(x - prev_x_);
        mouse_dy_ += float(y - prev_y_);
      }
    }
    prev_x_ = x;
    prev_y_ = y;
    if (captured_ && !relative_) Recenter(x, y);
  }

  bool TakeLook(float& dx, float& dy) {
    const auto now = Clock::now();
    const auto last = last_look_tick_.exchange(now);
    std::lock_guard lock(mutex_);
    dx = mouse_dx_ + injected_dx_;
    dy = mouse_dy_ + injected_dy_;
    mouse_dx_ = mouse_dy_ = injected_dx_ = injected_dy_ = 0;
    // Motion from menus or a pause must not snap the camera on the way back in.
    if (now - last >= kLookIdle) dx = dy = 0;
    return dx != 0 || dy != 0;
  }

  void Inject(float dx, float dy) {
    std::lock_guard lock(mutex_);
    injected_dx_ += dx;
    injected_dy_ += dy;
  }

  bool InjectKeyFor(std::string_view name, double seconds) {
    const int slot = ParseKey(name);
    if (slot < 0) return false;
    std::lock_guard lock(mutex_);
    if (slot == kWheelUp || slot == kWheelDown) {
      int& queued = wheel_queued_[slot == kWheelUp ? 0 : 1];
      queued = std::min(queued + 1, 2);
    } else {
      injected_until_[slot] =
          Clock::now() + std::chrono::duration_cast<Clock::duration>(
                             std::chrono::duration<double>(seconds));
    }
    return true;
  }

 private:
  static int SlotOf(rex::ui::MouseEvent::Button button) {
    switch (button) {
      case rex::ui::MouseEvent::Button::kLeft: return int(VirtualKey::kLButton);
      case rex::ui::MouseEvent::Button::kRight: return int(VirtualKey::kRButton);
      case rex::ui::MouseEvent::Button::kMiddle: return int(VirtualKey::kMButton);
      case rex::ui::MouseEvent::Button::kX1: return int(VirtualKey::kXButton1);
      case rex::ui::MouseEvent::Button::kX2: return int(VirtualKey::kXButton2);
      default: return -1;
    }
  }

  void SetKey(int slot, bool down) {
    if (slot <= 0 || slot >= 256 || !REXCVAR_GET(kbm)) return;
    if (down && !has_focus_) return;
    std::lock_guard lock(mutex_);
    key_down_[slot] = down;
  }

  bool SlotDown(int slot, bool live, Clock::time_point now) const {
    if (slot == kWheelUp || slot == kWheelDown) {
      return wheel_held_[slot == kWheelUp ? 0 : 1];
    }
    return (live && key_down_[slot]) || now < injected_until_[slot];
  }

  // Bindings are re-read every poll: the cvars can change at run time (settings overlay).
  bool ControlDown(int control, bool live, Clock::time_point now) const {
    std::string_view rest = BindOf(control);
    while (!rest.empty()) {
      const size_t comma = rest.find(',');
      std::string_view token = Trim(rest.substr(0, comma));
      rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
      const int slot = token.empty() ? -1 : ParseKey(token);
      if (slot >= 0 && SlotDown(slot, live, now)) return true;
    }
    return false;
  }

  void StepWheel(Clock::time_point now) {
    for (int i = 0; i < 2; ++i) {
      if (now < wheel_until_[i]) continue;
      if (wheel_held_[i]) {
        wheel_held_[i] = false;
        wheel_until_[i] = now + kWheelPulse;
      } else if (wheel_queued_[i] > 0) {
        --wheel_queued_[i];
        wheel_held_[i] = true;
        wheel_until_[i] = now + kWheelPulse;
      }
    }
  }

  void UpdateCapture(bool want) {
    capture_wanted_ = want && REXCVAR_GET(kbm);
    if (capture_wanted_ == captured_ || capture_update_queued_.exchange(true)) return;
    std::lock_guard lock(mutex_);
    if (!window_) {
      capture_update_queued_ = false;
      return;
    }
    window_->app_context().CallInUIThreadDeferred([this] {
      capture_update_queued_ = false;
      rex::ui::Window* window = window_;
      if (!window) return;
      if (!capture_wanted_) {
        ReleaseCapture(window);
      } else if (!captured_) {
        captured_ = true;
        cursor_before_ = window->GetCursorVisibility();
        window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
        window->CaptureMouse();
        relative_ = window->SetRelativeMouseMode(true);
        std::lock_guard lock(mutex_);
        mouse_dx_ = mouse_dy_ = 0;
      }
    });
  }

  void ReleaseCapture(rex::ui::Window* window) {
    if (!captured_) return;
    captured_ = false;
    window->SetRelativeMouseMode(false);
    relative_ = false;
    window->SetCursorVisibility(cursor_before_);
    window->ReleaseMouse();
  }

  void Recenter(int32_t x, int32_t y) {
    rex::ui::Window* window = window_;
    if (!window) return;
    const int32_t w = int32_t(window->GetActualPhysicalWidth());
    const int32_t h = int32_t(window->GetActualPhysicalHeight());
    if (w <= 0 || h <= 0) return;
    if (std::abs(x - w / 2) < w / 4 && std::abs(y - h / 2) < h / 4) return;
    int32_t cx = 0, cy = 0;
    if (window->WarpMouseToCenter(cx, cy)) {
      prev_x_ = cx;
      prev_y_ = cy;
    }
  }

  void Detach() {
    rex::ui::Window* window = window_;
    if (!window) return;
    window->app_context().CallInUIThreadSynchronous([this, window] {
      {
        std::lock_guard lock(mutex_);
        window_ = nullptr;
      }
      if (capture_update_queued_) window->app_context().ExecutePendingFunctionsFromUIThread();
      ReleaseCapture(window);
      window->RemoveInputListener(this);
      window->RemoveListener(this);
    });
  }

  mutable std::mutex mutex_;
  rex::ui::Window* window_ = nullptr;
  std::array<bool, 256> key_down_{};
  std::array<Clock::time_point, 256> injected_until_{};
  int wheel_queued_[2] = {};
  bool wheel_held_[2] = {};
  Clock::time_point wheel_until_[2] = {};
  float mouse_dx_ = 0, mouse_dy_ = 0;
  float injected_dx_ = 0, injected_dy_ = 0;
  uint32_t packet_ = 0;

  std::atomic<bool> has_focus_{false};
  std::atomic<Clock::time_point> last_look_tick_{};
  std::atomic<bool> capture_wanted_{false};
  std::atomic<bool> capture_update_queued_{false};
  // UI thread only.
  bool captured_ = false;
  bool relative_ = false;
  rex::ui::Window::CursorVisibility cursor_before_ = rex::ui::Window::CursorVisibility::kVisible;
  int32_t prev_x_ = 0, prev_y_ = 0;
};

Driver* g_driver = nullptr;

}  // namespace

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  auto input = rex::input::CreateDefaultInputSystem(tool_mode);
  if (!tool_mode) {
    auto driver = std::make_unique<Driver>();
    g_driver = driver.get();
    input->AddDriver(std::move(driver));
  }
  return input;
}

bool TakeMouseLook(float& yaw, float& pitch) {
  float dx = 0, dy = 0;
  if (!g_driver || !REXCVAR_GET(kbm) || !g_driver->TakeLook(dx, dy)) return false;
  const double scale = kDegreesPerCount * REXCVAR_GET(kbm_sensitivity) * kRadiansPerDegree;
  yaw = float(-dx * scale);
  pitch = float(-dy * scale * REXCVAR_GET(kbm_vertical_ratio));
  if (REXCVAR_GET(kbm_invert_mouse)) pitch = -pitch;
  return true;
}

void InjectMouse(float dx, float dy) {
  if (g_driver) g_driver->Inject(dx, dy);
}

bool InjectKey(std::string_view name, double hold_seconds) {
  return g_driver && g_driver->InjectKeyFor(name, hold_seconds);
}

}  // namespace reach::kbm
