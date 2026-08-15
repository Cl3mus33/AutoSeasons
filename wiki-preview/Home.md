# AutoSeasons Wiki

AutoSeasons scans your Skyrim SE load order for seasonal texture/mesh variants and generates the
plugin + `Data/Seasons/*.ini` files that [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861)
(the SKSE plugin, by powerof3) reads at runtime to swap them in-game. AutoSeasons never touches the
running game itself — it's a pure offline data generator you run once (or whenever your load order
changes) before playing.

**This folder is a preview of the wiki content** — GitHub Wiki isn't published yet, so these pages
live here as plain markdown for now. Once the wiki is enabled, each file below becomes one wiki
page verbatim (file name → page title).

## Pages

- **[Usage Guide](Usage-Guide.md)** — installing, pointing AutoSeasons at your load order, running
  it, and what to do with the output. Start here if you're a player/user.
- **[Naming Conventions](Naming-Conventions.md)** — for mod authors: exactly which texture/mesh
  filename suffixes AutoSeasons recognizes as seasonal variants, so your own textures get picked up
  automatically with zero extra configuration from your users.
- **[Compatibility](Compatibility.md)** — how AutoSeasons interacts with other tools in a typical
  pipeline: PGPatcher, DynDOLOD/xLODGen, TerrainHelper, Community Shaders, and other mods that ship
  their own `Data/Seasons` declarations (Turn of the Seasons, Nature of the Wild Lands, etc.).
- **[Troubleshooting](Troubleshooting.md)** — "why didn't X get a seasonal variant," "it says 0
  records were created," conflicting mods, and how to read the log.
- **[FAQ](FAQ.md)** — quick answers: VR/Enderal support, running through your mod manager, and
  other common questions.

## The short version

1. Install AutoSeasons as a mod in MO2/Vortex like any other.
2. **Run it through your mod manager's tool list/dashboard** — not by double-clicking the exe in
   Explorer (see [Usage Guide](Usage-Guide.md) for why this matters).
3. Point it at your game install and an output folder, click Start Patching.
4. Enable the generated `AutoSeasons.esp` in your mod manager's plugin list.
5. Install and run [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861) —
   it's what actually performs the swap in-game. AutoSeasons alone changes nothing.
