// fable2_hooks.cpp - mid-asm hook functions for the [[entrypoint.midasm_hook]]
// entries in fable_2_manifest.toml (see docs/patches.md).
//
// Codegen emits an extern prototype for each hook into the generated code
// (e.g. `extern void fable2_hook_website_g1(PPCRegister& r9);`) and calls it at
// the configured instruction address, passing the named registers BY
// REFERENCE, so a hook can read and/or rewrite them. Define each hook with
// plain C++ linkage (NOT extern "C") and exactly once, matching the emitted
// prototype.
//
// Every patch hook is gated on a toggle in fable2_config.toml ([patches]),
// consulted on each call, so a patch can be A/B'd without a rebuild.

#include <bit>
#include <cmath>
#include <cstring>

#include <rex/ppc/context.h>  // PPCRegister (union with u64/s64/u32/... views)
#include <rex/logging/macros.h>

#include "fable2_patches.h"
#include "fable2_tick_rate.h"
#include "fable2_config.h"

// ---------------------------------------------------------------------------
// Guild chest unlock (fable2.com website items + Collectors Edition content).
//
// An unregistered player who clicks the Guild chest sees "Go to www.fable2.com
// for information on how to access the gold and items your heroic ancestors
// left behind." The chest contents are decided by two getter functions that
// run at save load:
//   - GuildChest_GetWebsiteItem_8256E368 (website items)
//   - GuildChest_GetCEContent_824B3528    (CE content)
//
// Each getter is gated by two "registered" flag bits on the chest object, and
// only when both pass does it find the item and call a per-object GRANT vtable
// method, returning THAT method's result. For an unregistered player the gates
// fail (and even where they pass, the grant method returns 0), so the getter
// reports "not available" and the chest stays locked. We force BOTH:
//   1. the two gate bit-extract results to 1 (g1 / g1b hooks below), and
//   2. the grant method's return to 1 (grantnew / grantavail hooks below).
//
// The getters run at save load, so this populates the chest inventory on load;
// the chest then opens normally and shows the website + CE items.
//
// Toggle: [patches] unlock_website / unlock_ce in fable2_config.toml
// (both default true).
// ---------------------------------------------------------------------------

// Unlock Website Items. GATE 1 (bit6 of *(r4+0x90)): the `rlwinm r9, r10, 0,
// 0x19, 0x19` at 0x8256E384 extracts bit 6 into r9 (0x40 or 0). Force r9 = 1
// so the gate always passes.
void fable2_hook_website_g1(PPCRegister& r9) {
  if (fable2::config::Get().unlock_website) {
    r9.u64 = 1;
  }
}

// Unlock Website Items. GATE 1b (bit0 of *(r4+0x40)): the `clrlwi r8, r9,
// 0x1f` at 0x8256E3AC extracts bit 0 into r8 (1 or 0). Force r8 = 1 so the
// gate always passes.
void fable2_hook_website_g1b(PPCRegister& r8) {
  if (fable2::config::Get().unlock_website) {
    r8.u64 = 1;
  }
}

// Unlock Website Items. Grant method sub_8256D940 (the website-chest
// vtable[1] "is this item new?" check): its final instruction is `xori r3,
// r9, 1` at 0x8256D9E4. It returns 0 when the item hash is already in the
// grant list; force r3 = 1 so the getter reports the item as granted.
void fable2_hook_website_grantnew(PPCRegister& r3) {
  if (fable2::config::Get().unlock_website) {
    r3.u32 = 1;
  }
}

// Unlock Collectors Edition Content. GATE 1 (bit6 of *(r4+0x90)): the
// `rlwinm r10, r11, 0, 0x19, 0x19` at 0x824B3540 extracts bit 6 into r10
// (0x40 or 0). Force r10 = 1 so the gate always passes.
void fable2_hook_ce_g1(PPCRegister& r10) {
  if (fable2::config::Get().unlock_ce) {
    r10.u64 = 1;
  }
}

