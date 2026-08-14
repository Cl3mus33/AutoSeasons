# Usage Guide

## Requirements

- Skyrim SE (Steam or GOG), Skyrim VR, or Enderal SE.
- [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861) — the SKSE plugin
  that actually reads AutoSeasons' output and performs the swap in-game. AutoSeasons only
  *generates the data*; without Seasons of Skyrim installed, nothing changes when you play.
- A mod manager (Mod Organizer 2 or Vortex) is strongly recommended, or a plain merged Data folder.

## Installation

Install AutoSeasons like any other mod: as a regular mod entry in MO2, or extracted into a mod
folder for Vortex.

## ⚠️ Run it through your mod manager, not by double-clicking the exe

This is the single most common way a first run silently produces a wrong or incomplete result.

If you use **Mod Organizer 2**: add AutoSeasons to MO2's executables list (or use the mod's own
entry if it's registered automatically) and launch it **from inside MO2**, the same way you'd
launch xEdit or any other tool that needs to see your installed mods. MO2 uses a virtual file
system (USVFS) that only applies to processes it launches itself — if you instead double-click
`AutoSeasons.exe` straight from Explorer, it only sees your game's *real, unmodified* Data folder,
not your installed mods. The scan will still "succeed" and produce output — just against the wrong
files, with no built-in way to tell the difference from a genuine run.

If you use **Vortex**: launch AutoSeasons from Vortex's dashboard/tool list the same way.

## Running AutoSeasons

1. **Game Location**: your game's install directory (the folder containing `Data\`), or your mod
   manager's merged view of it.
2. **Output Location**: a folder AutoSeasons will write its generated plugin/inis into — this
   becomes its own mod entry in your manager (e.g. an "AutoSeasons Output" mod). AutoSeasons clears
   and regenerates its own previous output here on every run; it never touches anything outside
   this folder except reading from your Data folder.
3. **Seasonal Variation Blocklist** / **Season-Locked EditorID Keywords**: usually fine at their
   defaults (interior/dungeon meshes, `coast`/`river`/`cave`/`dead`-named records are excluded by
   default since they're never meant to change with the season). See the tooltips in the launcher
   for the exact matching rules.
4. **Manage Season Mod Conflicts**: only relevant if you have other mods that ship their own
   `Data/Seasons` declarations (see [Compatibility](Compatibility.md)) — lets you decide who wins
   when two mods disagree about the same record.
5. **Preview only (dry run)**: scans and logs what *would* be created/skipped without writing
   anything to your output folder — useful for safely trying out blocklist changes.
6. Click **Start Patching**. When it finishes, the completion message shows how many seasonal
   records were actually created (broken down by type) — if it says 0, something is likely
   misconfigured (wrong game directory, or an overly aggressive blocklist); see
   [Troubleshooting](Troubleshooting.md).

## After running

1. Enable the generated `AutoSeasons.esp` in your mod manager's plugin list, if it isn't already
   active.
2. Make sure the "AutoSeasons Output" mod (or whatever you named the output folder's mod entry) is
   **enabled** in your manager so its files are actually part of the merged Data view.
3. If you're regenerating after a load-order change, make sure that mod is **disabled** in your
   manager while AutoSeasons runs (it warns you if it detects its own previous output still active
   in the merged view, since scanning its own output as if it were a real mod produces wrong
   results).
4. Install and run [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861)
   if you haven't already — that's what reads the `Data/Seasons/*.ini` AutoSeasons wrote and
   performs the actual in-game swap.

## PBR / Complex Material users

If AutoSeasons detects a seasonal texture living under a `textures\pbr\...` path, it authors a
matching PBRNIFPatcher rule automatically so a downstream [PGPatcher](https://github.com/hakasapl/PGPatcher)
run picks up and converts the seasonal duplicate correctly. Run PGPatcher *after* AutoSeasons in
your pipeline, same as you would for any other new PBR texture.

## Command-line / automation

AutoSeasons has a full CLI mode for scripting — run it with any argument (e.g.
`AutoSeasons.exe --help`) to see all options, including `--blocklist`, `--editor-id-blocklist`,
`--override-season-mods`, `--mod-priority`, and `--dry-run`.
