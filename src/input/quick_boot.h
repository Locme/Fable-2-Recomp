// fable_2 - Quick boot: press A at the title screen, then pick Continue.
//
// Off by default. With [patches] quick_boot = true in fable2_config.toml the
// game boots straight into the last save: a synthetic gamepad presses A on the
// "Press A to start" screen and then selects Continue on the main menu, the
// same inputs a player would give.
//
// The presses are gated on the game's own front-end state, not on a timer:
//
//  - The front-end controller object (vtable handlers around 0x826C6xxx) keeps
//    a state machine at +24 and a "can press A" byte at +72. When the GUI
//    sends the CAN_PRESS_A message (name constant @ 0x820CD0C0, its hash is
//    stored at 0x8349AFA8 by a static initializer), handler sub_826C60C0 sets
//    +72 = 1. The mid-asm hook fable2_hook_quick_boot_can_press_a (after the
//    `stb r11, 72(r31)` at 0x826C60D8) hands us the controller pointer.
//  - The controller's input handler sub_826C61B8 accepts the title press only
//    while +72 is set: it clears +72 and moves the state 1 -> 10 (waiting for
//    the profile) -> 11 (loading it) -> 0 (front end idle, main menu up).
//    State 8 is the attract movie (started after ~40 s idle in state 0), 9 is
//    "front end not ready yet", and +57 is set once a game starts loading
//    (sub_826C67F8).
//
// So the sequence is:
//   1. Title: while +72 == 1, pulse A (once a second) until the game clears it.
//   2. Menu: wait until the controller is back in state 0, not loading, and has
//      stayed there for quick_boot_menu_delay_ms (the menu fades in; the GUI
//      menu itself has no state we can read yet, so this one wait is timed).
//   3. Press Up 4 times (the menu clamps at the top, so this lands on New Game
//      wherever the cursor started), Down once (Continue is the second item:
//      New Game / Continue / Downloadable Content / Language / Subtitles, see
//      plans/main-menu-mod-item.md), then A.
// Every press re-checks the controller first; if the attract movie starts or a
// load begins early, quick boot stops and leaves the game to the player. It
// runs once per launch and gives up after a fixed time.
//
// The synthetic pad is OR-merged with the keyboard and real pads (synthetic
// devices go to guest user 0), so the player can still press buttons.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fable2::quickboot {

// X_INPUT_GAMEPAD_* bits.
constexpr uint16_t kButtonUp = 0x0001;
constexpr uint16_t kButtonDown = 0x0002;
constexpr uint16_t kButtonA = 0x1000;

// Front-end controller fields (see the header comment).
constexpr uint32_t kOffState = 24;      // u32 state machine
constexpr uint32_t kOffLoading = 57;    // u8, set when a game starts loading
constexpr uint32_t kOffCanPressA = 72;  // u8, CAN_PRESS_A received
constexpr int32_t kStateIdle = 0;
constexpr int32_t kStateMovie = 8;
constexpr int32_t kStateNotReady = 9;

// Guest arena base (fixed; see src/diagnostics/alloc_watch.h).
constexpr uintptr_t kGuestBase = 0x100000000ull;

inline std::atomic<bool>& enabled() {
  static std::atomic<bool> e{false};
  return e;
}
// Front-end controller guest address, 0 until CAN_PRESS_A is first seen.
inline std::atomic<uint32_t>& controller() {
  static std::atomic<uint32_t> c{0};
  return c;
}
// Buttons the synthetic pad reports right now.
inline std::atomic<uint16_t>& buttons() {
  static std::atomic<uint16_t> b{0};
  return b;
}

// Called from the mid-asm hook (guest thread) each time the controller sets
// its can-press-A flag.
inline void OnCanPressA(uint32_t controller_addr) {
  if (!enabled().load(std::memory_order_relaxed)) return;
  uint32_t expected = 0;
  if (controller().compare_exchange_strong(expected, controller_addr)) {
    REXSYS_INFO("[quick-boot] title is ready for A (front-end controller 0x{:08X})",
                controller_addr);
  }
}

