# FAQ

## Does AutoSeasons work with Skyrim VR?

AutoSeasons itself can scan a VR install and generate the same `Data/Seasons/*.ini` files - but
you need the right build of Seasons of Skyrim installed to actually see the swap in-game:

- The regular [Seasons of Skyrim SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/62861)
  page only lists the SE-specific Address Library as a requirement, so it does **not** run on
  Skyrim VR.
- Instead, install [Seasons of Skyrim SKSEVR](https://www.nexusmods.com/skyrimspecialedition/mods/63593),
  a community VR port built from the same source - same ini format, just a different SKSE build.

## Does AutoSeasons work with Enderal Special Edition?

Untested - we genuinely don't know. AutoSeasons offers Enderal SE as a selectable game type
(scanning it should work the same way as any other Skyrim SE-based install), but neither
AutoSeasons nor Seasons of Skyrim has been confirmed to work correctly against Enderal's own
heavily-modified world and `Skyrim.esm`. If you try this combination, please report back (see the
mod page's Posts/Bugs tabs) - a confirmed report either way is more useful than us guessing.

## Do I need to run AutoSeasons through my mod manager?

Yes - see the [Usage Guide](Usage-Guide.md). Double-clicking the exe from Explorer only sees your
game's real, unmodified `Data` folder, not your installed mods, and silently produces an incomplete
result.

## Why does AutoSeasons say "0 records created"?

See [Troubleshooting](Troubleshooting.md) - usually means no seasonal texture/mesh variants were
found on disk for anything in your load order, or your game location/output folder is misconfigured.
