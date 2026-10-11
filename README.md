# Grungify

Procedural damage and weathering for Unreal Engine 5. Drop a **BP_WeatheringZone** into a level, size its box, and press **Preview**, **Apply** or **Clear**. The zone finds every static mesh it overlaps and bakes four kinds of wear into them, each following how the space is built and used:

| Layer | Where it goes | Vertex channel |
|---|---|---|
| **Edge chips** | Convex, exposed edges. More on edges facing into the wear direction, and on edges near the floor that get kicked. | R |
| **Dust at feet** | Builds up from the floor (traced, so it works across storeys and stairs), on ledges, and in creases. | G |
| **Runoff below vents** | Streaks running down from vents, drains and spouts, fanning out as they fall. Faint streaks on all walls too. | B |
| **Handling on rails** | Polish/grime on rails and handles, plus a lighter band on edges at hand height. | A |

Each material slot is classified as **stone**, **metal** or **trim**, so the wear follows the construction. Trim chips most, metal rusts under runoff and gets polished by hands, and stone collects dust.

Everything is driven by a **Seed**, so the same seed and settings always produce the same result.

> **Status:** v0.2.1 targets UE 5.8. This revision fixes build compatibility, state restoration, overlap ownership and material activation. Native verification results are recorded below; visual quality still needs review on the intended meshes and lighting.

---

## Install