inline bool Readable(const void* p) {
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION m;
  if (::VirtualQuery(p, &m, sizeof m) == 0) return false;
  return m.State == MEM_COMMIT &&
         (m.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY |
                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
#else
  (void)p;
  return true;
#endif
}

struct ControllerView {
  bool ok = false;
  int32_t state = -1;
  bool loading = false;
  bool can_press_a = false;
};

inline ControllerView ReadController() {
  ControllerView v;
  const uint32_t c = controller().load(std::memory_order_relaxed);
  if (c == 0) return v;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(kGuestBase + c);
  if (!Readable(p) || !Readable(p + kOffCanPressA)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      REXSYS_WARN("[quick-boot] cannot read the front-end controller at 0x{:08X}", c);
    return v;
  }
  const volatile uint8_t* vp = p;
  v.state = static_cast<int32_t>((uint32_t(vp[kOffState]) << 24) |
                                 (uint32_t(vp[kOffState + 1]) << 16) |
                                 (uint32_t(vp[kOffState + 2]) << 8) |
                                 uint32_t(vp[kOffState + 3]));
  v.loading = vp[kOffLoading] != 0;
  v.can_press_a = vp[kOffCanPressA] != 0;
  v.ok = true;
  return v;
}

inline void SleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Hold `mask` long enough for the game to see it (it polls the pad once per
// frame, 30-60 Hz), then release and leave a gap before the next press.
inline void Press(uint16_t mask, int hold_ms = 120, int gap_ms = 300) {
  buttons().store(mask, std::memory_order_relaxed);
  SleepMs(hold_ms);
  buttons().store(0, std::memory_order_relaxed);
  SleepMs(gap_ms);
}

inline const char* StateName(int32_t s) {
  switch (s) {
    case 0: return "idle";
    case 1: return "accepted";
    case 8: return "attract movie";
    case 9: return "not ready";
    case 10: return "waiting for profile";
    case 11: return "loading profile";
    case 12: return "12";
    default: return "?";
  }
}

// The sequencer (background thread). Returns when done, stopped or timed out.
inline void Run(int menu_delay_ms) {
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  auto elapsed_ms = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start)
        .count();
  };
  constexpr int64_t kTitleTimeoutMs = 300000;  // 5 min to reach the title
  constexpr int64_t kStageTimeoutMs = 60000;   // 1 min per later stage

  // 1. Title: wait for CAN_PRESS_A, then pulse A until the game takes it.
  while (controller().load() == 0) {
    if (elapsed_ms() > kTitleTimeoutMs) {
      REXSYS_WARN("[quick-boot] stopped: the title never became ready for A");
      return;
    }
    SleepMs(50);
  }
  auto stage_start = clock::now();
  auto stage_ms = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() -
                                                                 stage_start)
        .count();
  };
  // The press counts as taken once the game clears the flag, starts loading,
  // or moves the controller out of the title states (0 idle, 8 movie,
  // 9 not ready, 12). A tester's log showed the flag can stay set after the
  // game moved on, so the flag alone is not enough to stop pressing.
  auto title_state = [](int32_t s) {
    return s == kStateIdle || s == kStateMovie || s == kStateNotReady || s == 12;
  };
  constexpr int kMaxTitlePresses = 10;
  int a_presses = 0;
  int32_t title_last_state = -2;
  for (;;) {
    const ControllerView v = ReadController();
    if (v.ok && v.state != title_last_state) {
      REXSYS_INFO("[quick-boot] title: front-end state {} ({}), can_press_a={}, loading={}",
                  v.state, StateName(v.state), v.can_press_a, v.loading);
      title_last_state = v.state;
    }
    if (v.ok && v.loading) {
      REXSYS_INFO("[quick-boot] done: a game started loading after {} A press(es)",
                  a_presses);
      return;
    }
    if (v.ok && a_presses > 0 && (!v.can_press_a || !title_state(v.state))) break;
    if (a_presses >= kMaxTitlePresses || stage_ms() > kStageTimeoutMs) {
      REXSYS_WARN(
          "[quick-boot] stopped: the title did not take A after {} press(es) "
          "(state={}, can_press_a={})",
          a_presses, v.state, v.can_press_a);
      return;
    }
    if (v.ok && v.can_press_a && title_state(v.state) && v.state != kStateNotReady) {
      ++a_presses;
      REXSYS_INFO("[quick-boot] pressing A on the title (press {}, state={})", a_presses,
                  v.state);
      Press(kButtonA, 120, 880);
    } else {
      SleepMs(50);
    }
  }
  REXSYS_INFO("[quick-boot] title took A after {} press(es); waiting for the main menu",
              a_presses);

  // 2. Menu: the controller must be idle (state 0), not loading, for
  // menu_delay_ms in a row.
  stage_start = clock::now();
  int32_t last_state = -2;
  auto idle_since = clock::time_point{};
  bool idle = false;
  for (;;) {
    const ControllerView v = ReadController();
    if (v.ok && v.state != last_state) {
      REXSYS_INFO("[quick-boot] front-end state {} ({})", v.state, StateName(v.state));
      last_state = v.state;
    }
    if (v.ok && v.loading) {
      REXSYS_INFO("[quick-boot] stopped: a game is already loading");
      return;
    }
    const bool now_idle = v.ok && v.state == kStateIdle && !v.can_press_a;
    if (now_idle && !idle) idle_since = clock::now();
    idle = now_idle;
    if (idle && std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() -
                                                                      idle_since)
                        .count() >= menu_delay_ms)
      break;
    if (stage_ms() > kStageTimeoutMs) {
      REXSYS_WARN("[quick-boot] stopped: the main menu never settled (state={})",
                  v.state);
      return;
    }
    SleepMs(50);
  }

  // 3. Up x4 (clamps on New Game), Down (Continue), A. Re-check before each.
  struct Step {
    uint16_t mask;
    const char* name;
  };
  const Step steps[] = {{kButtonUp, "Up"},     {kButtonUp, "Up"},
                        {kButtonUp, "Up"},     {kButtonUp, "Up"},
                        {kButtonDown, "Down"}, {kButtonA, "A"}};
  for (const Step& s : steps) {
    const ControllerView v = ReadController();
    if (!v.ok || v.state != kStateIdle || v.loading) {
      REXSYS_WARN(
          "[quick-boot] stopped before {}: the menu is no longer idle (state={} {}, "
          "loading={})",
          s.name, v.state, StateName(v.state), v.loading);
      return;
    }
    if (s.mask == kButtonA) SleepMs(300);  // let the Down settle
    Press(s.mask);
  }
  REXSYS_INFO("[quick-boot] pressed Up x4, Down, A on the main menu (Continue)");

  // 4. Report what the game did with it.
  stage_start = clock::now();
  while (stage_ms() < 15000) {
    const ControllerView v = ReadController();
    if (v.ok && (v.loading || v.state != kStateIdle)) {
      REXSYS_INFO("[quick-boot] front end moved on after Continue: state={} ({}), "
                  "loading={}",
                  v.state, StateName(v.state), v.loading);
      return;
    }
    SleepMs(100);
  }
  REXSYS_WARN("[quick-boot] the front end still looks idle 15 s after Continue");
}

