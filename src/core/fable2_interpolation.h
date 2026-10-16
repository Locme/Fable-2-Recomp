// fable2_interpolation - which smoothing parts are active ([patches]
// interpolation, plus the test presets cycled with F6).
//
// The parts:
//   kGui         - GUI (subtitles, HUD text) updates on every HF tick
//                  (fable2_hook_gui_every_tick);
//   kCloth       - cloth colliders blended between the last two poses
//                  (fable2_hook_cloth_collider_bone);
//   kRenderDelay - the render delay uses the smoothed tick rate
//                  (fable2_hook_render_time_lf);
//   kNoPoseBlend - the game's OWN character pose blending is switched off
//                  (fable2_hook_pose_blend_alpha forces the blend to the
//                  newest pose). Only in a test preset, never by default.
// The hooks live in fable2_hooks.cpp. F6 cycles the presets (Presets()); the
// log shows each change as "[interpolation] preset N/M: ...", and the window
// title shows the active preset (src/input/keyboard_gamepad.h).

#pragma once

#include <cstdint>

namespace fable2::interp {

inline constexpr uint32_t kGui = 1u << 0;
inline constexpr uint32_t kCloth = 1u << 1;
inline constexpr uint32_t kRenderDelay = 1u << 2;
inline constexpr uint32_t kNoPoseBlend = 1u << 3;

struct Preset {
  const char* name;
  uint32_t parts;
};

// The presets F6 cycles through, in order.
inline constexpr Preset kPresets[] = {
    {"Off (original blending only)", 0},
    {"Text only", kGui},
    {"Cloth only", kCloth},
    {"Smooth render delay only", kRenderDelay},
    {"Full (default)", kGui | kCloth | kRenderDelay},
    {"Full, character blending off", kGui | kCloth | kRenderDelay | kNoPoseBlend},
    {"Nothing blended (character blending off)", kNoPoseBlend},
};
inline constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
inline constexpr int kFullPreset = 4;

// Active parts (any thread, cheap).
uint32_t Parts();
inline bool Has(uint32_t part) { return (Parts() & part) != 0; }

// Active preset index into kPresets.
int PresetIndex();

// Host input poll, once per frame while the game window is focused, with the
// F6 key state: a press selects the next preset. Returns true on a switch.
bool PollHotkey(bool f6_down);

}  // namespace fable2::interp
