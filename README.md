# Dynamic_Terrain_Deformation_Skse

Ground in Skyrim that actually takes a mark. Footprints, trenches through deep snow, wheel ruts, and blast craters are pressed into the landscape itself — real displaced geometry, not decals — and recover over time.

A standalone SKSE plugin. **No Community Shaders dependency**; it works on vanilla Skyrim SE/AE, with or without ENB.

Copyright (c) 2026 NearMidnightNow (NMN). All rights reserved — see [LICENSE](LICENSE).

**The source is here to be read, not to be reused.** It is published so anyone can see exactly what the plugin does to their game — every hook, every draw it touches. That is not the same as an open-source licence, and none is granted. If you want to use part of it, ask.

Authored in full by NearMidnightNow. This is a heavily reduced standalone SKSE port of the author's own Community Shaders features — cs-footprints, cs-snowblanket, cs-snowcollision, and cs-snowbuildup — and contains no code from Community Shaders itself or from any other project.

Third-party build dependencies remain under their own licences, as listed in [LICENSE](LICENSE).

## Building

Requirements:

- Windows x64.
- Visual Studio 2022 or Build Tools 2022, with the **Desktop development with C++** workload, MSVC v143, and a Windows SDK.
- Git, CMake 3.21 or newer, and Ninja available on `PATH`.
- A bootstrapped vcpkg installation, with `VCPKG_ROOT` pointing to its directory.

If you do not already have vcpkg, run the following in PowerShell, choosing a suitable installation directory:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& C:\dev\vcpkg\bootstrap-vcpkg.bat
```

Then clone and build the plugin from PowerShell:

```powershell
git clone --recurse-submodules https://github.com/maglarnet/Dynamic_Terrain_Deformation_Skse.git
cd Dynamic_Terrain_Deformation_Skse
$env:VCPKG_ROOT = "C:\dev\vcpkg"
.\build.cmd
```

Adjust `VCPKG_ROOT` if your installation is elsewhere. The script locates Visual Studio 2022 in its standard installation location, configures a Release build, installs the required vcpkg dependencies, builds the DLL, and runs the offline shader and deformation checks. The first build requires internet access for dependencies.

If you cloned without submodules, run `git submodule update --init --recursive` before building.

The built plugin is `build/release/NMN_DeformableTerrain.dll`. The existing DLL, INI, and texture names are intentionally retained. Runtime settings and textures are in `dist/`.

Building does not install the mod by default. If `SKYRIM_FOLDER` or CMake's `OUTPUT_FOLDER` is configured, CMake can copy the DLL to that location.