inline void Start(int menu_delay_ms) {
  static std::atomic<bool> started{false};
  bool expected = false;
  if (!started.compare_exchange_strong(expected, true)) return;
  enabled().store(true);
  REXSYS_INFO("[quick-boot] on: will press A at the title, then Continue "
              "(menu delay {} ms)",
              menu_delay_ms);
  std::thread([menu_delay_ms] {
    Run(menu_delay_ms);
    buttons().store(0);
    enabled().store(false);
  }).detach();
}

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;
using rex::input::XINPUT_DEVTYPE_GAMEPAD;

// Synthetic pad that reports buttons(). Not gated on window focus, so quick
// boot also works while the window is in the background.
class GamepadDriver final : public rex::input::InputDriver {
 public:
  GamepadDriver(rex::ui::Window* window, size_t window_z_order)
      : rex::input::InputDriver(window, window_z_order) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kDeviceId;
    info.name = "Quick boot gamepad";
    info.guid = "fable2-quick-boot-gamepad";
    info.synthetic = true;  // routed to guest user 0 by the default assignment
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    if (id != kDeviceId) return X_ERROR_DEVICE_NOT_CONNECTED;
    const uint16_t b = buttons().load(std::memory_order_relaxed);
    if (b != prev_) {  // packet_number advances only on a change
      ++packet_number_;
      prev_ = b;
    }
    out_state->packet_number.set(packet_number_);
    out_state->gamepad.buttons.set(b);
    out_state->gamepad.left_trigger = 0;
    out_state->gamepad.right_trigger = 0;
    out_state->gamepad.thumb_lx.set(0);
    out_state->gamepad.thumb_ly.set(0);
    out_state->gamepad.thumb_rx.set(0);
    out_state->gamepad.thumb_ry.set(0);
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != kDeviceId) return X_ERROR_DEVICE_NOT_CONNECTED;
    *out_caps = X_INPUT_CAPABILITIES{};
    out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
    out_caps->sub_type = 0x05;  // XINPUT_SUBTYPE_GAMEPAD
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return id == kDeviceId ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    (void)out_keystroke;
    return X_ERROR_EMPTY;
  }

 private:
  // Must not collide with the SDL driver's sequential ids, the keyboard
  // driver's 1<<60 or the remote pad's 1<<61.
  static constexpr DeviceId kDeviceId{1ull << 59};

  uint32_t packet_number_ = 0;
  uint16_t prev_ = 0;
};

}  // namespace fable2::quickboot