// Unlock Collectors Edition Content. GATE 1b (a bit of *(r4+0x28)): the
// `rlwinm r9, r10, 7, 0x1f, 0x1f` at 0x824B3568 extracts the bit into r9
// (1 or 0). Force r9 = 1 so the gate always passes.
void fable2_hook_ce_g1b(PPCRegister& r9) {
  if (fable2::config::Get().unlock_ce) {
    r9.u64 = 1;
  }
}

// Unlock Collectors Edition Content. Grant method sub_824ACAE0 (the CE-chest
// grant vtable slot called from GetCEContent): its entire body is `lbz r3,
// 993(r3)` (returns the byte at item+0x3E1, the "CE content available" flag).
// Force r3 = 1 so the getter reports the CE content as available.
void fable2_hook_ce_grantavail(PPCRegister& r3) {
  if (fable2::config::Get().unlock_ce) {
    r3.u32 = 1;
  }
}

// Disable Motion Blur. Runs right after `lfs f12, 0xd4(r31)` at 0x822A49E8 in
// the camera update, where the camera's current full-screen motion blur amount
// (+0xD4) is loaded to be copied into the renderer's view settings (+0x7C).
// Forcing f12 to 0 means the renderer never applies the blur, while the camera
// object and any script reading Camera.GetBlur still see the game's value.
// The first non-zero request is logged once, so the log shows whether the game
// actually asked for motion blur during play.
void fable2_hook_disable_motion_blur(PPCRegister& f12) {
  static bool logged = false;
  const bool disable = fable2::config::Get().disable_motion_blur;
  if (!logged && f12.f64 != 0.0) {
    logged = true;
    REXSYS_INFO("[motion-blur] game requested full-screen motion blur {:.3f} ({})", f12.f64,
                disable ? "forced to 0" : "left on");
  }
  if (disable) {
    f12.f64 = 0.0;
  }
}

// Skip Intro Videos (just-harry's "Skip intro videos" patch, as a hook).
// sub_822F4958 builds the boot video queue (microsoft_logo.bik,
// lionhead_logo.bik, terminator) and loops queueing slots until
// CompareString_8229AD78 reports the terminator. This runs right after the
// first CompareString (bl at 0x822F49B8): r3 = 0 means "slot 0 is the
// terminator", so the loop is skipped and no intro video is queued. Harry's
// original NOPs the slot-0 construction instead; the hook leaves all three
// strings constructed and released normally. Runs once per boot.
void fable2_hook_skip_intro_videos(PPCRegister& r3) {
  if (fable2::config::Get().skip_intro_videos) {
    r3.u64 = 0;
  }
}

// High Tick Rate (Xenia patch by Guy), code half. Runs BEFORE the store at
// 0x8233AEB4 that writes the game's LF tick-rate double (0x83319510);
// returning true jumps past it, which is exactly the Xenia patch's NOP.
// Without this the game overwrote the patched value (15 Hz -> 30 Hz, written
// at load by src/core/fable2_patches.cpp) and the patch had no effect.
// Toggle: [patches] high_tick_rate. Also skipped under [patches]
// dynamic_tick_rate, which owns both rates (src/core/fable2_tick_rate.h).
bool fable2_hook_high_tick_rate_skip_store() {
  static const bool enabled = [] {
    const auto& cfg = fable2::config::Get();
    if (cfg.dynamic_tick_rate) {
      if (cfg.high_tick_rate || cfg.higher_hf_tick_rate) {
        REXSYS_WARN("[tick-rate] high_tick_rate / higher_hf_tick_rate ignored: "
                    "dynamic_tick_rate is on");
      }
      REXSYS_INFO("[tick-rate] dynamic: one HF tick per frame at the frame "
                  "rate, up to {} Hz", fable2::tickrate::MaxHfHz());
      return true;
    }
    if (cfg.high_tick_rate) {
      REXSYS_INFO("[tick-rate] LF tick forced to 30 Hz (high_tick_rate)");
    }
    return cfg.high_tick_rate;
  }();
  return enabled;
}

