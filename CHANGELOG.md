# Changelog

All notable changes to this project are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [1.2.2] - 2026-08-27

### Changed
- Generalized the intro text's PGPatcher reminder (added in 1.2.1) to cover Complex Material, not
  just PBR - PGPatcher converts meshes to either downstream, and AutoSeasons' seasonal duplicates
  need to already exist for either conversion to act on them.

## [1.2.1] - 2026-08-27

### Changed
- The launcher's intro text now tells users to run AutoSeasons before PGPatcher, not after -
  reinforcing the already-documented correct order directly in the app, matching AutoBlend's own
  equivalent PG Patcher reminder.

## [1.2.0] - 2026-08-27

### Added
- "Move to Top" / "Move to Bottom" buttons in the "Manage Season Mod Conflicts" list, alongside the
  existing Move Up/Down - reordering across a load order with 50+ foreign seasonal mods no longer
  takes dozens of individual clicks to reach either end.

## [1.1.2] - 2026-08-25

### Fixed
- A foreign `Data/Seasons` ini saved with a UTF-8 byte-order mark had its whole content silently
  discarded - the BOM shifted the first line's leading `[` by 3 bytes, so the very first section
  header was never recognized, and every entry after it (having no recognized section to belong
  to) was skipped. The mod never appeared in "Manage Season Mod Conflicts" and its own coverage was
  never respected, indistinguishable from the ini not existing at all.

## [1.1.1] - 2026-08-25

### Fixed
- GPU device creation and internal compute shader initialization now log the actual underlying
  error (HRESULT message, or the HLSL compiler's own diagnostic text) instead of just failing with
  a generic "Failed to initialize internal shaders"/"Failed to initialize GPU" message and no way
  to tell why - the log now shows whether it's e.g. a missing `d3dcompiler_47.dll`, a missing
  `AutoSeasons_cshaders` folder, or a genuine driver incompatibility.

## [1.1.0] - 2026-08-20

### Fixed
- A 0-byte/unreadable mesh file (a broken file shipped by another mod) aborted the entire
  generation run with an unhandled "File is empty" error instead of just skipping that one mesh -
  now caught, logged as a warning, and the run continues.
- PBR-only load orders could pull in `TerrainHelper.esp` as an unnecessary plugin master purely
  because a PBR landscape texture has its own native displacement map - `TerrainHelper.esp` is now
  only referenced when actually patching a *vanilla* landscape texture's parallax slot.
- `DefaultPBRLand` now also gets a matching `PBRTextureSets` config cloned from its source material
  when AutoSeasons creates the record itself, instead of only the bare TextureSet.
- A PBR rule that omits its normal map or RMAOS slot now falls back to the conventionally-named
  sibling file on disk (`..._n.dds`/`..._rmaos.dds`) if one genuinely exists, instead of leaving the
  slot empty.
- Foreign `Data/Seasons` conflict resolution now genuinely respects the configured override/priority
  settings for grass-borrowing and coverage-skip decisions across every foreign mod involved,
  instead of just the most-recently-scanned one winning.
- 24 GUI strings (the Season Mod Conflicts dialog, Config Profile section, remove-grass-in-winter/
  dry-run labels, the MO2/Vortex warning, and the completion message variants) were missing from
  every translation file, including the English one, and always fell back to their English default
  regardless of the selected language. Also fixed a stale completion message left over from before
  it gained a per-record-type breakdown.

### Changed
- Simplified the "Manage Season Mod Conflicts" dialog to a single reorderable list (respect/
  overwrite a foreign mod, with drag-to-reorder priority), removing the separate per-record-type
  customization table - the engine still supports per-type overrides via
  `AutoSeasons_config.json` for anyone who wants that granularity, just without a dedicated second
  table cluttering the everyday case.

### Removed
- Dead `PGModManager` mod-attribution code (inherited from PGPatcher, never actually wired up -
  AutoSeasons always relied on the mod manager's own virtual file system for active-mod filtering).

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
