# Windows launcher

A .NET 8 WPF settings launcher for verified GOTY USA/Europe and German GOTY
dumps. It validates the original XEX and content markers, then checks the
native build descriptor. Unknown revisions and known mixed retail/TU1 content
are rejected; original game files are never modified.

## Extracting from an ISO

"Extract from ISO" opens a file picker for a raw Xbox 360 disc image (`.iso`)
and extracts straight into the folder the launcher is running from (next to
`fable_2.exe`) — there is no destination picker. The ISO content is written in
two phases:

1. **`default.xex` only** — extracted and SHA-256-hashed first.
2. **The remaining disc** (`data`, `nxeart`, `$SystemUpdate`, …) — written
   only if the hash matches a known-good build in the version catalogue
   (`GameCompatibilityInspector.CheckHash`, same catalogue the launcher uses
   to validate game folders).

An ISO the project does not support therefore costs one ~21 MB file, which is
deleted on failure — the ~6.3 GB extraction never starts. On success the
launcher's folder is automatically set as the selected game folder. The
GDFX/ISO 9660 readers come from the shared `X360Extract` library
(`tools/x360extract/lib`, referenced as a project dependency), so the output is
byte-for-byte identical to the standalone `x360extract` tool; STFS containers
are copied raw.

Settings: 720p/1080p/1440p/4K output, 1x–4x internal render scale, anisotropic
filtering (game default through 16x), none/FXAA/FXAA Extreme, VSync,
windowed/borderless/exclusive fullscreen and 30/60/120/144/165/240/unlimited FPS. Output size
is separate from the original 720p guest mode. The FPS options need the
matched source-built Release runtime described in
[the runtime guide](../docs/RUNTIME_FIXES.md). Higher rates are not a promise
of correct timing in every scene.

Dropdowns use dark text on a light background, including the selected item.
There are no texture/font replacements, remaster switches or save editors.
The launcher and game use an original project-owned book/tree icon, not
extracted game art; see [asset provenance](../assets/README.md).

Build from the repository root:

```cmd
build.cmd launcher                 self-contained (default; ships with .NET)
build.cmd launcher-dev             framework-dependent (needs .NET installed)
dotnet run --project tests/Fable2.Launcher.ConfigTests -c Release
```

The tests also cover the extraction hash gate (every known-good hash is
accepted, every unsupported/unknown hash is rejected before extraction).

## Shipping without a .NET install

The default build is a **self-contained single-file publish**: the .NET 8
Desktop Runtime is bundled inside `Fable2Launcher.exe` (~160 MB), so users
need no .NET install and no separate runtime installer is shipped. The same
applies to `x360extract.exe` (see `tools/x360extract`, default build is
self-contained too). `build.cmd launcher-dev` produces the small
framework-dependent exe for fast local iteration; it requires the .NET 8
Desktop Runtime on the target machine.

Output is `out/tests/launcher-build/Fable2Launcher.exe`. Put it beside
`fable_2.exe`, its matched DLLs, `fable2_build.json` and `app-icon.png`.

A fresh launcher starts with no selected game folder. After choosing one,
`launcher-game-path.txt` beside the launcher remembers that user's location.
No developer path, global fallback or automatic dump selection is embedded.
The directory must be writable to persist settings.

`launcher-settings.toml` stores only the managed preferences. The launcher
reads existing `fable_2.toml` first for legacy configurations, then overlays
those preferences so runtime rewrites cannot reset output resolution or render
scale. Save/start writes both files. Unknown engine settings are preserved,
and the first engine-config write creates a one-time backup. Saves, caches and
logs remain beside the native EXE, not in the original dump.

F3 in the game shows the guest-swap FPS counter in Release.
`StartWithoutAudio.cmd`, staged with the native EXE, exercises the clocked
silent fallback without disabling any Windows device. Exit existing game and
launcher processes before running that test.


Launcher overview supplied by the tester (before the high-refresh presets were added):

![Launcher overview](../docs/screenshots/launcher.png)
