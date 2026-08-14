# Compatibility

## PGPatcher

AutoSeasons is built on and reuses [PGPatcher](https://github.com/hakasapl/PGPatcher)'s own
mesh/texture/plugin-scanning core, and authors PBRNIFPatcher rules for any seasonal PBR texture it
finds. Run PGPatcher **after** AutoSeasons — it needs to see AutoSeasons' generated
`AutoSeasons.esp` and the PBR rules AutoSeasons wrote in order to convert the seasonal duplicates
correctly.

## TerrainHelper

Optional. If [TerrainHelper](https://github.com/hakasapl/TerrainHelper) is in your load order,
AutoSeasons also patches its `LandscapeDefault` record with a seasonal parallax height map when one
exists on disk, and looks for `_p` (parallax) siblings of LTEX diffuse textures. No effect if
TerrainHelper isn't installed — the core diffuse/normal season swap works without it.

## Community Shaders (PBRTextureSets)

AutoSeasons authors a `PBRTextureSets` config (cloned from the base texture's own config, if one
exists) for every seasonal LTEX duplicate whose base TextureSet is PBR-designated — Community
Shaders reads these directly at runtime, no PGPatcher step needed for this particular file type.

## DynDOLOD / xLODGen (LOD)

[DynDOLOD](https://dyndolod.info) has its own, separate seasonal LOD generation system, and it's
designed to read exactly the files AutoSeasons produces:

- DynDOLOD reads `po3_SeasonsOfSkyrim.ini` (Seasons of Skyrim's own settings file) to know which
  record types (Statics, Trees, Activators, Furniture, MovableStatics) are enabled for seasonal
  swapping.
- DynDOLOD reads the **same `Data\Seasons\*[SPR|SUM|AUT|WIN].ini` files** AutoSeasons writes (the
  merged `z_AIO_AutoSeasons_<SEASON>.ini`), and generates matching seasonal LOD billboards named
  `*.[SPR|SUM|AUT|WIN].[BTO|BTT|LST|DDS]`.

**Recommended pipeline order:**

1. Run AutoSeasons (writes `Data/Seasons/*.ini`).
2. Run PGPatcher if you use PBR/Complex Material.
3. Run xLODGen for terrain LOD (with seasonal identifiers — DynDOLOD's own docs cover the exact
   xLODGen settings for this).
4. Run DynDOLOD last, so it can see both AutoSeasons' `Data/Seasons/*.ini` and the terrain LOD
   xLODGen produced.

Object/tree LOD generated this way genuinely reflects the season; terrain LOD does too, as long as
xLODGen was run with seasonal output *before* DynDOLOD. Skipping the xLODGen step (or running
DynDOLOD before AutoSeasons) is the most likely reason someone would see a seam between correctly
swapped near-scale objects and unchanged terrain/object LOD at distance — this isn't an AutoSeasons
limitation, it's a pipeline-order issue, and DynDOLOD's own [Seasons help page](https://dyndolod.info/Help/Seasons)
is the authoritative reference if something doesn't look right at LOD range.

## Other mods that ship their own `Data/Seasons` declarations

Some mods (Turn of the Seasons, Nature of the Wild Lands, Simply Dirt Roads, Ivy Replacer, and
others) ship their own hand-authored `Data/Seasons/<ModName>_<SEASON>.ini` files, either using
Seasons of Skyrim's own `0x<FormID>~Plugin.esp|0x<FormID>~Plugin.esp` syntax or a bare
`<EditorID>|<EditorID>` pair format. AutoSeasons detects every such file and folds its content
verbatim into one merged `z_AIO_AutoSeasons_<SEASON>.ini`, writing a neutralizing empty override for
each original so nothing is evaluated twice. By default, AutoSeasons always defers to another mod's
own declaration for a record it already covers — it won't generate a redundant or conflicting
duplicate on top of one.

Two situations you may want to adjust via the launcher's **Manage Season Mod Conflicts** dialog:

- **You want AutoSeasons' own texture instead of a specific mod's swap.** Check that mod under
  "Override" — AutoSeasons will then generate its own seasonal duplicate for any record that mod
  covers, wherever it has matching art, exactly as if that mod's declaration didn't exist.
- **Two different foreign mods both cover the same record, and you want a specific one to win.**
  Set a priority order (global, or per record type — e.g. prefer a different mod for trees than for
  everything else). The mod lower in the list wins, matching Mod Organizer's own left-pane priority
  convention. If this isn't configured and a genuine conflict exists, AutoSeasons logs a warning
  naming the conflicting mods and points you at this dialog — today's winner in that case is
  whichever mod's file the disk scan happened to visit last, which isn't predictable without
  reading the log.

A special case: if a foreign mod's own declaration for a record turns out to be **grass-only**
(it keeps the exact same base landscape texture, only changing which grass types are drawn),
AutoSeasons still generates its own seasonal texture for that record while reusing that mod's own
curated grass list — so you get both your own seasonal texture and that mod's deliberately-chosen
flora, without needing to configure anything.
