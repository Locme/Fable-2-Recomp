# Test report: Vulkan CPU memexport and vexptefp range fix (branch `vulkan-cpu-memexport`)

Tested 2026-11-17 on the development PC (Windows 11, 60 Hz display). Release
builds of this branch: master (c764cba) plus the two commits, SDK submodule
at `vulkan-cpu-memexport` (base c26d8b6 plus the two SDK commits). Two
backends:

- D3D12: `rexgpu-xenos.dll`.
- Vulkan: the dual-backend source plugin from `tools\build_sdk_vulkan.cmd`.
  `rexruntime-vulkan.dll` and `rexgpu-xenos-vulkan.dll` were copied over the
  D3D12 pair for the run, as `tools\fable2.cmd vulkan` does.

## Method

- Each run:
  - Launch the game off-screen (window at x = -6000, never focused).
  - Run console `boot slot 1`. That's the adult save, with a female hero, and
    it loads in Bowerstone Market.
  - Skip the intro videos. Wait 15 s, measure for 40 s, then kill the game.
  - Only one-shot console scripts were run. Nothing was left registered in the
    GeneralScriptManager, and nothing was saved.
- fps comes from the game's FPS meter (`FABLE2_FPS_METER=1`, one line per
  frame). It's frames / summed frame time over the last 40 s. "Worst" is the
  longest frame in that window.
- "Uncapped" = `vsync = false`, which makes the SDK run the guest vblank at
  1000 Hz. "Paced" = the default 30 fps pacing.
- "Off" = `terrain_tess_cpu = false` and `skinning_cpu = false` in
  `[patches]` of `fable2_config.toml`. When both are on, the app sets
  `readback_memexport_fast = false`. When they're off, the SDK default (fast
  readback, which hands over the previous result) stays, unless
  `readback_memexport_fast = false` is set in `fable_2.toml`. "Off, sync"
  means that line was set. Master's Vulkan behaviour with the options on in
  its config was "off, sync": master warned that the two cvars were missing
  in the Vulkan build, but still set `readback_memexport_fast = false`.
- Readback counts come from a temporary counter build of the Vulkan plugin.
  The patch was never committed. Every 5 s it logged three per-second counts:
  - synchronous readbacks (`IssueDraw_MemexportReadbackFullPath`, which waits
    for the GPU queue);
  - fast readbacks (`IssueDraw_MemexportReadbackFastPath`, which hands over
    the previous result);
  - draws whose result was computed on the CPU.

  The clean plugin was restored after those runs (hash checked).
- Other sessions on this PC were testing other branches of the game, so game
  runs took turns under a shared lock. A run waited until:
  - no other copy of the game was running;
  - no build was running;
  - no other process used more than 5% of the GPU 3D engine.

  The exe was renamed for these runs so other sessions' cleanup couldn't kill
  them. Each run also checked for other GPU users afterwards. Two Vulkan
  uncapped runs (26.9 and 2.7 fps) overlapped another project's game holding
  about 22% of the GPU. They agree with the clean counter runs but aren't used
  below. Every earlier fps number from this session was discarded for the
  same reason.

## 3. vexptefp range fix (always on)

- SDK codegen (`src/codegen/builders/vector.cpp`): the exponent is clamped to
  [-126, 127]. Inputs below -126 flush to 0, and inputs from 128 up give
  +inf. NaN passes through. The old code gave garbage below -127, about 0
  from 128, and NaN for +-inf.
- Numeric check of the new code against `exp2f` (from the original branch)
  gave equal results. The inputs were -inf, -1000, -200, -127, -126.5, -126,
  -10, -1, 0, 0.5, 1, 10, 127, 127.9, 128, 129, 1000, +inf and NaN. Results
  below 2^-126 flush to 0.
- SDK test `tests/ppc/asm/instr_vexptefp.s` has a new case, `test_vexptefp_range`:
  -200, 128, -inf, +inf -> 0, +inf, 0, +inf. Both vexptefp cases (`test_1` and `test_range`) pass on this branch's
  generated code. With the range lines removed (the old code), `test_range`
  fails on three of its four lanes (-200, -inf, +inf) and `test_1` still
  passes. See Build for how they were run.
- Regenerated game code, old SDK vs this branch: all 7 vexptefp sites changed,
  and nothing else in those functions did.

  | Function | Instruction |
  |---|---|
  | `sub_82BA34B0` | `vexptefp v10,v13` |
  | `sub_82BA3598` | `vexptefp v9,v3` |
  | `sub_82CE24B0` | `vexptefp128 v61,v62` |
  | `sub_82D09D00` | `vexptefp128 v62,v61` and `vexptefp128 v63,v63` |
  | `sub_82D0A168` | `vexptefp128 v63,v63` |
  | `sub_82D0B2C0` | `vexptefp128 v63,v63` |

  Each site gains the clamp, an `andnot` against `x < -126`, a blend to +inf
  for `x >= 128`, and a blend back to `x` for NaN.
