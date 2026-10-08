# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A static recompilation of Fable 2 (Xbox 360, title ID 4D5307F1) built on the ReXGlue SDK v0.10.0. `rexglue codegen` turns the guest PowerPC code in `default.xex` into C++ (`generated/default/fable_2_recomp.*.cpp`, ~291 TUs / ~60k functions) driven by `fable_2_manifest.toml`; this repo adds the host app, hooks, patches, diagnostics, input, a WPF launcher and tooling around that.

Game content (`default.xex`, `data/`) is user-supplied, gitignored, and required for codegen. Never commit game files, XEX dumps, or saves. `generated/` (except `generated/rexglue.cmake`) and `out/` are build output.

## Build and test (Windows only)

The build needs a VS x64 developer shell with LLVM/clang 20+, Ninja, CMake ≥ 3.25, Python, Git and the .NET 8 SDK. It cannot run in a Linux container; there, limit yourself to editing and the Python checks.

```cmd
build.cmd                     # configure + fable_2_codegen (Debug, out\build\win-amd64-debug)
build.cmd fable_2             # full Debug executable
build.cmd -release fable_2    # Release (-O3), out\build\win-amd64-release; use for any perf work
build.cmd fable_2_profiler    # Release + real PDB (no PE cleaning), for VTune/WinDbg
build.cmd -clean [target]     # delete the build dir for that config
build.cmd launcher            # WPF launcher (.NET only); launcher-self-contained bundles the runtime
```

Tests (no game files, GPU or audio device needed):

```cmd
python tools/generate_game_versions.py --check
python -m unittest discover -s tests -p test_*.py
python -m unittest discover -s tests -p test_runtime_staging.py   # single Python test file
dotnet run --project tests/Fable2.Launcher.ConfigTests -c Release
tests\run_native_tests.cmd [sdk-source-dir]            # clang++ one-file native tests -> out\tests
```

Each native test in `tests/native/` is compiled standalone by `run_native_tests.cmd` against SDK sources; add a new one there as another `clang++ ... && run` line.

- Toolchain must be clang: SDK headers use clang builtins, so MSVC `cl` cannot compile `generated/`.
- Codegen re-runs automatically when the manifest or `default.xex` changes.
- `build.cmd` auto-downloads the official prebuilt SDK (codegen tool only) into `out/tooling/rexglue-sdk-0.10.0`. The runtime DLLs are built from the pinned source SDK submodule `thirdparty/rexglue-sdk` plus the tracked patches `thirdparty/rexglue-sdk-*.patch`; the upstream SDK is never forked or pushed. Debug stages `rexruntimed.dll`/`rexgpu-xenosd.dll`, Release `rexruntime.dll`/`rexgpu-xenos.dll`, and CMake rejects mismatched pairs. See `docs/RUNTIME_FIXES.md`.
- `FABLE2_BUILD_PROFILE` defaults to `goty-compatible` (US/EU + German GOTY). Supported editions come from `constants/game_versions.json`; after editing it run `python tools/generate_game_versions.py` and commit the regenerated C#/C++/CMake outputs in the same commit (see `constants/README.md`).
- `.gitignore` ignores `*.py`, `*.ps1`, `*.sh`, `*.png` repo-wide. A new script the build or tests need must get a `!path` exception there or a fresh clone breaks.

## Architecture

**Host app.** `src/main.cpp` includes `generated/default/fable_2_init.h` and a stack of header-only modules, defines cvars, and instantiates `Fable2App` (`src/core/fable_2_app.h`), which overrides SDK lifecycle hooks: `OnPreSetup` (input drivers, GPU plugin selection), `OnPostInitLogging` (loads `fable2_config.toml`, seeds cvars), `OnLoadXexImage` / `OnPostLoadXexImage` (XEX hash verification against the version catalogue, then guest-image data patches), `OnConfigurePaths`, `OnCreateDialogs`. Most diagnostics in `src/diagnostics/` are headers that self-install when included from `main.cpp`.

