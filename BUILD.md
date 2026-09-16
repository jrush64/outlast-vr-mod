# Building Outlast VR

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools)
  with the **"Desktop development with C++"** workload installed. `build.bat` locates it
  automatically via `vswhere.exe`.
- No other tools or package managers required. MinHook and Dear ImGui are vendored in
  `third_party/` - nothing to fetch or install separately.

## Build

```
scripts\build.bat
```

Output: `builds\d3d9.dll` (and `builds\d3d9.map`).

The build is a single `cl.exe` invocation (no solution/project file, no CMake) that compiles
the mod's own sources together with the vendored MinHook and Dear ImGui sources, statically
linked (`/MT`), so the resulting DLL needs no separate vcruntime redistributable in the game
folder.

## Deploying

Copy these files into `<Outlast install>\Binaries\Win64\`, next to `OLGame.exe`:

- `builds\d3d9.dll` (just built)
- `openxr_loader.dll` - an x64 build of the official [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK), not built by this repo
- `outlastvr.ini` (see `runtime/outlastvr.ini` for a starting point)

See `release/README.txt` for the installer script and end-user install steps.

## Third-party components

| Component | Location | License |
|---|---|---|
| MinHook | `third_party/minhook` | BSD 2-Clause (see its `LICENSE.txt`) |
| Dear ImGui | `third_party/imgui` | MIT (see its `LICENSE.txt`) |
| OpenXR loader | not vendored, obtained separately | Apache 2.0 |