// Higher HF Tick Rate (Xenia patch by Ultra), code half. Runs BEFORE the store
// at 0x8233AE98 that writes the HF tick double (0x83319518, normally 30 Hz =
// twice the 15 Hz LF tick). Skipping it keeps the patched 60 Hz, so with
// high_tick_rate on the HF:LF ratio stays 2:1 (60:30) instead of collapsing
// to 1:1 (30:30), which made cloth physics misbehave.
// Requires high_tick_rate: on its own (HF 60 Hz with LF 15 Hz, 4:1) it would
// break the 2:1 ratio the other way, so it is ignored unless both are on.
// Toggle: [patches] higher_hf_tick_rate (needs [patches] high_tick_rate).
// Also skipped under [patches] dynamic_tick_rate.
bool fable2_hook_high_hf_tick_rate_skip_store() {
  static const bool enabled = [] {
    const auto& cfg = fable2::config::Get();
    if (cfg.dynamic_tick_rate) {
      return true;  // dynamic_tick_rate owns the rate (see the LF hook above)
    }
    if (cfg.higher_hf_tick_rate && !cfg.high_tick_rate) {
      REXSYS_WARN("[tick-rate] higher_hf_tick_rate ignored: it requires high_tick_rate");
      return false;
    }
    if (cfg.higher_hf_tick_rate) {
      REXSYS_INFO("[tick-rate] HF tick forced to 60 Hz (higher_hf_tick_rate)");
    }
    return cfg.higher_hf_tick_rate;
  }();
  return enabled;
}

// Realtime Texture Morphing (hero/dog black textures without CPU readback;
// plans/hero-dog-realtime-texture-morphing.md). sub_82A76018 turns each
// texture morph request into a morph job and copies the request's
// RealTimeTextureMorphing byte with `lbz r9, 0x20(r30)` at 0x82A7607C. With
// it set, the morph renderer (sub_82A69728) draws straight into the final
// uncompressed texture and builds its mips on the GPU, instead of resolving to
// a scratch texture that the CPU reads back and DXT-compresses (that CPU read
// is what returns black on a split-memory host).
void fable2_hook_realtime_texture_morphing(PPCRegister& r9) {
  if (!fable2::config::Get().realtime_texture_morphing) {
    return;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXSYS_INFO("[texture-morph] building hero/dog textures in realtime (GPU) mode");
  }
  r9.u64 = 1;
}

// ---------------------------------------------------------------------------
// Interpolation ([patches] interpolation). Two places where the game shows
// LF-tick state without blending it to the render time.
// ---------------------------------------------------------------------------
namespace {

bool InterpolationEnabled() {
  static const bool enabled = [] {
    const bool on = fable2::config::Get().interpolation;
    if (on) {
      REXSYS_INFO("[interpolation] on: GUI updates every HF tick, cloth "
                  "colliders blended between poses");
    }
    return on;
  }();
  return enabled;
}

uint8_t* GuestMem(uint32_t addr) {
  return fable2::patches::GuestBase() + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
}

uint32_t LoadU32(uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, GuestMem(addr), 4);
  return __builtin_bswap32(v);
}

float LoadF32(uint32_t addr) { return std::bit_cast<float>(LoadU32(addr)); }

}  // namespace

// GUI every HF tick. sub_82278C90 (run on every HF tick from ProcessGameFrame)
// calls the front-end object's vtable[2] (sub_82286B40: GUI manager update,
// subtitles, HUD) only when an LF boundary was crossed (r29) or a query says
// so; the check starts with `clrlwi r10,r29,24` at 0x82278D1C. That update
// measures its own elapsed time from the wall clock (sub_822B6D70), so running
// it every HF tick does not speed anything up; it only updates text twice as
// often (every frame under dynamic_tick_rate). Returning true jumps to the
// call at 0x82278D40.
bool fable2_hook_gui_every_tick() { return InterpolationEnabled(); }

