# Faster loading (EXPERIMENTAL, off by default)

> **Warning: experimental.** This is a timing trick, not a real fix. It has had
> only light play-testing. If anything looks, sounds or plays wrong around a
> load, turn it off.

## Goal

Make "Continue" / save loads faster without touching the game's data or logic.
The disk is not the bottleneck: during a load the disk is nearly idle for long
stretches. Most of the wall-clock time is the game **waiting on its own timers**.

## What the profiling showed

Test case: loading a save with "Continue" (about 22.5 s from process start to
the first `Forced readback-resolve` log line, unmodified).

1. **Idle phase, about +9 to +17 s after load start.** The game thread sleeps
   in 100 ms polls while disk reads are ~0. It is waiting for something that
   completes on a timer, not for I/O.
2. **A ~5.05 s game-time timeout stall near the end of the load.** No frame is
   presented for the whole time (logged as `[stall] #1 ... 5052 ms`).

## What the toggle does

While a load is detected, the guest clock is briefly run faster so the game's
timed waits end sooner:

- **idle boost, x16** when the game thread is mostly asleep and the disk is
  nearly idle, within 30 s of load start.
- **stall boost, x8** when no frame has been presented for 250 ms during a
  load, capped at 15 s.

Sleeps and wait timeouts are divided by the clock scalar, so they shrink while
boosted. Boost ends as soon as reads resume, the thread becomes busy, or the
window is over. Outside loads nothing changes.

## Side effects (why it is experimental)

Game time jumps forward by a few seconds in total during a load. Anything driven
by game time (animations, music, scripts, timers) can briefly run fast or
misbehave. Testing so far only showed normal behaviour, but it was not extensive.

## Enable it

In `fable2_config.toml`:

```toml
[loading]
faster_loading = true
```

Default is `false`. When on, the log prints a `faster_loading is ON (EXPERIMENTAL)`
warning at startup. Fine tuning is via SDK cvars in `fable_2.toml`:
`idle_boost` (16), `stall_boost` (8), `idle_boost_window_s` (30),
`stall_boost_max_s` (15), `diag_stall_ms` (250), `load_timer_start_mb` (20),
`load_timer_quiet_s` (5). Boost is clamped to x16.

## Results

Metric: process start to first `Forced readback-resolve` log line. Single runs,
one machine.

| Run | Settings | Time |
|---|---|---|
| baseline (4 runs + vanilla + control) | off | 22.46 - 22.61 s |
| 014 | idle x8, stall x8 | 17.6 s |
| 016 | idle x8, stall x8 | 18.4 s |
| 017 | idle x16, stall x8 | 16.3 s |
| 019 | idle x16 (32 requested, clamped) | 16.3 s |
| 020 | idle x16 (64 requested, clamped) | 16.6 s |
| 021 | idle x16 (256 requested, clamped) | 17.4 s |

So about **4-6 s faster** (22.5 s down to 16-18 s). The 5.05 s stall in the
control run (log 018) happens *after* that marker (about +29.6 s) and does not
appear as a long stall in the boosted runs, so the end-to-end saving should be
larger than the table shows. That was not measured as a "time to playable"
number, so treat it as likely rather than confirmed. The remaining ~16 s is
real work.

## What did not help

Precise sleep, cheaper trace hooks, a global fast-forward hotkey, spin backoff,
capping load sleeps, a lock-contention probe, vsync/FPS cap (vsync was already
off), and idle boost above x16 (clamped; runs 019-021 were all x16).

## Known limits

- Root cause of the 5 s timeout and of the 3D engine job loop is not explained.
- Light play-testing only.
- Needs the SDK change below.

## SDK

Implemented in the SDK, branch `faster-loading`, commit
`d0a248e06b13c0abda6856690cec651b23f1da0a`
(`src/kernel/xboxkrnl/xboxkrnl_load_profile.cpp`, `src/core/clock.cpp`, plus
small hooks in the io/threading/video kernels). The `faster_loading` cvar is
off by default. Diagnostics (`diag_load_profile`) are separate and also off.
