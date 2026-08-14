# Troubleshooting

## "Generation complete, but 0 seasonal records were created"

AutoSeasons ran successfully but found nothing to do — almost always a configuration issue, not a
bug:

1. **Most common cause: AutoSeasons was launched by double-clicking the exe instead of through your
   mod manager.** See [Usage Guide](Usage-Guide.md#-run-it-through-your-mod-manager-not-by-double-clicking-the-exe) —
   if it only sees your game's real Data folder, it never sees any of your installed mods' seasonal
   textures either.
2. Double-check the **Game Location** actually points at a folder containing `Data\` (or your mod
   manager's merged view of it), and that mods that ship seasonal textures are actually enabled.
3. Check the **Seasonal Variation Blocklist** / **Season-Locked EditorID Keywords** aren't
   accidentally excluding everything (e.g. a stray `*` pattern).
4. Try a **Preview only (dry run)** pass and read the log — every skip is logged.

## "Why didn't texture/record X get a seasonal variant?"

The completion message and log always show a per-type breakdown (e.g. `STAT: 736, LTEX: 46, ...`),
but for a specific record, the most direct way to find out is to run with verbose logging and search
the log file for that record's EditorID or texture filename:

```
AutoSeasons.exe <game-dir> <output> -v
```

(In the GUI, the log file at `<output>/AutoSeasons.log` always includes this level of detail
regardless of what's shown in the compact status panel — open it in a text editor and search.)

Search for the EditorID (e.g. `rock01_snow01`) or the texture path (e.g. `rock01.dds`). Every skip
reason is logged next to it, for example:

- `skipped: EditorID matches a season-locked keyword` — it matched `coast`/`river`/`cave`/`dead` (or
  a custom keyword you added). Remove the keyword from your config if this record genuinely should
  be seasonal.
- `skipped: no shapes with a usable texture` — the record has no `AlternateTextures` override and
  its mesh couldn't be found anywhere in the merged Data view. This can happen if a PBR/Complex
  Material patcher only outputs meshes it actually converts, leaving an untouched vanilla mesh
  unresolvable.
- `no seasonal sibling found for diffuse=...` — the exact texture path AutoSeasons looked for a
  `_win`/`_spr`/`_sum`/`_aut` sibling of, and didn't find one. This is the single most common
  reason: **the seasonal texture file genuinely doesn't exist yet** for that specific texture/season
  combination, even if other seasons or other nearby textures do have one. Check
  [Naming Conventions](Naming-Conventions.md) for the exact filename AutoSeasons expects.
- `skipped: already covered by foreign mod "X"` — another mod's own `Data/Seasons` declaration
  already claims this record for this season; see [Compatibility](Compatibility.md) for how to
  override or reprioritize this.

## "Two mods are fighting over the same record"

If the log shows `Multiple foreign mods cover the same ... record with no priority order set
between them`, open **Manage Season Mod Conflicts** in the launcher and set an explicit priority
(or override) between the mods named in the warning — see
[Compatibility](Compatibility.md#other-mods-that-ship-their-own-dataseasons-declarations).

## "AutoSeasons' own output mod is still enabled in your mod manager"

AutoSeasons refuses to run while its own previous output is active in the merged Data view (scanning
its own output as if it were a real mod produces wrong results and slows the scan down a lot).
Disable the output mod (e.g. "AutoSeasons Output" in MO2), rerun, then re-enable it once the run has
finished.

## Nothing changes in-game even though the log shows records were created

- Make sure `AutoSeasons.esp` is **enabled** in your plugin list.
- Make sure the output mod is **enabled** in your mod manager so its files are part of the merged
  Data view.
- Make sure [Seasons of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/62861) is
  installed and running — AutoSeasons only generates data, it never touches the running game.
- LOD (distant objects/trees) won't reflect the season unless DynDOLOD/xLODGen were run *after*
  AutoSeasons with seasonal identifiers — see [Compatibility](Compatibility.md#dyndolod--xlodgen-lod).