// Cloth colliders blended to the render time. sub_82A895C0 builds the cloth's
// collision shapes from the driving character's skeleton. Its four loops read
// a bone straight from the CURRENT pose (*(inst+512)+16, 48-byte bones: quat,
// translation, scale) right after `add r11,r11,r9`, while the skin it collides
// with is drawn blended between the previous pose (*(inst+516)) and the
// current one with the alpha at *(inst+176)+16 (sub_8222E300, using
// sub_8222E5D0). So the colliders run up to one LF tick ahead of the drawn
// body and jump on every LF tick. Here r11 is pointed at the same bone blended
// the way sub_8222E5D0 + sub_8222E300 blend it (quaternion sign-corrected
// lerp, then normalized; translation and scale lerped), written below the
// stack pointer: the four loads from r11 that follow run before the next call,
// so nothing overwrites it in between.
void fable2_hook_cloth_collider_bone(PPCRegister& r1, PPCRegister& r11,
                                     PPCRegister& r30) {
  if (!InterpolationEnabled()) return;
  if (fable2::patches::GuestBase() == nullptr) return;
  const uint32_t inst = r30.u32;
  const uint32_t cur_pose = LoadU32(inst + 512);
  const uint32_t prev_pose = LoadU32(inst + 516);
  const uint32_t interp = LoadU32(inst + 176);
  if (cur_pose == 0 || prev_pose == 0 || interp == 0 || prev_pose == cur_pose) {
    return;
  }
  const uint32_t cur_bones = LoadU32(cur_pose + 16);
  const uint32_t prev_bones = LoadU32(prev_pose + 16);
  if (cur_bones == 0 || prev_bones == 0) return;
  const float alpha = LoadF32(interp + 16);
  if (!(alpha >= 0.0f && alpha < 1.0f)) return;  // 1 = current pose (or NaN)

  // A bone is 12 contiguous big-endian floats; guest memory is mapped
  // linearly, so each bone is read and written through one host pointer.
  const uint32_t cur = r11.u32;
  const uint8_t* prev_p = GuestMem(prev_bones + (cur - cur_bones));
  const uint8_t* cur_p = GuestMem(cur);
  uint32_t raw_a[12], raw_b[12];
  std::memcpy(raw_a, prev_p, sizeof(raw_a));
  std::memcpy(raw_b, cur_p, sizeof(raw_b));
  float a[12], b[12];
  for (int i = 0; i < 12; ++i) {
    a[i] = std::bit_cast<float>(__builtin_bswap32(raw_a[i]));
    b[i] = std::bit_cast<float>(__builtin_bswap32(raw_b[i]));
  }
  const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  const float sign = dot < 0.0f ? -1.0f : 1.0f;
  float out[12];
  for (int i = 0; i < 4; ++i) out[i] = a[i] + (sign * b[i] - a[i]) * alpha;
  for (int i = 4; i < 12; ++i) out[i] = a[i] + (b[i] - a[i]) * alpha;
  const float len_sq =
      out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3];
  if (len_sq > 1e-12f) {
    const float inv_len = 1.0f / std::sqrt(len_sq);
    for (int i = 0; i < 4; ++i) out[i] *= inv_len;
  }
  const uint32_t scratch = (r1.u32 - 256) & ~15u;
  uint32_t raw_out[12];
  for (int i = 0; i < 12; ++i) {
    raw_out[i] = __builtin_bswap32(std::bit_cast<uint32_t>(out[i]));
  }
  std::memcpy(GuestMem(scratch), raw_out, sizeof(raw_out));
  r11.u64 = scratch;
}

// Steady render delay under dynamic_tick_rate. The render thread draws the
// scene about one LF period in the past: sub_8236C520 loads the LF rate
// (`lfd f0,-27376(r10)` at 0x8236C5A4) and subtracts 1/LF from now. With
// dynamic_tick_rate every tick lasts as long as its frame, so the LF global
// changes every frame, and that delay would jump with it, shaking everything
// drawn. Here the render side gets the smoothed rate (the measured frame rate)
// instead, so the delay only drifts slowly.
void fable2_hook_render_time_lf(PPCRegister& f0) {
  if (!fable2::tickrate::Enabled()) return;
  const fable2::tickrate::Plan plan = fable2::tickrate::CurrentPlan();
  if (plan.frames_per_tick == 0) return;
  f0.f64 = plan.hf_hz / 2.0;
}
