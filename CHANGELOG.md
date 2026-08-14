# Changelog

All notable changes to this project are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [1.0.0] - 2026-08-14

Initial public release. AutoSeasons scans your Skyrim SE load order for seasonal texture and mesh
variants and automatically generates the plugin and `Data/Seasons/*.ini` files that
[Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861) reads to swap them
in-game - no manual xEdit/CK work required.

### Seasonal detection and generation
- Scans Static, Activator, Furniture, MovableStatic, Tree, Flora, and LandscapeTexture records for
  a seasonal sibling texture on disk.
- Recognizes multiple common seasonal filename conventions per season (e.g. `_WIN`/`_winter`,
  `_AUT`/`_autumn`/`_fall`), not just Seasons of Skyrim's own short codes - works with texture packs
  that ship their own naming.
- Detects and duplicates seasonal grass meshes alongside the landscape texture swap. Grass with no
  detected winter variant is dropped in winter by default (configurable in
  `AutoSeasons_config.json`), rather than looking out of place under snow.
- If a mod author already ships their own seasonal grass record (matched by EditorID, including
  common short seasonal suffixes like `_Sn`/`_Sp`/`_Su`/`_Au`), AutoSeasons reuses it instead of
  creating a redundant duplicate on top of it.
- A record whose `AlternateTextures` only overrides some of its shapes still gets seasonal
  duplicates for the shapes it doesn't override.

### PBR / Complex Material support
- Detects textures under `textures\pbr\...` and authors matching PBRNIFPatcher rules for a
  downstream [PGPatcher](https://www.nexusmods.com/skyrimspecialedition/mods/120946) run, plus
  PBRTextureSets configs for Community Shaders.
- Creates a `DefaultPBRLand` TextureSet record when generating a PBR landscape variant and no mod
  in the load order already provides one.
- Optional [TerrainHelper](https://www.nexusmods.com/skyrimspecialedition/mods/143149) integration
  for landscape parallax - entirely optional, auto-detected, never required.

### Compatibility with other seasonal mods
- Detects other mods' own `Data/Seasons` declarations and folds them into one merged, authoritative
  ini instead of leaving Seasons of Skyrim to sort out several separate files.
- Respects another mod's own seasonal coverage by default (no redundant duplicate records), with a
  "Manage Season Mod Conflicts" launcher dialog to override specific mods (globally or per record
  type) when you want AutoSeasons' own textures to win instead.
- Lets you set an explicit priority order for when two foreign mods both declare a swap for the
  same record, and warns (in the log and completion message) whenever such a conflict is left
  unresolved.
- When a foreign mod's own ini covers a landscape texture for grass only, AutoSeasons still
  generates its own seasonal texture duplicate and reuses that mod's curated grass list instead of
  skipping the record outright.

### Interface and workflow
- Explicit in-launcher warning about running AutoSeasons through Mod Organizer 2/Vortex's tool list
  rather than double-clicking the exe directly - the most common cause of a silently incomplete
  scan.
- User-editable mesh blocklist and season-locked EditorID keyword list, shipping with sensible
  built-in defaults (dungeon/interior mesh paths; `coast`/`river`/`cave`/`dead` EditorID keywords).
- Load Config / Save Config As: save and reload distinct settings profiles instead of being tied to
  a single `AutoSeasons_config.json` next to the exe - useful if you run AutoSeasons for more than
  one modlist from a shared install.
- `--dry-run` / "Preview only" mode: runs the full scan and decision pass and logs what would be
  created without writing anything, so blocklist/keyword changes can be tried safely first.
- Completion message shows the actual per-record-type breakdown instead of a generic "Generation
  complete" message.
- Per-skip debug diagnostics throughout the scan (season-locked EditorID, foreign coverage, no
  seasonal sibling found, etc.), always captured to the log file - answers "why didn't record X get
  a seasonal duplicate" by checking the log.
- Localized GUI: English, French, Spanish, German, Portuguese (Brazil), Italian. Light/Dark/System
  theme support.
- Full CLI mode mirroring all GUI options, for scripted/automated runs.
- Refuses to run if AutoSeasons' own previous output mod is still active in the merged Data view,
  to avoid scanning its own prior output as if it were a real mod.
