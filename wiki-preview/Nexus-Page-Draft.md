# Draft Nexus Mods page for AutoSeasons

Copy/paste each section into the corresponding field on Nexus's "Add file"/"Edit mod" pages. Fill
in the bracketed placeholders (screenshots, your own links) before publishing.

---

## Title

```
AutoSeasons
```

## Summary (short teaser, shown in search results and mod lists - keep it under ~200 characters)

```
Scans your load order for seasonal texture/mesh variants and auto-generates the plugin and
Data/Seasons ini files Seasons of Skyrim needs to swap them - no manual xEdit work required.
```

## Category

**Utilities for Modders** (matches Seasons of Skyrim's own category for this kind of tool - it's a
data-generation utility, not a content mod).

## Suggested tags

`Utilities for Modders`, `Terrain`, `Foliage (Plants)`, `Models/Meshes`, `Landscaping`, `Automation`

## Requirements

```
Seasons of Skyrim SKSE (hard requirement - AutoSeasons only generates the data; Seasons of
Skyrim is what actually performs the swap in-game)
```

Optional, only if you use the relevant feature:
```
PGPatcher (for PBR/Complex Material texture support)
TerrainHelper (for landscape parallax)
DynDOLOD / xLODGen (for seasonal LOD - see the Compatibility section below)
```

---

## Description

### About this mod

AutoSeasons scans your Skyrim SE load order for seasonal texture and mesh variants (e.g.
`rock01_AUT.dds` next to `rock01.dds`) and automatically generates the plugin and
`Data/Seasons/*.ini` files that [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861)
reads at runtime to swap them in-game - no manual xEdit/CK work required. Point it at your load
order, click Start, and it does the rest.

Beyond the base scan-and-generate, AutoSeasons also actively manages how it interacts with other
seasonal-content mods already in your load order (Turn of the Seasons, Nature of the Wild Lands,
etc.), instead of just adding its own `Data/Seasons` file alongside theirs and hoping for the best:

- It **detects every other mod's own seasonal declarations** and merges them with its own into one
  authoritative ini per season, rather than leaving Seasons of Skyrim's own file-priority system to
  sort out several separate, potentially conflicting files.
- By default it **respects another mod's coverage** - if a mod already declares a seasonal swap for
  a record, AutoSeasons won't generate a redundant duplicate on top of it.
- A built-in **"Manage Season Mod Conflicts" dialog** lets you override that default per mod - either
  globally or for a specific record type only (e.g. respect a mod's landscape textures but override
  its trees) - so AutoSeasons' own generated textures win instead when that's what you want.
- When two or more foreign mods both declare a swap for the very same record, you can set an
  **explicit priority order** between them (the same idea as mod order in MO2/Vortex) to resolve it
  deterministically - and AutoSeasons will warn you in the log/completion summary if such a
  conflict is ever left unresolved, instead of silently picking whichever file a directory scan
  happened to visit last.

In short: AutoSeasons isn't just a generator, it's also a conflict resolver for your load order's
seasonal content as a whole.

### Companion mod: AutoBlend

Released alongside AutoSeasons, from the same author: **[AutoBlend](#)** *(add the Nexus link once
its own page is up)* scans your load order for landscape texture variants and patches the meshes
that use them to alpha-blend instead of alpha-test - the same fix mods like Vanaheimr ship by hand
for their own textures, generalized to your whole load order. Same standalone-tool philosophy as
AutoSeasons (point it at your merged load order, get a generated plugin back, no manual xEdit
work), same MO2/Vortex-based workflow, and shares AutoSeasons' own native shell architecture
(wxWidgets + a DNNE-bridged .NET/Mutagen patch backend) - if you're already comfortable running
AutoSeasons, AutoBlend will feel immediately familiar. Neither is a hard requirement for the
other, but **if you use both, always run AutoSeasons first and AutoBlend second** - AutoBlend
derives its output from whichever texture set is currently active on a shape, so it needs to see
AutoSeasons' own seasonal duplicates already in place to build on top of them correctly.

### Features

- Scans Static, Activator, Furniture, MovableStatic, Tree, Flora, and LandscapeTexture records for
  a seasonal sibling texture on disk, recognizing several common naming conventions (`_WIN`/
  `_winter`, `_AUT`/`_autumn`/`_fall`, etc.) so it works with texture packs that ship their own
  naming, not just Seasons of Skyrim's own short codes.
- Detects and duplicates seasonal grass meshes too, with an option to drop grass entirely in
  winter if no winter variant exists.
