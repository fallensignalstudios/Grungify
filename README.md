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

> **Status:** v0.1, written for UE 5.8. This version hasn't been compiled or run in the editor yet. Expect a round of compile fixes on the first build.

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

   If they're missing, run **Tools → Grungify → Build Plugin Content**. Uasset files can't ship as text, so they're generated locally instead. Commit them once they exist.

## Hook up your materials (once per master material)

The zone writes masks; the material function renders them. In each master material you want weathered:

```
[your Base Color] ──► BaseColor ┐
[your Roughness]  ──► Roughness ├─ MF_GrungifyWeathering ─► BaseColor / Roughness / Metallic outputs
[your Metallic]   ──► Metallic  ┘
                      SurfaceOverride (optional, default -1)
```

- **SurfaceOverride**: leave unconnected to use the type the zone detected for the mesh. On a mesh that mixes stone and metal slots, set it per material: `0` stone, `1` metal, `2` trim.
- Tweakable parameters (group *Grungify*): `Grungify Dust Color`, `Grungify Runoff Color`, `Grungify Trim Chip Color`, `Grungify Strength`.
- The shading code lives in `Shaders/Private/Grungify.ush`, so you can tune the look there without rebuilding the graph.

You don't need materials hooked up to start: set **Preview Display** to *All Masks* and the preview swaps in the debug material.

## Using a zone

1. Drag `BP_WeatheringZone` into the level and scale or size its box over the area.
2. Adjust settings. With **Live Preview** on, the result regenerates whenever you release a slider or finish moving the zone.
3. Review it. Switch **Preview Display** between *Final Material* and the individual masks (*Edge Chips*, *Dust*, *Runoff*, *Handling*) to check each layer.
4. Press **Apply** to commit, or **Cancel Preview** to drop the preview.
5. Press **Clear** to put every mesh the zone touched back the way it was.

Apply and Clear are single undo steps (Ctrl+Z). **New Seed** rolls a new random seed; type a seed in by hand to get back a previous look.

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
- Feature scale, seed and detected surface type go into **custom primitive data** (4 floats starting at index 0; configurable), which the material function reads.
- The zone keeps a backup of each mesh's previous vertex colours and custom data. That's what Clear and Cancel Preview restore.

## Limitations

- **Vertex density matters.** Masks live on vertices, and the shader adds detail at the feature scale. A wall made of one quad gets only four samples, so the shape of the wear will be coarse.
- **Nanite** meshes may not show per-instance vertex colours. The zone warns when it touches one. Disable Nanite on those meshes, or check how your engine version handles it.
- **Instanced / HISM** components are skipped, since all instances share one set of vertex colours.
- **Existing vertex colours are replaced** on meshes inside the zone. Clear restores them.
- **Overlapping zones:** the last one applied wins on shared meshes. Clearing a zone restores what the mesh looked like before *that* zone.
- **Movable** meshes are skipped unless *Include Movable* is on.
- Weathered meshes must stay put: moving a mesh after Apply doesn't move its wear. Re-apply the zone.

## Layout

```
Grungify.uplugin
Shaders/Private/Grungify.ush          Material-side noise and shading
Source/GrungifyShaders/               Maps /Plugin/Grungify for the shader include (PostConfigInit)
Source/Grungify/                      Runtime: zone actor, settings, mask generation
Source/GrungifyEditor/                Asset builder, viewport visualizer, Tools menu
```
