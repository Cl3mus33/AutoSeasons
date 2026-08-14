# Draft Nexus Mods page for AutoBlend

Copy/paste each section into the corresponding field on Nexus's "Add file"/"Edit mod" pages. Fill
in the bracketed placeholders (screenshots, your own links) before publishing. Based on
`Cl3mus33/AutoBlend`'s README - update this draft if the tool's behavior changes before release.

---

## Title

```
AutoBlend
```

## Summary (short teaser, shown in search results and mod lists - keep it under ~200 characters)

```
Scans your load order for landscape texture variants and patches the meshes that use them to
alpha-blend instead of alpha-test - the fix mods like Vanaheimr ship by hand, generalized.
```

## Category

**Utilities for Modders**

## Suggested tags

`Utilities for Modders`, `Terrain`, `Landscaping`, `Models/Meshes`, `Textures`, `Automation`

## Requirements

```
None strictly required for the core patch - AutoBlend works standalone.
```

Strongly recommended:
```
AutoSeasons - run BEFORE AutoBlend in your pipeline (see "Pipeline order" below - this matters).
```

---

## Description

### About this mod

Vanilla Skyrim's landscape texture blending uses alpha-testing (a hard on/off cutout) rather than
true alpha-blending, which is why several high-quality landscape texture mods (Vanaheimr among
them) ship hand-patched meshes just to get a smooth blend for their own textures. AutoBlend
generalizes that fix to your **whole load order**: point it at your merged Data folder and it finds
every mesh using a texture that signals "I want blending" (via the `statics`/`blending` subfolder
convention several texture mods already use), patches the mesh's `NiAlphaProperty` from
alpha-testing to alpha-blending, and generates the plugin overrides to make it stick - all offline,
no manual xEdit/mesh editing required.

AutoBlend is also mod-order-aware when it patches: instead of blindly reverting a shape back to its
vanilla texture set, it derives its output from whichever texture set that shape already carries
after your other texture mods have applied - so a shape another mod has already retextured keeps
that mod's own normal/other maps, and only the alpha-blend fix gets added on top.

### ⚠️ Pipeline order: run AutoSeasons first, then AutoBlend

**This order matters.** If you also use [AutoSeasons](#) *(link once public)*, always run
AutoSeasons **before** AutoBlend in your generation pipeline. AutoBlend derives its output from
whichever texture set a shape *currently* carries (preferring an already-retextured mod's own set
over vanilla) - running it after AutoSeasons means it builds on top of AutoSeasons' own seasonal
duplicates correctly; running it first (or re-running AutoSeasons afterward) can leave AutoBlend
working from a state that no longer matches what's actually active in your load order. Recommended
order: **AutoSeasons → AutoBlend → PGPatcher (if used) → xLODGen/DynDOLOD (if used)**.

### Features

- Scans every `*/landscape/` texture path (vanilla, DLC, and mod-added) for a `statics` or
  `blending` subfolder - the convention several landscape texture mods already use to mark "this
  texture wants alpha blending, not alpha testing".
- Finds every mesh that references a matching texture, both via loaded plugins' Alternate Textures
  and by scanning meshes directly for their baked-in diffuse path.
- Bakes the detected diffuse texture path into each mesh's embedded texture slot and flips its
  `NiAlphaProperty` to alpha-blending, without touching any other flag or restructuring the mesh.
- Generates a dedicated output plugin with derived TextureSets and Alternate Texture overrides,
  deriving from whichever mod's texture set a shape already carries (rather than vanilla) when one
  exists - so a shape another mod has already retextured keeps that mod's normal/other maps.
- Two interchangeable shells (native wxWidgets, and a WPF desktop app) sharing the same settings
  file and the same underlying Mutagen/niflysharp patch engine - use whichever fits your workflow.
- Runs entirely offline against your files - it never touches the running game.

### ⚠️ Run it through your mod manager, not by double-clicking the exe

Same reasoning as AutoSeasons: if you use Mod Organizer 2, the executable needs to be inside a mod
folder MO2 already knows about and launched through MO2's tool list - a plain, unregistered folder
won't participate in MO2's virtual filesystem merge, which AutoBlend's own runtime library
resolution depends on. Double-clicking the exe directly, or running it from an unregistered folder,
will not see your installed mods correctly.

### Installation

1. Install like any other mod (MO2: as a regular mod; Vortex: extract into a mod folder).
2. Run it through your mod manager's tool list (see warning above).
3. Point it at your game install/merged load order and an output folder, run the patch.
4. Enable the generated output plugin in your mod manager's plugin list.
5. If you also use AutoSeasons, make sure you ran it **before** AutoBlend - see above.

### Compatibility

- **AutoSeasons**: companion tool, same author, same architecture. Run AutoSeasons first (see the
  pipeline-order warning above).
- **PGPatcher**: run AutoBlend before PGPatcher if you use PBR/Complex Material, so PGPatcher's own
  conversion pass sees AutoBlend's already-patched meshes.
- Landscape texture mods that already ship their own hand-blended meshes (e.g. Vanaheimr) are
  unaffected where they've already done this work - AutoBlend only touches meshes that still need
  the fix.

### Credits

- Native shell architecture (wxWidgets + DNNE-bridged .NET patch backend) adapted from
  [AutoSeasons](#) *(link once public)*, itself built on
  [PGPatcher](https://www.nexusmods.com/skyrimspecialedition/mods/120946) by hakasapl.
- [nifly](https://github.com/ousnius/nifly) by ousnius, and its C# binding
  [niflysharp](https://github.com/Aetherinox/niflysharp), for NIF file handling.
- [Mutagen](https://github.com/Mutagen-Modding/Mutagen) for reading and writing Bethesda plugin
  files.

---

## Permissions and credits (Nexus's dedicated permissions fields)

Same as AutoSeasons - GPLv3, derived from the same GPLv3-licensed lineage (AutoSeasons →
PGPatcher):

```
This mod is licensed under GPLv3. Source code: [GitHub link]. You are free to modify and
redistribute it under the same license (see the LICENSE file in the repository for the exact
terms) - please credit the original project and link back to the source.
```

Set Nexus's permission checkboxes (upload modified files, convert to other games, use assets in
other mods, etc.) permissively to match GPLv3's own terms.

---

## Still needed before publishing

- [ ] Screenshots: the GUI (native and/or WPF shell), and ideally a before/after showing the
  alpha-blend fix on a landscape seam.
- [ ] Your GitHub repo link (once public) for the Credits/source-code references.
- [ ] AutoSeasons' Nexus link, once it's up, for the pipeline-order warning and Credits above.
- [ ] Decide the exact version number to list.
- [ ] Confirm whether to publish AutoSeasons and AutoBlend's pages at the exact same time, or one
  first then the other with the cross-links added right after (avoids a temporary broken "(link
  once public)" placeholder being visible to early visitors on either page).