- Full PBR / Complex Material support: detects textures under `textures\pbr\...` and authors
  matching PBRNIFPatcher rules for a downstream [PGPatcher](https://www.nexusmods.com/skyrimspecialedition/mods/120946)
  run, plus PBRTextureSets configs for Community Shaders.
- Optional [TerrainHelper](https://www.nexusmods.com/skyrimspecialedition/mods/143149) integration
  for landscape parallax - entirely optional, the core diffuse/normal swap works without it.
- Detects other mods' own `Data/Seasons` declarations and folds them into one merged,
  authoritative ini instead of leaving Seasons of Skyrim to sort out several separate files.
- **Manage Season Mod Conflicts**: pick, per foreign mod (globally or per record type), whether
  AutoSeasons should respect its declarations or override them with its own generated textures -
  and set an explicit priority order for when two foreign mods genuinely compete over the same
  record.
- Real diagnostics: every skip decision is logged (season-locked EditorID, foreign coverage, no
  seasonal texture found, etc.), and the completion screen shows the actual record count instead
  of a generic "success" message.
- Dry-run/preview mode to safely try out blocklist changes without touching your output folder.
- Localized GUI (English, French, Spanish, German, Italian, Portuguese-BR), light/dark/system
  theme, and a full CLI mode for scripted/automated runs.
- Runs entirely offline against your files - it never touches the running game.

### ⚠️ Run it through your mod manager, not by double-clicking the exe

If you use Mod Organizer 2, add AutoSeasons to MO2's executables list (or use its own entry if
registered automatically) and launch it from inside MO2, the same way you'd launch xEdit - MO2's
virtual file system only applies to processes it launches itself. Double-clicking the exe from
Explorer only sees your game's real, unmodified Data folder, not your installed mods, and will
silently produce an incomplete result. Vortex users: launch from Vortex's dashboard the same way.

### Installation

1. Install like any other mod (MO2: as a regular mod; Vortex: extract into a mod folder).
2. Run it through your mod manager's tool list (see warning above).
3. Point it at your game install/merged load order and an output folder, click Start Patching.
4. Enable the generated `AutoSeasons.esp` in your plugin list, and make sure the output mod is
   enabled so its files are part of the merged Data view.
5. Requires [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861) to
   actually see anything change in-game.

### For mod authors

If your mod already ships seasonal texture or mesh variants under the naming conventions above,
AutoSeasons will detect and use them automatically - your users don't need to configure anything.
Full naming reference in the [wiki](../../wiki/Naming-Conventions).

### Compatibility

- **[PGPatcher](https://www.nexusmods.com/skyrimspecialedition/mods/120946)**: run it *after*
  AutoSeasons - it needs to see the generated plugin and PBR rules.
- **DynDOLOD / xLODGen**: DynDOLOD has its own seasonal LOD generation and reads the exact
  `Data/Seasons/*.ini` files AutoSeasons writes. Recommended order: AutoSeasons → PGPatcher (if
  used) → xLODGen (terrain, with seasonal identifiers) → DynDOLOD. See the wiki's
  [Compatibility page](../../wiki/Compatibility) for the full explanation.
- **Other seasonal-content mods** (Turn of the Seasons, Nature of the Wild Lands, etc.): AutoSeasons
  detects and respects their own `Data/Seasons` declarations by default, with an in-app dialog to
  override or reprioritize specific mods if you want AutoSeasons' own textures to win instead.

### Known limitations

- LOD (distant objects/trees) only reflects the season if DynDOLOD/xLODGen were run *after*
  AutoSeasons with seasonal identifiers - see Compatibility above.
- A record needs an actual seasonal texture/mesh file to exist somewhere in your load order;
  AutoSeasons can't invent art that doesn't exist.

### Credits

- Built on [PGPatcher](https://www.nexusmods.com/skyrimspecialedition/mods/120946) by hakasapl -
  AutoSeasons reuses and adapts its mesh/texture/plugin-scanning core.
- [nifly](https://github.com/ousnius/nifly) by ousnius, for NIF file handling.
- [Mutagen](https://github.com/Mutagen-Modding/Mutagen) for reading/writing Bethesda plugin files.
- [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861) by powerof3, the
  SKSE plugin this tool generates data for.

---

## Permissions and credits (Nexus's dedicated permissions fields)

Given AutoSeasons is GPLv3 (derived from the GPLv3-licensed PGPatcher), the permissions should say
something like:

```
This mod is licensed under GPLv3. Source code: [GitHub link]. You are free to modify and
redistribute it under the same license (see the LICENSE file in the repository for the exact
terms) - please credit the original project and link back to the source.
```

Nexus's own permission checkboxes (upload modified files, convert to other games, use assets in
other mods, etc.) should generally all be set to "Yes"/permissive to match GPLv3's own terms - a
proprietary "no derivatives without asking" setting would actually conflict with the license.

---

## Still needed before publishing

- [ ] Screenshots: the launcher GUI, the "Manage Season Mod Conflicts" dialog, and ideally a
  before/after in-game comparison of a seasonal swap.
- [ ] Your GitHub repo link (once public) for the description's Credits/source-code references.
- [ ] Decide the exact version number to list (matches whatever you tag the GitHub release as).
- [ ] AutoBlend's Nexus page link, once it's up, for the "Companion mod" section above.
