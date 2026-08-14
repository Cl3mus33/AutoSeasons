# Naming Conventions (for mod authors)

If your mod already ships seasonal texture or mesh variants under any of the naming schemes below,
AutoSeasons will detect and use them automatically — your users don't need to configure anything.

## Season codes and aliases

AutoSeasons recognizes Seasons of Skyrim's own two-letter suffix convention (`_WIN`/`_SPR`/`_SUM`/
`_AUT`), plus several common alternate spellings other texture packs already use in the wild —
checked in addition to, never instead of, the canonical suffix:

| Season | Canonical | Also recognized |
|---|---|---|
| Winter | `_WIN` | `_winter`, `_sn` |
| Spring | `_SPR` | `_spring`, `_sp` |
| Summer | `_SUM` | `_summer`, `_su` |
| Autumn | `_AUT` | `_autumn`, `_fall`, `_au` |

Matching is case-insensitive. The suffix goes at the very end of the filename, before the
extension: `rock01_AUT.dds`, `rock01_fall.dds`, and `rock01_autumn.dds` are all recognized as an
autumn variant of `rock01.dds`.

## Textures (STAT/ACTI/FURN/MSTT/TREE/FLOR + LTEX)

Ship a season-suffixed sibling of the diffuse texture next to the original:

```
textures\landscape\rock01.dds        <- base (year-round/no swap needed for this season)
textures\landscape\rock01_aut.dds    <- autumn variant
textures\landscape\rock01_win.dds    <- winter variant
```

You don't need to provide all four seasons — AutoSeasons only generates a duplicate for the seasons
where a sibling actually exists on disk. A normal map sibling (`_n`) is picked up the same way if
present (`rock01_aut_n.dds`); it's optional.

This works both for a texture painted directly via a record's `AlternateTextures` override (Static,
Activator, Furniture, MovableStatic, Tree, Flora) and for a landscape texture's (LTEX) own diffuse.

## PBR / Complex Material textures

Same convention, just under the `textures\pbr\...` subfolder instead of `textures\...`. AutoSeasons
detects these via [PGPatcher](https://github.com/hakasapl/PGPatcher)'s own PBRNifPatcher rules (not
just by folder path), so a generic PBR texture pack that ships the files/rule for a texture without
having rewritten every record's own AlternateTextures/TXST to point there is still picked up.

## Grass meshes

Grass doesn't swap via a texture override — Seasons of Skyrim swaps the whole `.nif`. Ship a
season-suffixed sibling mesh:

```
meshes\landscape\grass\grass01.nif        <- base
meshes\landscape\grass\grass01_aut.nif    <- autumn variant (different mesh/baked-in texture)
```

Same season aliases apply (`_aut`/`_autumn`/`_fall`/`_au`, etc.).

## Landscape texture parallax (TerrainHelper users only)

If [TerrainHelper](https://github.com/hakasapl/TerrainHelper) is in the load order, AutoSeasons also
looks for a season-suffixed parallax sibling of an LTEX's diffuse (`_p` slot) — e.g.
`dirt02_aut_p.dds`. No fallback to the non-seasonal parallax map: if there's no seasonal height map,
that slot is simply left unset for that season rather than reusing a possibly-mismatched one.

## What NOT to name this way

Records whose EditorID contains `coast`, `river`, `cave`, or `dead` are skipped by seasonal
variation entirely by default (user-configurable in the launcher) — these are assumed to be
intentionally season-invariant. If your mod adds a genuinely seasonal coastal/river-adjacent object
that SHOULD swap, your users can remove that keyword from their own config, or you can pick an
EditorID that doesn't contain it.