**Ways to change guest behaviour**, in order of preference:
1. **Mid-asm hooks.** Add `[[entrypoint.midasm_hook]]` (address, name, registers, `after_instruction = true`) to the manifest and define the hook in `src/core/fable2_hooks.cpp` with plain C++ linkage (not `extern "C"`) matching the prototype codegen emits; registers arrive as `PPCRegister&`. Gate every hook on a `[patches]` toggle read from `fable2::config::Get()`.
2. **Hot-function overrides.** Generated functions are weak `extern "C"` aliases of `__imp__<name>`; a strong definition with the same name in `src/core/hotfunc/<category>/` (globbed recursively by CMake) replaces both direct calls and indirect dispatch, and can forward to `__imp__<name>`. See `docs/hotfunc_overrides.md` for the transform rules (non-volatile access, global-lock caching) and which overrides are hand-tuned.
3. **Guest-image data patches.** `config/fable2_patches.toml` (Xenia patch format), applied before launch by `src/core/fable2_patches.cpp`. Data regions only: guest `.text` is never executed, so code-region ops are inert.
4. **Post-codegen text patches.** `tools/apply_recomp_patches.py` (fallback, currently empty; `FABLE2_RECOMP_PATCHES=0` disables).

Never hand-edit `generated/`; it is overwritten by codegen.

**Manifest naming rule.** A manifest `name` becomes the C symbol `__imp__<name>`. It must not equal any XAPI export of the runtime DLL, or it silently shadows the kernel import (this once broke audio via `KeWaitForSingleObject`; such functions are suffixed `_Guest`). The README refers to `tools/check_manifest_collisions.py` and `docs/hotfunc_overrides.md` to `tools/make_hotfunc_overrides.py`, but both are gitignored and absent from the repo, so check names by hand. Manifest boundaries may not overlap (ReXGlue rule). `FUNCTION_NAMES.md` explains each named function; guest SDK runtime code lives at `0x82C00000`–`0x831FFFFF`.

**User config.** `config/fable2_config.toml` is staged next to the exe once and recreated with defaults if missing; it is separate from the SDK cvar file `fable_2.toml`. To add a setting: add a defaulted member to `fable2::config::Values`, read it in `Load()`, and document the key in both `config/fable2_config.toml` and the embedded template in `src/core/fable2_config.cpp` (keep them in sync). Config-backed cvars are only seeded when `fable_2.toml`, `REX_*` env vars or the command line have not set them. Bad config never blocks launch.

**Input.** `src/input/keyboard_gamepad.h` is a synthetic pad driver OR-merged with SDL pads (`keyboard_gamepad_map` cvar, mouse look, F5 Lua trigger). Debug builds also compile `FABLE2_REMOTE_CONTROL`: a localhost JSON-lines TCP server (`127.0.0.1:8791`) driving a second pad, plus `game_state` and `screenshot` commands, client `tools/fable2_control.py`. It must stay compiled out of Release.

**Lua.** `src/lua/*.lua` is staged into `data/scripts/recomp/`; F5 replays the game's `RunScript` with `f5_lua_path` (`src/core/fable2_f5_lua.h`). `FABLE2_LUA_API.md` is the reference for the game's Lua API.

**Renderers.** D3D12 (`--gpu_plugin=xenos`) is the default. Vulkan (`--gpu_plugin=xenos-vulkan`) is a Release-only plugin built from SDK source via `tools\build_sdk_vulkan.cmd`; it is experimental.

**Launcher.** `launcher/Fable2.Launcher` (WPF, .NET 8) detects the edition using the shared version catalogue, keeps its own `launcher-settings.toml`, and writes `fable_2.toml`. See `launcher/README.md`.

## Debugging aids

- Guest function tracing: `FABLE2_FUNC_TRACE=1` (optional `FABLE2_FUNC_TRACE_FILTER`, `FABLE2_FUNC_TRACE_SUBS_ONLY=1` to rank unnamed `sub_` hot functions) writes `fable2_func_trace.log` / `fable2_func_summary.log` next to the exe; `fable2-functrace.cmd` wraps it. Implemented by hooking `REX_FUNC_PROLOGUE` via a PCH in `src/core/fable2_func_trace.h`.
- Debug builds run drastically slower than Release (guest code, runtime and GPU emulator all at `-O0`).
- Investigation write-ups live in `docs/` and `plans/`; check them before re-investigating crashes, FPS caps, text rendering (`TEXT_RENDERING_NOTES.md`) or texture bugs.
