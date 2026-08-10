# Changelog

All notable changes to this project are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- Initial standalone release, split out from a PGPatcher fork into its own tool.
- Seasonal texture-variant detection and duplicate-record generation for Static, Activator,
  Furniture, MovableStatic, Tree, Flora, and LandscapeTexture records.
- Recognition of multiple seasonal filename conventions per season (e.g. `_WIN`/`_winter`,
  `_AUT`/`_autumn`/`_fall`), not just Seasons of Skyrim's own short codes.
- PBR-folder texture variant detection and matching PBRNIFPatcher json rule authoring.
- Optional TerrainHelper integration (landscape parallax), auto-detected and never required.
- Seasonal grass mesh detection and duplication, riding along with the landscape-texture swap;
  grass with no detected winter variant is dropped in winter by default
  (`removeGrassInWinter` in `AutoSeasons_config.json`).
- Skips records already covered by another mod's own `Data/Seasons` ini, avoiding redundant
  duplicate records.
- User-editable mesh blocklist and season-locked EditorID keyword list (both via the GUI), now
  shipping with sensible built-in defaults (dungeon/interior mesh paths; `coast`/`river`/`cave`/
  `dead` EditorID keywords) instead of empty lists.
- Compact progress UI with a post-run reminder to enable the generated plugin.
- Localized GUI: English, French, Spanish, German, Portuguese (Brazil), Italian.
- CLI mode for scripted/automated runs, mirroring all GUI options.
- Redesigned GUI: header banner and accent colors giving AutoSeasons its own visual identity
  (distinct from the PGPatcher lineage it's derived from), a tabbed General/Options layout, and a
  working Light/Dark/System theme toggle (implemented via a full process respawn on change, to
  work around a wxWidgets/MSW dark-mode limitation where the underlying API can only be enabled
  once per process).
- Auto-clears the previous run's output before each generation, matching PGPatcher's behavior.
- One-click close on the progress window when a run finishes successfully.
- Detects and reuses a mod author's own pre-existing seasonal grass record (matched by EditorID,
  including short seasonal suffixes like `_Sn`/`_Sp`/`_Su`/`_Au` used by widely-depended-on grass
  packs) instead of creating a redundant duplicate on top of it.
- Refuses to run if AutoSeasons' own previous output mod is still active in the merged Data view
  (checked across every generated file, not just the plugin), matching PGPatcher's own guard
  against a tool scanning its own prior output as if it were a real mod.
- Creates a `DefaultPBRLand` TextureSet record when generating a PBR landscape variant and no mod
  in the load order already provides one - the record Community Shaders itself reads as its PBR
  terrain fallback, entirely separate from TerrainHelper's own `LandscapeDefault` (which only
  matters for the unrelated vanilla-parallax unlock).
- PBR detection also recognizes textures placed directly under `textures\pbr\...`, not only
  records whose PBRNifPatcher rule folder structure was already inspected.

### Changed
- Foreign `Data\Seasons\*.ini` files are now folded into one merged, authoritative AIO ini per
  season rather than left scattered for Seasons of Skyrim's alphabetical-priority system to sort
  out, with the originals neutralized via an empty override.
- PBRNifPatcher rule matching now uses an index built once per run instead of a linear scan per
  shape/season - fixes a ~2x generation-time regression observed on a modlist shipping 3500+ PBR
  rules.

### Fixed
- Seasons of Skyrim's own `MainFormSwap_*.ini` (its auto-generated, always-regenerated scan cache)
  is no longer read as if it were a stable mod-authored ini - once AutoSeasons had run at least
  once, this file could reference AutoSeasons.esp's own FormIDs from a previous run, polluting the
  next AIO ini with dead references.
- A record whose `AlternateTextures` only overrides some of its shapes no longer silently skips
  seasonal duplication for the shapes it doesn't override (which still use the NIF's own embedded
  textures and can have a valid seasonal sibling of their own).
- Fixed a data race on the internal file map (unlocked reads racing locked writers) that could
  surface as an intermittent crash or a spurious "File map was not populated" error under the
  multithreaded mesh-scanning pipeline.
- Fixed the native/managed exception-reporting bridge: the last-reported .NET exception was never
  cleared after being read, which meant any transient error would incorrectly poison every
  subsequent call for the rest of the run.
- Fixed incorrect decoding of mod names, EditorIDs, and texture paths containing non-ASCII
  characters (accented Latin, Cyrillic, CJK) crossing the native/managed boundary - they were
  being byte-widened instead of properly UTF-8 decoded.
- Fixed a config-load bug where the mesh blocklist's entries could duplicate on top of its own
  built-in defaults instead of replacing them.
- Fixed several smaller native/managed boundary issues: an allocator/deallocator mismatch on
  batch-transfer buffers, a memory leak on one buffer-verification failure path, and an unindexed
  per-call scan when looking up a mod's own pre-existing seasonal grass record by EditorID.
