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
- User-editable mesh blocklist and season-locked EditorID keyword list (both via the GUI).
- Compact progress UI with a post-run reminder to enable the generated plugin.
- Localized GUI: English, French, Spanish, German, Portuguese (Brazil), Italian.
- CLI mode for scripted/automated runs, mirroring all GUI options.