- In game: on the original branch, 9 shots were compared between the old
  build and the new build: Bower Lake dusk/dawn/night, Wraithmarsh
  now/dusk/night, and Bloodstone now/dawn/dusk. The mean absolute difference
  was 0.1-5 per channel, from moving NPCs and the dog. The share of strongly
  blue pixels was identical.
- **This is a correctness fix with no visible change in these scenes.** It
  does not fix the blue/purple sky. That's a separate issue, already fixed on
  master. This fix has no toggle.

## 6. Vulkan CPU skinning and terrain tessellation

The CPU implementations moved from `src/graphics/d3d12/` to `src/graphics/`
(namespace `rex::graphics`, via `git mv`). The two cvars are now defined in
the shared command processor and declared in `flags.h`. The Vulkan draw path
skips the readback when the CPU computed the result, the same way D3D12 does.
The Vulkan boot log no longer warns that `terrain_tess_cpu`/`skinning_cpu`
are missing.

All runs below are in Bowerstone Market on Vulkan, with the counter plugin
where readback counts are shown.

| Run (Vulkan) | fps | worst ms | sync readbacks/s | fast readbacks/s | computed on CPU/s |
|---|---|---|---|---|---|
| on, uncapped | 29.0 | 59 | 0 | 0 | 1,120-1,230 |
| off, sync, uncapped (master's behaviour) | 3.0 | 556 | 109-207 | 0 | 0 |
| off, fast default, uncapped | 29.9 | 50 | 0 | 1,222-1,230 | 0 |
| on, paced | 30.0 | 62 | 0 | 0 | 1,225-1,234 |
| off, sync, paced (two runs) | 30.0 / 30.0 | 64 / 62 | 1,223-1,232 | 0 | 0 |

Clean runs on the normal plugin, without counters:

| Run | fps | worst ms |
|---|---|---|
| Vulkan on, uncapped (round 1 / round 2) | 89.4 / 81.8 | 42 / 29 |
| Vulkan off, sync, uncapped (round 1 / round 2) | 30.0 / 25.2 | 143 / 75 |
| Vulkan on, paced | 30.0 | 48 |
| Vulkan off, sync, paced | 29.9 | 64 |
| D3D12 on, uncapped | 105.0 | 250 |
| D3D12 off, uncapped | 91.7 | 38 |

- **Readbacks:** with the options on, Vulkan reads 0 memexport results back
  and computes about 1,230 draws per second on the CPU. With them off and sync
  readback, every one of those draws waits for the GPU.
- **fps:**
  - Uncapped, the options are always faster than sync readback in the same
    round: 29.0 vs 3.0 (counter build), 89.4 vs 30.0, and 81.8 vs 25.2. That
    makes them 3-10x faster.
  - The absolute numbers moved a lot between rounds. The counter-build runs
    were at 16:32-16:38 and the plain runs at 18:57-19:04. Every run passed
    the same checks: no other game, build or GPU user over 5%, same save.
    Only the direction and the readback counts are claimed, not a specific
    fps.
  - The uncapped runs present with Vulkan's immediate mode (no display
    sync, `vsync=0` in the log), so the ~30 fps rounds weren't a display
    cap. Their frames were 30-45 ms each.
  - At 30 fps pacing, this PC holds 30 fps either way, so the gain there is
    the removed GPU round trips, not frame rate.
- **Fast readback** (the SDK default when the options are off) also avoids
  the waits, but it hands the game the previous frame's skinning and
  tessellation output. With the options on, the game gets this frame's
  result.
- **D3D12 is unchanged.** It already used these CPU paths on master, and it
  booted and played normally with the options on and off. Its fps numbers
  vary a lot between runs on this PC (see the original branch's report), so
  the on/off difference there isn't a claim.
- Every run loaded the same save with the same hero, stayed alive for the
  whole window, and had no other copy of the game running.

## Build

- Release build of `fable_2` (D3D12) and the Vulkan plugin via
  `tools\build_sdk_vulkan.cmd`: both passed. The AVX2 build was not
  rebuilt; this PR doesn't touch it.
- SDK `ppc_tests` (built with `REXGLUE_BUILD_TESTS=ON`): the asm tests
  assembled and `rexglue recompile-tests` generated the test code. The
  generated test files compiled, but the `ppc_tests` link fails on Windows at
  the SDK base too. `rex::memory::Memory` and parts of the core library
  aren't exported from the shared `rexruntime`. That's unrelated to this PR,
  and it isn't fixed here.
- To still run the vexptefp tests, the generated `instr_vexptefp_*`
  functions and both `vexptefp` test cases were compiled into a small
  standalone program, with the same compiler flags as `ppc_tests`. It's not
  part of the branch. Result: 2/2 pass. The negative control is described
  in section 3.
