# AutoSeasons

A standalone tool for Skyrim Special Edition that scans your load order for seasonal texture and
mesh variants and automatically generates the plugin and `Data/Seasons/*.ini` files needed for
[Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861) to swap them
in-game — no manual xEdit/CK work required.

## What it does

Point AutoSeasons at your merged load order (via Mod Organizer 2, Vortex, or a plain Data folder)
and it will:

- Scan every Static, Activator, Furniture, MovableStatic, Tree, Flora, and LandscapeTexture record
  for a seasonal sibling texture on disk (e.g. `rock01_AUT.dds` next to `rock01.dds`), recognizing
  several common naming conventions (`_WIN`/`_winter`, `_AUT`/`_autumn`/`_fall`, etc.) so it works
  with texture packs that ship their own naming, not just Seasons of Skyrim's.
- Duplicate the matching records with the seasonal texture set and write the
  `Data/Seasons/*.ini` files Seasons of Skyrim reads at runtime.
- Detect PBR-folder texture variants and author matching PBRNIFPatcher json rules for a downstream
  [PGPatcher](https://github.com/hakasapl/PGPatcher) run.
- Optionally add parallax to landscape textures via
  [TerrainHelper](https://github.com/hakasapl/TerrainHelper) integration, if it's in your load
  order (entirely optional — the core diffuse/normal season swap works without it).
- Detect and swap grass meshes per season, if seasonal grass `.nif` files are present, and can
  drop grass with no winter variant so nothing looks out of place under snow.
- Skip records another mod already covers via its own `Data/Seasons` ini, avoiding redundant
  duplicate records.
- Run fully offline against your files — it never touches the running game.

See the [Wiki](../../wiki) for a full guide, including naming conventions for mod authors who want
their textures/meshes to be picked up automatically.

## Installation

1. Download the latest release and install it like any other mod (MO2: as a regular mod;
   Vortex: extract into a mod folder).
2. Point it at your game install / merged load order and an output folder.
3. Run it, then enable the generated plugin in your mod manager.
4. Requires [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861) to
   actually see anything change in-game — AutoSeasons only generates the data it reads.

Supports Skyrim SE (Steam), Skyrim GOG, Skyrim VR, and Enderal SE.

## For mod authors

If your mod already ships seasonal texture or mesh variants, AutoSeasons will likely pick them up
automatically — see the [Wiki](../../wiki) for the exact naming conventions it looks for, and how
to make sure it does.

## Building from source

Requirements:
- Windows, Visual Studio 2022 (MSVC toolchain) or the standalone Build Tools
- [vcpkg](https://github.com/microsoft/vcpkg) (manifest mode; dependencies are pulled automatically)
- .NET 8 SDK (for the Mutagen-based plugin backend)
- CMake 3.31+, Ninja
- [flatc](https://github.com/google/flatbuffers) (FlatBuffers compiler) on your `PATH`, matching
  the FlatBuffers version vcpkg resolves for this project (see `vcpkg.json`) — a mismatched `flatc`
  can generate headers incompatible with the runtime library.

`--recurse-submodules` is required: [nifly](https://github.com/ousnius/nifly) (NIF file handling)
is checked out as a git submodule under `external/`, not pulled in by vcpkg.

```bash
git clone --recurse-submodules https://github.com/<your-username>/AutoSeasons.git
cd AutoSeasons
cmake -B buildRelease -S . -G Ninja -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build buildRelease
```

The C# Mutagen backend (`ASMutagen/`) is built and published automatically as part of this same
CMake build (via DNNE, which exports it as a native DLL callable from the C++ side) — no separate
`dotnet build` step is needed. The built executable and its runtime dependencies (shaders,
`AutoSeasons_translations/`, .NET runtime files) end up in `buildRelease/bin/`.

## Credits

- Built on [PGPatcher](https://github.com/hakasapl/PGPatcher) by hakasapl — AutoSeasons reuses and
  adapts its mesh/texture/plugin-scanning core.
- [nifly](https://github.com/ousnius/nifly) by ousnius, for NIF file handling.
- [Mutagen](https://github.com/Mutagen-Modding/Mutagen) for reading and writing Bethesda plugin
  files.
- [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861) by powerof3, the
  SKSE plugin this tool generates data for.
- Developed by Cl3mus33 with [Claude](https://claude.com/claude-code), used throughout as an
  active coding collaborator on this project's design and implementation.

## License

GPLv3 — see [LICENSE](LICENSE). AutoSeasons is derived from PGPatcher, also GPLv3-licensed.
