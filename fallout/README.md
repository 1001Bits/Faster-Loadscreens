# Faster Loadscreens

A Fallout 4 mod that speeds up loading screens and displays custom concept art backgrounds with the game's loading tips.

## Supported runtimes

The plugin supports these exact game builds:

- Fallout 4 VR **1.2.72**
- Fallout 4 OG **1.10.163**
- Fallout 4 AE **1.11.221**
- Fallout 4 AE **1.11.240**

Older next-generation builds **1.10.980**, **1.10.984**, and **1.11.191** are intentionally not accepted. Several hooks and performance patches do not have verified addresses on those executables, so treating them as compatible would silently disable major features or risk patching the wrong code.

## Features

- **Custom background images** — Random DDS artwork shown during loading (from `Data/Textures/LoadingScreens/`)
- **Loading speed optimization** — Breaks the animation loop so loading runs at full CPU speed
- **Four loading screen modes** (configurable via MCM):
  - **Black (fastest)** — Plain black screen, no rendering
  - **Native (no 3D)** — Bethesda's tips and progress without the rotating model
  - **Background only** — Random concept art image
  - **Background + Tips** — Art with the game's loading tips and level progress
    (default); when Bethesda selects a minimal/no-tip screen, the plugin follows
    that choice with solid black instead of showing unrelated artwork
- **VR support** — World-locked overlays with captured tip display
- **Performance patches** — load-only VSync/350-FPS policy, verified timer/iFPSClamp bypasses, and 3D-model/animation-loop suppression
- **Interior preloading retired** — Full mode forces Bethesda's
  `bPreloadLinkedAreas` off and ignores removed speculative preload controls.
  The plugin never submits speculative interior cells and does not change the
  teleport radius or interior cell buffer.
- **Exterior gate look-ahead** — The loaded-reference/TES::PreloadWorld path
  ships on at a one-cell radius. Version 2.1.1 initializes destination worldspace
  data before queueing a cell, defers scene attachment to the real transition,
  and retains the graph until native form cleanup. `bPreloadExteriorGates=1`
  remains the default. Diamond City target selection is confirmed in
  both directions; the exit destination was already resident in the validation
  run, so additional cold-exit benefit is not yet claimed.
- **Immediate MCM updates** — Settings-file changes are debounced and applied on
  the game thread before the next load, while active loads defer disk access.
- **Read-only save cache warm** — While the main-menu confirmation is open, the
  selected local save can be read into the Windows file cache. The below-normal
  worker is capped at 128 MiB/2.5 seconds, and the real-load hand-off waits no
  more than 250 ms for cancellation.
- **Runtime-correct fade restore** — Before any custom write, the plugin captures
  the running game's six fade/scene-settle values. Vanilla mode restores that
  OG/AE/VR- and INI-specific snapshot. Shorter custom fades can shorten a visible
  transition or a benchmark that includes it, but not the cell/save I/O.

The hidden startup-only `iBenchmarkMode` setting ships as `1` (the complete
plugin). Set it to `0` only for a timing-only baseline and restart Fallout after
changing it; it is deliberately not exposed as a live MCM control.

## Requirements

- One of the exact Fallout 4 runtimes listed above
- [F4SE](https://f4se.silverlock.org/) matching your game version
- [Address Library](https://www.nexusmods.com/fallout4/mods/47327) for your game version
- [Mod Configuration Menu](https://www.nexusmods.com/fallout4/mods/21497) only
  if in-game settings are desired; direct `settings.ini` configuration works
  without it

## Installation

Install with your mod manager of choice (Mod Organizer 2 recommended). The mod folder structure:

```
Data/
  F4SE/Plugins/LoadingScreens.dll
  MCM/Config/FasterLoadscreens/config.json
  MCM/Config/FasterLoadscreens/settings.ini
  Textures/LoadingScreens/*.DDS
```

Add your own landscape DDS images (DXT1/DXT5, 2048x1024) to
`Data/Textures/LoadingScreens/` for custom backgrounds. The curated release
set is validated as 2:1 landscape so VR never receives square loading art.

## Building from Source

Run the following commands from the repository's `fallout/` directory.

Requires:

- Visual Studio 2022
- CMake 3.23+
- vcpkg (selected through `VCPKG_ROOT` or an existing vcpkg toolchain file)
- A clean [alandtse/CommonLibF4](https://github.com/alandtse/CommonLibF4)
  checkout at revision `2b64114a449ebdcaa82d33d83d8df9bb8d8092d5`
  with flat, NG, and VR support

The configure step never downloads CommonLibF4. Point it at a reviewed local checkout with either a cache option:

```powershell
cmake -S . -B build -DCOMMONLIBF4_PATH="C:/src/CommonLibF4"
cmake --build build --config Release
```

or set `COMMONLIBF4_PATH` in the environment before running `cmake -S . -B build`. An in-tree checkout at `extern/CommonLibF4` is discovered automatically. Configure rejects another revision or local CommonLibF4 source changes so release builds are reproducible. A developer can explicitly opt into a non-reproducible patched dependency with `-DCOMMONLIBF4_ALLOW_DIRTY=ON`. The output DLL is copied to `package/Data/F4SE/Plugins/`.

This repository contains the source and MCM configuration. The 80 background
DDS files are distributed separately with the mod; they are not stored in Git.
To run the source-only policy tests after building:

```powershell
ctest --test-dir build -C Release -R "^(policy_logic|worldspace_preload_lifetime)$" --output-on-failure
```

For release validation and packaging, copy the release's
`Data/Textures/LoadingScreens/*.dds` into
`release/Faster Loadscreens/Textures/LoadingScreens/`. Then create the complete
release ZIP from the current DLL, both MCM files, and all 80 loading-screen assets:

```powershell
cmake --build build --config Release --target package_release
```

The target recreates a clean staging directory and writes `dist/Faster-Loadscreens-2.1.1.zip`, so stale files from an older package cannot survive. Its input check structurally walks the MCM controls, rejects duplicate control IDs and duplicate active INI keys, limits help text to one sentence, enforces the full-plugin benchmark arm, internal preload defaults, and compiled performance-patch policy, and validates all 80 DDS files as 2048x1024 landscape images. Packaging then checks the final archive manifest.

Run the same validation through CTest after building:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

## License

GPL-3.0; see the repository's [LICENSE](../LICENSE).
The pinned CommonLibF4 dependency retains its own MIT license; see
[DEPENDENCIES.md](DEPENDENCIES.md) for build provenance.
