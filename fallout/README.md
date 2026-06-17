# Faster Loadscreens

A universal Fallout 4 mod that speeds up loading screens and displays custom concept art backgrounds with the game's loading tips. Works on **VR**, **OG** (1.10.163), **NG** (1.10.980/984), and **AE** (1.11.191).

## Features

- **Custom background images** — Random DDS artwork shown during loading (from `Data/Textures/LoadingScreens/`)
- **Loading speed optimization** — Breaks the animation loop so loading runs at full CPU speed
- **Three loading screen modes** (configurable via MCM):
  - **Black (fastest)** — Plain black screen, no rendering
  - **Background only** — Random concept art image
  - **Background + Tips** — Art with game's loading tips and level progress (default)
- **VR support** — World-locked overlays with captured tip display
- **Performance patches** — VSync disable during loading, PresentThread yield, iFPSClamp bypass

## Requirements

- Fallout 4 (any version: VR, OG, NG, or AE)
- [F4SE](https://f4se.silverlock.org/) matching your game version
- [Address Library](https://www.nexusmods.com/fallout4/mods/47327) for your game version

## Installation

Install with your mod manager of choice (Mod Organizer 2 recommended). The mod folder structure:

```
Data/
  F4SE/Plugins/LoadingScreens.dll
  MCM/Config/FasterLoadscreens/config.json
  MCM/Config/FasterLoadscreens/settings.ini
  Textures/LoadingScreens/*.DDS
```

Add your own DDS images (DXT1/DXT5, recommended 2048x1024) to `Data/Textures/LoadingScreens/` for custom backgrounds.

## Building from Source

Requires:
- Visual Studio 2022
- CMake 3.23+
- vcpkg (`VCPKG_ROOT` environment variable set)
- [CommonLibF4](https://github.com/alandtse/CommonLibF4) (alandtse fork with VR/NG support)

```powershell
cmake -B build -S .
cmake --build build --config Release
```

Output DLL is copied to `package/Data/F4SE/Plugins/`.

## License

MIT