1. Clone or copy this repository into `<YourProject>/Plugins/Grungify`.
2. Regenerate project files and build the editor target (it's a C++ plugin).
3. Enable **Grungify** in *Edit → Plugins* if it isn't enabled already.
4. On first editor start the plugin creates its assets in the plugin content folder (*Content Browser → Settings → Show Plugin Content → Grungify Content*):
   - `BP_WeatheringZone`: the zone to drop into levels
   - `Materials/MF_GrungifyWeathering`: the material function that renders the wear
   - `Materials/M_GrungifyMaskDebug`: shows the raw masks during preview
   - `Materials/M_GrungifyExample`: a minimal master material that already uses the function
   - `Materials/T_GrungifyNoMask`: the black default for the mask texture parameter

   Uasset files can't ship as text, so they're generated locally. Commit them once they exist.

### Content versioning

The generated material assets carry a marker of the form `[GrungifyContent v4 cpd=0 uv=1]` (content version, custom data index, mask UV channel). On editor start, and whenever the CPD index or mask UV channel changes in Project Settings, outdated assets are **rebuilt in place**. Every material that already calls `MF_GrungifyWeathering` picks up the fix without relinking. Clear and re-apply zones created before v0.2.1 so they receive the new owned material instances and activation values.

- **Tools → Grungify → Update Plugin Content** creates missing assets and rebuilds outdated ones.
- **Rebuild Plugin Content** forces a rebuild of all the generated material assets.

Every build checks graph connections, channel outputs and ownership gates, compiles the debug and example materials, and checks package saves. Failed validation prevents saving generated packages. Readiness requires a successful build and a `Saved/Grungify/ContentReady.txt` token matching the shader and all five saved packages. An interrupted or partial save remains eligible for repair. Results go to the Output Log and `Saved/Grungify/ContentReport.txt`, with a notification if anything failed.

## Hook up your materials (once per master material)

The zone writes masks; the material function renders them. In each master material you want weathered:

```
[your Base Color] ──► BaseColor ┐
[your Roughness]  ──► Roughness │
[your Metallic]   ──► Metallic  ├─ MF_GrungifyWeathering ─► BaseColor / Roughness / Metallic / Normal
[your Normal]     ──► Normal    │
                      SurfaceOverride (optional, default -1)
```

- **Meshes no zone has touched (or that were cleared) pass through unchanged.** Weathering requires both the zone's signed mask mode and `Grungify Active = 1` on a component-owned material instance. The master material's activation defaults to 0, so unrelated custom primitive data and unpainted white vertex colours cannot activate it.
- **SurfaceOverride**: leave unconnected to use the zone's per-slot `Grungify Surface Override` parameter. You can connect an explicit material value instead: `0` stone, `1` metal, `2` trim; `-1` uses the component's detected surface.
- **Normal** is passed through unchanged unless `Grungify Relief Strength` is above 0. When it is, chips read as recessed and dust as slightly raised, layered on top of your normal rather than replacing it. Relief uses finite differences rather than ddx/ddy, so it also works with Nanite.
- Parameters (group *Grungify*):
  - colours: `Grungify Dust Color`, `Grungify Runoff Color`, `Grungify Trim Chip Color`
  - strength: overall `Grungify Strength`, plus separate `Grungify Chip / Dust / Runoff / Handling Strength`
  - relief: `Grungify Relief Strength`
- The shading code lives in `Shaders/Private/Grungify.ush`, so you can tune the look there without rebuilding the graph.

You don't need materials hooked up to start: set **Preview Display** to *All Masks* and the preview swaps in the debug material.

## Using a zone

1. Drag `BP_WeatheringZone` into the level and scale or size its box over the area.
2. Adjust settings. With **Live Preview** on, the result regenerates whenever you release a slider or finish moving the zone.
3. Review it. Switch **Preview Display** between *Final Material* and the individual masks (*Edge Chips*, *Dust*, *Runoff*, *Handling*) to check each layer.
4. Press **Apply** to commit, or **Cancel Preview** to drop the preview.
5. Press **Clear** to put every mesh the zone touched back the way it was.

Apply and Clear are single undo steps (Ctrl+Z). **New Seed** rolls a new random seed; type a seed in by hand to get back a previous look. **Dry Run** generates and reports without touching anything.

A preview is never saved. Saving the level, deleting the zone, or unloading it (level unload or a World Partition cell streaming out) discards the preview and puts the meshes back. Delete is undoable, and undo restores the preview too. Autosaves keep the preview, since they're recovery copies and the zone's records are saved with them. Applied weathering stays on the meshes when the zone is deleted.

### Where the masks go

| Mask Output | Storage | Use for |
|---|---|---|
| **Vertex Color** | Per-instance override vertex colours on every LOD | Dense meshes. Detail follows vertex density, and it may not render on Nanite. |
| **Texture** | A per-instance mask texture baked in the mask UV channel (Project Settings, default UV 1), bound through a material instance on each slot. Texel size is physical (*Mask Texel Size*, default 4 cm). | Nanite meshes, and sparse geometry such as one-quad walls. Edge wear is computed from the distance to creased edges (*Edge Wear Width*), so it stays crisp regardless of vertex count. |
| **Auto** (default) | Texture for Nanite or sparse meshes that have the mask UV channel, vertex colours otherwise. | |

The mask UV channel must be non-overlapping, which lightmap UVs usually are. Meshes without that channel fall back to vertex colours, and the report says so. For Nanite meshes the texture is baked from the fallback mesh, which shares the UV layout.

The mask texture and material instances live inside the component, so they're saved with the actor and removed by Clear. Both output modes create owned material instances only on admitted slots; excluded slots retain their materials and inactive shading.

### Targeting

| Setting | What it does |
|---|---|
| **Include Only Actors** / **Excluded Actors** | Actor-level filters. |
| **Required Component Tags** | Only components carrying one of these tags. |
| **Slot Rules** | Per material slot, first match wins. Match by actor, wildcard name (slot, material and its parents, mesh, component, actor label), or exact material. A rule can **exclude** the slot, set its **surface**, give it its own **profile** (e.g. separate ceramic, steel and bronze multipliers), force **rail on or off**, and **gate** it by facing, by height above the floor, or by a painted **gate texture** in any UV channel. |
| **Overlap Policy** | *Skip Owned* (default) leaves meshes another zone applied to alone and reports them. *Take Over* moves ownership, including the original backup, to this zone on Apply. Preview always skips another applied owner, even with Take Over selected. Meshes another zone is currently previewing are always skipped. |

Example: rail wear without touching a joined bridge deck. Add a rule matching `*Deck*` (or the deck material) with *Exclude* on. Or give it a profile with Handling 0 and a facing gate, if the deck should still collect dust.

### Runoff at real leak locations

Add entries to **Leaks** and drag their diamond widgets to the actual leak points. Each leak has its own radius, length and strength. Turn off **Auto Detect Vents** to use only hand-placed leaks and the explicit **Runoff Sources** actors.

### Reports

Every Dry Run, Preview and Apply fills **Last Report** on the zone. It's also readable from Blueprint or Python via `GetLastReport`, and written to `Saved/Grungify/Reports/<Zone>_<Operation>.json`. Per mesh it lists:

- the result (vertex, texture or skipped) and why
- each slot's material, surface, matched rule and flags
- vertex count and texture size
- the mean and max of each mask
- whether the mesh is Nanite
- custom data conflicts
- the previous owner

*Tools → Grungify* also has **Preview / Apply / Clear / Cancel All** for every zone in the level.

When a zone is selected, the viewport shows the dust height (tan), the hand-height band (green) and each detected runoff source with its spread cone (blue). The blue arrow is the wear direction. The tan and green lines are drawn from the bottom of the box; the actual generation measures from the traced floors.

### Controls

| Group | Setting | What it does |
|---|---|---|
| **Grungify** | Seed | Same seed, same result. |
| | Intensity | Overall strength. |
| | Feature Scale (cm) | Physical size of chips, dust patches and streak spacing. Also drives the shader detail. |
| | Surface Type | *Auto* classifies each slot; *Stone / Metal / Trim* forces everything in the zone. |
| | Live Preview, Preview Display | See above. |
| **Wear Direction** | Wear Direction, Influence | Direction weather and traffic come from. Edges facing into it chip more. |
| **Dust At Feet** | Intensity, Height, Ledge, Crevice, Floor | How high dust climbs, plus dust on ledges, in creases and on open floor. |
| **Runoff Below Vents** | Intensity, Length, Spread, Ambient Streaks, Runoff Sources | Streak length and fan-out. Add extra sources by hand. |
| **Handling On Rails** | Intensity, Hand Height Min/Max, General Handling | The hand band, and how much non-rail edges inside it get handled. |
| **Edge Chips** | Intensity, Edge Angle, Floor Kick Height | Lower *Edge Angle* to catch bevelled edges. |
| **Surfaces** | Stone / Metal / Trim profiles | Per-surface multipliers for each layer. |
| **Filtering** | Zone Falloff, Floor Mode, Floor Probe Spacing, Include Movable, Excluded Actors | Soft edge of the zone, how floors are found, and what gets skipped. |

## How surfaces, rails and vents are recognised

Checked in this order (*Project Settings → Plugins → Grungify*):

1. **Tags** on the component or actor: `Grungify.Stone`, `Grungify.Metal`, `Grungify.Trim`, `Grungify.Rail`, `Grungify.Vent`, `Grungify.Floor`, `Grungify.Ignore`.
2. **Physical material**: map physical surface types to stone/metal/trim in *Physical Surface Map*.
3. **Name keywords** in the material (and its parents), the material slot, the mesh, and the actor label. Names are split into words (`SM_HVACVent_02` becomes *sm, hvac, vent, 02*), so `Railing` matches *rail* but `BP_EventTrigger` doesn't match *vent*. Trim is checked before metal and stone, so `M_MetalTrim` counts as trim.
4. Otherwise the **Fallback Surface Type** (stone).

Traced floors only count if the mesh is at least *Min Floor Size* (150 cm) across on its short side, so tables and shelves don't act as floors. Tag small floor tiles with `Grungify.Floor`.

## How it works

- Masks are computed per vertex. Edge convexity comes from the welded mesh topology: how sharply faces bend at a vertex, and whether the neighbouring vertices fall away (convex) or rise up (concave). Floors come from a grid of downward multi-hit traces. Runoff is projected down from each source's footprint.
- Results go into per-instance **override vertex colours** on each `StaticMeshComponent` (every LOD), the same storage Mesh Paint uses. They're saved with the level and the meshes' own assets aren't modified.
- **Custom primitive data** holds 5 floats starting at the configured index (default 0): mask mode (`501` vertex or `502` texture), surface type, feature scale, seed offset and debug channel. If either the saved defaults or live data already has non-zero values at those indices, the zone leaves that mesh untouched and reports a conflict. Writes preserve unrelated live values outside the five owned indices.
- Before touching a mesh, the zone records everything it might change, and Clear and Cancel Preview put back exactly that:
  - override vertex colours per LOD
  - the mesh-paint cache (painted vertices)
  - the LOD data count
  - the static mesh derived-data key
  - complete default and live custom primitive data arrays, including their distinct lengths (originally empty arrays come back empty)
  - the capture-time custom data index, so changing settings does not lose the original snapshot
  - the complete override material array, including its length

Backups made before v0.2.1 have no separate live-data snapshot. Restoring one uses its saved defaults for live data and logs this limitation; values absent from that old backup cannot be recovered.

## Limitations

- **Vertex Color output** follows vertex density, and Nanite may not render it. Use Texture output (the Auto default picks it) for Nanite and sparse meshes.
- **Texture output** needs a non-overlapping UV channel and adds a texture per mesh: about 1 byte per texel compressed (DXT5), plus mips.
- **Instanced / HISM** components are skipped, since all instances share one set of vertex colours and materials.
- **Existing vertex colours are replaced** on meshes weathered with Vertex Color output. Clear restores them.
- **Movable** meshes are skipped unless *Include Movable* is on.
- Weathered meshes must stay put: moving a mesh after Apply doesn't move its wear. Re-apply the zone.

## Native verification (UE 5.8.3, 2026-10-11)

- A clean Win64 `BuildPlugin` passed for UnrealEditor Development, UnrealGame Development and UnrealGame Shipping.
- All six native automation tests passed: five `Grungify.State` cases covering CPD, paint/material restoration, ownership and level removal, plus `Grungify.Content.InPlaceRegeneration`. The regeneration test rebuilds at CPD indices 0 → 12 → 0 and checks exact node counts, connections, parameter indices, compilation, saves and readiness.
- A disposable native level produced eight reviewed 1280 × 720 captures with fixed exposure. A Nanite-enabled coffer used UV1 texture masks: Preview and Apply visibly weathered its edges/panels, while legacy modes 1/2, Cancel and Clear visually returned the baseline. Raw readable material/CPD state restored exactly; the images are not claimed to be pixel-identical. Ownership transfer, occupied saved/live CPD and five real settings-regeneration checks also passed. The editor exited normally with code 0, and all 3,545 protected production files remained unchanged.

Run the tests in the isolated `HostProject` retained by `BuildPlugin -NoDeleteHostProject`, using `UnrealEditor-Cmd.exe <Package>/HostProject/HostProject.uproject -unattended -RenderOffscreen -ExecCmds="Automation RunTests Grungify" -TestExit="Automation Test Queue Empty"`. The content test refuses to rebuild assets in a production project.

These checks do not establish final environment art quality. Full undo/redo, save/reload, World Partition cell streaming and cooked game execution still need separate verification.

## Verification checklist

These cases should be checked in-editor on a test level, never on production rooms:

1. **Build:** the plugin compiles. On first start the Output Log shows content rebuilt to v4 with no errors, and `Saved/Grungify/ContentReport.txt` reports ready content without `ERROR` lines.
2. **Channel masks:** in MF_GrungifyWeathering, the Roughness mask is R only and the Metallic mask is G only.
3. **Exact restore:** on a mesh whose default custom primitive data is `[]`, run Preview → Cancel Preview, and separately Apply → Clear. The data is `[]` again. Repeat on a mesh with painted vertex colours, a mesh with only some material slots overridden, and a mesh whose CPD indices 0–4 are already in use (it should be reported as a conflict and left untouched).
4. **Undo:** run Apply → Ctrl+Z and Clear → Ctrl+Z. Delete a zone while previewing, then Ctrl+Z.
5. **Save/reload:** Preview → save → reload should show no preview on any mesh, and the zone shows no preview active. Apply → save → reload should keep the weathering, and Clear after the reload should restore the originals.
6. **Unload:** Preview, then unload the zone's World Partition cell or sublevel. The meshes should return to their originals.
7. **Inactive path:** untouched and cleared meshes using the function look identical to the same material without it.
8. **Nanite / sparse:** on a Nanite-enabled mesh and a one-quad wall with lightmap UVs, Auto produces `texture` in the report, and the mask debug display shows crisp edge wear and streaks.
9. **Targeting:** check a deck-exclusion rule, a gate texture, and a leak placed with the widget.

## Layout

```
Grungify.uplugin
Shaders/Private/Grungify.ush          Material-side noise and shading
Source/GrungifyShaders/               Maps /Plugin/Grungify for the shader include (PostConfigInit)
Source/Grungify/                      Runtime: zone actor, settings, mask generation
Source/GrungifyEditor/                Asset builder, viewport visualizer, Tools menu
```
