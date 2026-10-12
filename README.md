# Grungify

Procedural damage and weathering for Unreal Engine 5. Drop a **BP_WeatheringZone** into a level, size its box, and press **Preview**, **Apply** or **Clear**. The zone finds every static mesh it overlaps and bakes four kinds of wear into them, each following how the space is built and used:

| Layer | Where it goes | Vertex channel |
|---|---|---|
| **Edge chips** | Convex, exposed edges. More on edges facing into the wear direction, and on edges near the floor that get kicked. | R |
| **Dust at feet** | Builds up from the floor (traced, so it works across storeys and stairs), on ledges, and in creases. | G |
| **Runoff below vents** | Streaks running down from vents, drains and spouts, fanning out as they fall. Faint streaks on all walls too. | B |
| **Handling on rails** | Polish/grime on rails and handles, plus a lighter band on edges at hand height. | A |

Optional **extended layers** add moisture and water lines, soot above lamps and fires, scorch, moss and lichen, rust bloom around fasteners, cracks and traffic scuffs (see [Extended layers](#extended-layers)).

Each material slot is classified as **stone**, **metal** or **trim**, so the wear follows the construction. Trim chips most, metal rusts under runoff and gets polished by hands, and stone collects dust.

Everything is driven by a **Seed**, so the same seed and settings always produce the same result.

> **Status:** v0.5.1 was natively verified on UE 5.8.3 (Editor, Development and Shipping builds; 18 automation tests; see [Native verification](#native-verification-ue-583-2026-10-11)). It includes:
> - **v0.4:** zones panel, wear stamps, cache and background preview, seed variants, Bake To Assets, sky exposure, water flow, extended layers, scatter, instanced meshes, Data Layers, a commandlet, a Blueprint/Python API, runtime wear and a reference scene.
> - **v0.5:** automatic material hookup and Substrate.
>
> **v0.6.1 passed native UE 5.8.3 builds and all 19 automation tests.** Four fixed reference views render successfully; approved screenshot baselines, packaged game rendering and the new terrain/displacement shader paths remain unverified. The 0.6 update adds:
> - **A stronger default look**, with a **Grungify Contrast** control (see [Look strength](#look-strength)).
> - **Skeletal meshes, landscape, height blending and Nanite displacement.**
> - **Fab packaging:** a showcase map, build scripts and a readiness check, read-only installed content, engine-compatibility wrappers for 5.5–5.8, and copyright headers.
>
> **v0.9.0 adds, not yet compiled natively:**
> - **Weather Selected Actors:** select props, right-click, *Weather with Grungify*. It places a fitted zone, offers the material hookup, and previews (see [Quick start](#quick-start-one-minute)).
> - **Shader code compiled offline in CI:** `Grungify.ush` and every generated custom node pass a real HLSL compiler (glslang), so shader typos fail on push, not in the editor.
>
> **v0.8.1 passed native UE 5.8.3 builds (Editor, Game and Shipping) and all 25 automation tests on D3D12**, including the eight-layer runtime GPU readback test. From the pressure-hall trial feedback, 0.8 adds:
> - **Runtime wear on all eight layers**, Handling and Moss included. Strokes change only their channel, verified by GPU readback (see [Runtime wear](#runtime-wear-gameplay)).
> - **Portable bakes**, with recorded compatibility (see [Bake to assets](#bake-to-assets-non-destructive)).
> - **Material hookup that keeps your edits** on Remove.
> - **Background previews from immutable snapshots**, restarted when a mesh changes mid-run.
> - **Landscape wear clipped to the zone**, and real compile checks for the landscape and displacement functions.
>
> v0.7.0 added:
> - **Art-direction controls**, independent of Feature Scale: chip scale, coverage, width and breakup; surface protection; grime breakup; and recess roughness variation (see [Art direction](#art-direction)).
> - **A Dry Coated Ceramic library look.**
> - **Per-slot physical coverage** in reports.
> - **A read-only inspection API** for QA scripts (see [Inspection](#inspection-read-only)).
>
> See the [Verification checklist](#verification-checklist).

---

## Quick start (one minute)

1. Install the plugin (below) and open your level.
2. Select the props, walls or vehicles you want weathered.
3. Right-click them and choose **Weather with Grungify** (or *Tools → Grungify → Weather Selected Actors*).
   - If their materials don't use Grungify yet, you're offered to hook them up (backed up first, reversible).
   - A zone is placed around the selection, limited to exactly those actors, and previewed. One undo step reverts all of it.
4. Tweak the zone in the Details panel (Seed, Strength, a library look), then press **Apply**.

From Python: `unreal.GrungifyEditorLibrary.weather_actors(actors, True)` does the same without the dialogs.

## Install

1. Clone or copy this repository into `<YourProject>/Plugins/Grungify`.
2. Regenerate project files and build the editor target (it's a C++ plugin).
3. Enable **Grungify** in *Edit → Plugins* if it isn't enabled already.
4. On first editor start the plugin creates its assets in the plugin content folder (*Content Browser → Settings → Show Plugin Content → Grungify Content*):
   - `BP_WeatheringZone`: the zone to drop into levels
   - `Materials/MF_GrungifyWeathering`: the material function that renders the wear
   - `Materials/MF_GrungifySubstrate`: the same for native Substrate materials
   - `Materials/MF_GrungifyLandscape`: the same for landscape materials (world-projected masks)
   - `Materials/MF_GrungifyDisplacement`: adds chip, crack and dust depth to Nanite displacement
   - `Materials/M_GrungifyMaskDebug`: shows the raw masks during preview
   - `Materials/M_GrungifyExample`: a minimal master material that already uses the function
   - `Materials/T_GrungifyNoMask`: the black default for the mask texture parameter

   Uasset files can't ship as text, so they're generated locally. Commit them once they exist.
5. Optional, from *Tools → Grungify*:
   - **Create Material Library**: starter presets and material instances in `/Game/Grungify/Library`.
   - **Build Reference Scene**: the visual test level.

### Content versioning

The generated material assets carry a marker of the form `[GrungifyContent v12 cpd=0 uv=1 inst=0]` (content version, custom data index, mask UV channel, per-instance custom data index). On editor start, and whenever one of those indices changes in Project Settings, outdated assets are **rebuilt in place**. Every material that already calls `MF_GrungifyWeathering` picks up the fix without relinking. Clear and re-apply zones created before v0.2.1 so they receive the new owned material instances and activation values.

- **Tools → Grungify → Update Plugin Content** creates missing assets and rebuilds outdated ones.
- **Rebuild Plugin Content** forces a rebuild of all the generated material assets.

Every build checks graph connections, channel outputs and ownership gates, compiles the debug and example materials, and checks package saves. Failed validation prevents saving generated packages. Readiness requires a successful build and a `Saved/Grungify/ContentReady.txt` token matching the shader and all eight saved packages. An interrupted or partial save remains eligible for repair. Results go to the Output Log and `Saved/Grungify/ContentReport.txt`, with a notification if anything failed.

## Hook up your materials (once per master material)

The zone writes masks; your materials render them. Let Grungify wire itself in:

- **Tools → Grungify → Hook Up Materials In Level**: finds every master material used by static meshes inside your zones (or the whole level, if it has no zones yet) that doesn't use Grungify, lists them, and hooks them all up after you confirm.
- **Right-click materials or material instances in the Content Browser → Add Grungify / Remove Grungify**, or the same from *Tools → Grungify* for the current selection. An instance resolves to its parent material, so every instance of that parent gets weathering.
- From Python or Editor Utility Blueprints: `UGrungifyMaterialHookup.add_grungify_to_material(material)`, `remove_grungify_from_material`, `uses_grungify` and `find_unhooked_materials_in_level`.
- **Your edits after Add are kept.** Each installation records exactly which nodes it inserted and which outputs it routed (editor-only asset user data on the material). *Remove* reconnects each output to whatever currently feeds Grungify, so upstream changes you made after Add survive, and deletes only the recorded nodes, so copies you made of them stay. If your own nodes read Grungify's nodes, Remove refuses and says which; disconnect them first. Installations from before 0.7 still remove the old way.

What it does for each kind of material:

| Material | What gets inserted |
|---|---|
| Ordinary (pins connected) | `MF_GrungifyWeathering` between your Base Color, Roughness, Metallic and Normal and the material outputs. Unconnected pins and *Use Constant* values become constants with the same values, so nothing changes on unweathered meshes. World-space normals and constant normals are left alone. |
| *Use Material Attributes* (including material layers) | Get Material Attributes → `MF_GrungifyWeathering` → Set Material Attributes, routing the same four attributes and passing everything else through. |
| Substrate (*Front Material* connected) | `MF_GrungifySubstrate` between your material and Front Material. If Front Material is a single slab, its normal feeds the grime layer. See [Substrate](#substrate). |

Safety:

- **Backups:** before the first change, a saved copy goes to `/Game/Grungify/Backups/<original path>/<Name>_PreGrungify`. Failed saves and unsaved backup collisions stop Add. Later Adds keep the first saved material copy.
- **Compile check:** each hooked-up material is compiled straight away, with the subject kept alive across Unreal's garbage collection. A failed Add rolls back its edits. A failed Remove is reported as a failure; use Undo to recover its prior graph.
- **Undo:** Add and Remove are editor transactions. Remove restores supported original connections, component masks and constant values. Legacy pins with dormant wires behind *Use Constant*, disconnected non-default pin metadata, and world-space normals in Material Attributes are refused before any edit because exact restoration is not supported yet.
- **Inserted nodes:** Remove identifies tool nodes by their Grungify description tags. Avoid copying those tags onto unrelated nodes; persistent per-hookup ownership is still needed for more complex edited graphs.
- **Open editors:** a material's open editor is closed before it's changed, so the two can't clash.
- **Skipped materials:** engine content, Grungify's own materials, unlit materials and non-surface domains (decals, post process, UI) are skipped with a reason.
- **Saving:** changed materials are left unsaved, ready for review.
- **Report:** results go to the notification and `Saved/Grungify/MaterialHookup.txt`.

Zone reports flag every included slot whose material doesn't use Grungify, and say how to fix it.

### Wiring it by hand

If you'd rather place the function yourself, in each master material you want weathered:

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
  - colours: `Grungify Dust Color`, `Grungify Runoff Color`, `Grungify Trim Chip Color`, `Grungify Moss Color`, `Grungify Soot Color`, `Grungify Salt Color` (efflorescence)
  - strength: overall `Grungify Strength` and `Grungify Contrast`, plus separate `Grungify Chip / Dust / Runoff / Handling / Moisture / Soot / Scorch / Moss / Crack Strength`
  - relief: `Grungify Relief Strength` (cracks also cut into the normal)
  - textures: `Grungify Mask Texture` and `Grungify Extended Mask Texture`, both set by the zone
- The shading code lives in `Shaders/Private/Grungify.ush`, so you can tune the look there without rebuilding the graph.

You don't need materials hooked up to start: set **Preview Display** to *All Masks* and the preview swaps in the debug material.

## Substrate

Projects with Substrate enabled are supported two ways:

- **Legacy-pin materials** keep using `MF_GrungifyWeathering`. Substrate converts them automatically, as it does any legacy material.
- **Native Substrate materials** use `MF_GrungifySubstrate`: Material (Substrate) in, Material out. It mixes one **grime slab** over your material with *Substrate Horizontal Mixing*.
  - The slab's albedo, F0, roughness and coverage come from the same masks, noise and palette as the legacy function.
  - All the wear layers are composited into that one slab, so the material gains exactly one extra closure, however many layers are active.
  - Its normal is your slab's normal, plus relief when `Grungify Relief Strength` is above 0.
  - Inactive meshes get zero coverage, so they look the same, though the material still carries the extra closure.

Differences from the legacy look: the legacy function edits your material's values directly, while Substrate layers grime on top. A few effects are therefore approximated:

- Handling polish on metal is a polished-metal layer rather than lowering your roughness.
- Darkening from handling and moisture is a dark layer rather than a multiply. This is exact for albedo.
- Fresh stone at chips uses `Grungify Stone Chip Color` (Substrate function only) rather than brightening your albedo.

Compare the two on the reference scene if you mix legacy and Substrate materials.

The content build always creates `MF_GrungifySubstrate`. In a project with Substrate enabled, it also compiles a throwaway Substrate material that uses the function, to prove it works. In a project without Substrate, the report notes that this compile check was skipped.

## Using a zone

*Weather Selected Actors* (see [Quick start](#quick-start-one-minute)) places and sizes a zone for you. By hand:

1. Drag `BP_WeatheringZone` into the level and scale or size its box over the area.
2. Adjust settings. With **Live Preview** on, the result regenerates whenever you release a slider or finish moving the zone.
3. Review it. Switch **Preview Display** between *Final Material* and the individual masks to check each layer: *Edge Chips*, *Dust*, *Runoff*, *Handling*, plus *Moisture*, *Soot*, *Scorch*, *Moss* and *All Extended* for the extended layers.
4. Press **Apply** to commit, or **Cancel Preview** to drop the preview.
5. Press **Clear** to put every mesh the zone touched back the way it was.

Apply and Clear are single undo steps (Ctrl+Z). **New Seed** rolls a new random seed; type a seed in by hand to get back a previous look. **Dry Run** generates and reports without touching anything.

### Look strength

The default look in v0.6 is deliberately stronger than v0.5's, which testers found too subtle on broad surfaces and runoff:

- **Single noise pass:** variation now comes only from the shader's noise. The bake carries the wear structure at full strength.
- **Lower thresholds:** chips start showing from a mask of about 0.25, and runoff streaks from about 0.05.
- **Stronger blends:** dust reaches 95% of its colour, handling darkens stone by up to 30%, and runoff reaches 90%.
- **Raised profiles:** the default surface profiles are higher (stone runoff and chips 1.0).

**Grungify Contrast** (material parameter, or *Palette → Contrast* on a zone or preset) reshapes how boldly faint wear reads, without changing fully worn or untouched areas:

| Contrast | Look |
|---|---|
| 0.6 | Close to v0.5's subtle look |
| 1 | Standard (default) |
| 1.5–2 | Heavy |

The library looks set 1.25 (Abandoned Industrial), 1.15 (Wet Sewer) and 1.0 (Old Temple).

**Contrast vs Strength:**

- **Strength** scales everything, including the most worn areas.
- **Contrast** lifts or sinks the in-between areas.

If wear still looks faint, switch **Preview Display** to *All Masks*:

- If the masks are faint too, raise the zone's layer intensities or profiles.
- If the masks are strong but the final material isn't, raise Contrast or check the material is hooked up. The zone report warns when it isn't.

### Art direction

Feature Scale sets the size of everything at once. On real assets that's not enough: you want a narrow worn edge with occasional chips and local grime, not a uniform outline or a broad flat patch. These controls are material parameters, and also *Palette → Art Direction* overrides on a zone or preset. They are independent of Feature Scale, and at their defaults they reproduce the standard look exactly.

| Parameter | Default | What it does |
|---|---|---|
| `Grungify Chip Scale` (cm) | 0 | Size of chip clusters. 0 follows half the Feature Scale; set it, for example to 1.5 cm, for fine chips on a coarse Feature Scale. |
| `Grungify Chip Coverage` | 0.5 | How much of a worn edge is chipped. Lower values keep edges mostly intact with occasional chips. |
| `Grungify Chip Width` | 1 | How much of the edge-wear band can chip. Lower values (around 0.3–0.5) give a narrow worn edge rather than a broad outline. |
| `Grungify Chip Breakup` | 0 | Breaks the outline with intact gaps, and mixes small chip clusters in with the medium ones. |
| `Grungify Surface Protection` | 0 | Removes low-level dust, runoff and handling below this level. Surface centres stay intact and grime stays local, which removes "continuous base coverage". |
| `Grungify Grime Breakup` | 0 | Turns continuous dust and handling grime into local patches. |
| `Grungify Roughness Variation` | 0 | Varies roughness across grimy recesses, from packed and dull to smeared and slicker, instead of a uniform matte. |

Raising Strength makes everything heavier, including broad flat areas, which is what produces coarse dark plates. To make wear read without that, use these controls instead:

1. **Width, coverage and breakup** shape the edge.
2. **Surface Protection** keeps faces clean.
3. **Contrast** decides how boldly what's left reads.

**Dry Coated Ceramic** (library look) is a starting point for coffers, panels and glazed or coated trim:

- Feature Scale 6 cm, Chip Scale 1.5 cm
- Chip Coverage 0.35, Width 0.4, Breakup 0.7
- Surface Protection 0.25, Grime Breakup 0.6, Roughness Variation 0.2
- restrained dust, handling and runoff
- Contrast 1.1

Tune it on the real asset. It isn't an approved look.

### Progress and cancelling

Any Dry Run, Preview or Apply that takes longer than about 0.3 s opens a progress dialog with a **Cancel** button. The dialog names the current phase and, during generation, each mesh in turn (`Actor / Component (3 of 40)`).

Generation runs in these phases: floor traces, sky exposure (when enabled), scene sources, scan, and mask generation. Scene sources covers vents, lights, fires, fasteners, stamps, traffic paths and traced water. Cancel is checked between phases, every row of floor-trace cells and exposure probes, and every LOD and every 32 texture rows inside each mesh. Nothing is written until generation has finished. So Cancel never leaves a half-weathered level:

- **Preview:** nothing new is written; the previous preview was already removed before generation started.
- **Apply:** no mesh is weathered and already-applied meshes are unchanged; an active preview is removed.
- **Dry Run:** nothing changes.

If a later mesh cancels generation, completed CPU mask rows are marked `cancelled_before_write`; their mask and UV diagnostics remain available, while the report counts zero newly weathered meshes.

The final write phase is short and can't be cancelled, so its dialog has no Cancel button. Every run records how long each phase took, and how many items it handled, in **Last Report → Phases**; the zone's Status line shows them too.

### Presets and palette

- **Palette** (on the zone) sets the material look: dust, runoff and trim-chip colours, overall and per-channel strength, and relief. Only the fields you tick are written; everything else keeps inheriting from the mesh's own material instance. The palette is written onto the zone's own material instances on every Preview and Apply, so it survives regeneration, re-seeding, Clear → Apply and plugin-content rebuilds.
- **Material Profiles** (under *Surfaces*) are named wear profiles such as *Ceramic*, *Steel* or *Bronze*. A slot rule uses one by setting **Profile Name**.
- **Presets** (`Grungify Preset` data assets) store a whole look: layer settings, the Stone/Metal/Trim profiles, Material Profiles, the palette and, optionally, the seed. Scene-specific settings stay on the zone: wear direction, leaks, actor lists, slot rules and filtering.
  - **Save To Preset** writes the zone's look into the assigned preset. If none is assigned, it creates `GP_<ZoneName>` in **Preset Folder** (default `/Game/Grungify/Presets`) and assigns it.
  - Assigning a preset, or pressing **Load Preset**, copies it into the zone as one undo step and refreshes the preview.
  - Presets live in your project's content, never in the plugin's generated content, so Update/Rebuild Plugin Content never touches them. They're ordinary assets, so you can share them between zones and levels and diff them in source control.

A preview is never saved. Saving the level, deleting the zone, or unloading it (level unload or a World Partition cell streaming out) discards the preview and puts the meshes back. Delete is undoable, and undo restores the preview too. Autosaves keep the preview, since they're recovery copies and the zone's records are saved with them. Applied weathering stays on the meshes when the zone is deleted.

### Where the masks go

| Mask Output | Storage | Use for |
|---|---|---|
| **Vertex Color** | Per-instance override vertex colours on every LOD | Dense meshes. Detail follows vertex density, and it may not render on Nanite. |
| **Texture** | A per-instance mask texture baked in the mask UV channel (Project Settings, default UV 1), bound through a material instance on each slot. Texel size is physical (*Mask Texel Size*, default 4 cm). | Nanite meshes, and sparse geometry such as one-quad walls. Edge wear is computed from the distance to creased edges (*Edge Wear Width*), so it stays crisp regardless of vertex count. |
| **Auto** (default) | Texture for Nanite or sparse meshes that have the mask UV channel, vertex colours otherwise. | |

The mask UV channel must be non-overlapping, which lightmap UVs usually are. For Nanite meshes the texture is baked from the fallback mesh, which shares the UV layout.

**UV validation.** Whenever a texture is a candidate for a mesh, its mask UV channel is checked first. The result goes into the report's **Mask UV** entry for that mesh:

| Check | Fails at | Warns at |
|---|---|---|
| Channel present | missing | |
| Overlapping islands (texels claimed by two triangles, on a 256² raster; shared edges don't count) | > 5% | > 0.5% |
| UV area outside 0..1 | > 2% | any |
| Zero-area UV triangles | | > 1% |
| Texel-density spread (world area per UV area, most vs least stretched triangle) | | > 8× |
| Coverage of the 0..1 square | none | < 20% |

Out-of-range UV area is measured by clipping each triangle against the unit square. A triangle crossing a boundary contributes only its actual outside portion; a small atlas overflow produces a warning without falsely failing the two-percent threshold.

With **Auto** output, a mesh whose UVs fail falls back to vertex colours and the report says why. With **Texture** output the bake still runs, since it was asked for, but the report notes the failure.

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

### Zones panel

*Tools → Grungify → Weathering Zones Panel* opens a dockable tab with every zone in the level:

- **Zones:** status, preview and applied counts, last run, warnings and preset. A filter box narrows the list, and a running background preview is marked.
- **Actions:** Preview, Apply, Clear, Cancel Preview and Dry Run on the selected zone. Dry Run All, Preview All, Apply All, Cancel All Previews and Clear All for every zone.
- **Selected zone:** its last report, one row per component with the result, notes, texture size, time and whether it came from the cache.
- **Selected component:** its slot decisions and mask UV checks.

### Faster iteration: cache and background preview

- **Use Generation Cache** (*Performance*, on by default) keeps each mesh's last result in memory. The cache key is a hash of everything that affects that mesh: settings, seed, zone transform, traced floors, sky exposure, water paths, traffic, stamps, lights and fires, and the mesh itself (transform, mesh asset and build, materials, slot decisions, instance transforms). Changing one setting only regenerates the meshes it actually affects. **Cache Budget** (default 512 MB) evicts least-recently-used entries. **Clear Generation Cache** empties it. The cache is editor-only and never saved.
- **Background Generation** (on by default) runs live previews on a worker thread, so the editor stays responsive while you drag sliders or move stamps. The quick game-thread work happens first: restoring the previous preview, traces and the scan. Any new change cancels the running job and starts another. A ticker writes the result when the worker finishes. The worker reads only snapshots (CPU copies of the mesh LODs and transforms taken when the job starts), never live components. If a mesh is edited, moved, reimported or re-materialled while the job runs, the result is discarded and the job restarts; undo, redo and unloading cancel it. Instanced meshes whose slots use a gate texture are reported as unsupported in background runs. Explicit **Preview**, **Apply**, **Bake**, **Dry Run** and the commandlet always run synchronously, with the progress dialog.
- The report shows cache hits per mesh (`bFromCache`) and per run (`CacheHits`), and whether the run was a background one.

### Seed variants (A/B comparison)

**Make Seed Variants** fills *Seed Variants* with the current seed plus two new ones. **Preview Next Variant** and **Preview Previous Variant** flip between them. Each variant stays in the generation cache, so flipping back is instant. Add or type seeds into the list by hand to compare specific ones. When you've chosen, Apply as usual; the zone's Seed is the variant shown.

### Wear stamps (paint-over)

Drop a **Grungify Wear Stamp** actor (*Place Actors → All Classes*) wherever the procedural result needs a hand-placed correction: an extra-dusty corner, a scorched patch, a clean spot.

- A stamp is a soft sphere (**Radius**, **Falloff**, **Edge Noise**, plus scale and rotation for ellipsoids).
- It has a per-layer amount for edge chips, dust, runoff, handling, moisture, soot, scorch and moss. Positive values add wear and negative values remove it.
- Stamps are actors, so they're saved with the level, undoable, deterministic, and kept when the zone regenerates with a new seed. A brush stroke would be lost on regeneration.
- Moving or editing a stamp refreshes every overlapping zone that has Live Preview on.
- Turn **Apply Wear Stamps** off on a zone to ignore stamps.

### Light- and sky-aware exposure

Tick **Use Sky Exposure** (*Exposure*) to probe how open each point is to the sky, using cosine-weighted rays (**Sky Rays**, default 24) on a grid (**Sky Probe Spacing**). **Sky Exposure Influence** scales the effect:

- Dust settles more in sheltered spots, under overhangs and in alcoves.
- Ambient streaks and chips concentrate where weather reaches.

### Water flow

Tick **Trace Water Flow** (*Runoff Below Vents*) and each runoff source is followed down the real geometry, one **Flow Step** at a time:

- Water falls until it hits something and clings to steep faces.
- It runs downhill over slopes, drops off edges, and pools on flat ground.

Runoff then follows those paths instead of a straight vertical cone. The pools feed the moisture layer. With the zone selected, the viewport draws each traced path.

### Extended layers

These are under *Extended Layers*, and are all off (0) by default. Anything that needs the extended mask texture switches that mesh to texture output.

| Layer | Driven by | Stored in |
|---|---|---|
| **Moisture** | Traced pools (**Pool Damp Radius**), traced water paths, a flood/**Water Line Height** above the floor, and shelter | extended R |
| **Soot** | Plumes rising above lamps (**Soot From Lights**, any visible local light) and fires (`Grungify.Fire` tag or *Fire Keywords*: fire, torch, brazier...), **Soot Radius**, **Plume Height** | extended G |
| **Scorch** | Within **Scorch Radius** of fires | extended B |
| **Moss / lichen** | Damp, sheltered, upward- and north-facing stone (**North Yaw**, **Moss North Bias**) | extended A |
| **Rust bloom** | Around fasteners (`Grungify.Fastener` tag, *Fastener Keywords* such as bolt, rivet, screw, or small meshes; instanced fasteners count each instance), **Rust Bloom Radius** | runoff (B) and chips (R) on metal |
| **Traffic scuffs** | **Grungify Traffic Path** spline actors (**Width**, **Traffic**): scuffs on floors along the path and on walls beside it up to **Scuff Wall Height** | dust (G) and chips (R) |
| **Cracks** | Shader-side cell borders on stone, scaled by chips and moisture; `Grungify Crack Strength` (palette or material) | material only |

Salt bloom (efflorescence) appears on damp stone, coloured by `Grungify Salt Color`.

Traffic is spline-driven on purpose: designers draw the routes people actually take, which is more predictable than inferring it from the NavMesh.

The extended texture is a second per-component mask texture at the same resolution. Vertex-colour output carries only the four primary masks, so meshes whose extended layers are non-zero use texture output.

### Decal and mesh scatter

**Scatter** entries place real decals or small meshes where a mask is high: cobwebs in sheltered dusty corners, puddle decals on pools, debris along floors, leaf litter. Each entry has:

- **Layer** and **Threshold**: which mask, and how high it must be
- **Density** per m²
- a **Decal Material** or a **Mesh**
- **Size Range**, and **Decal Depth** for decals
- **Surfaces**: optionally restrict to stone, metal or trim

Placement is deterministic per seed and capped at 2000 pieces per zone:

- Decals project into the surface and meshes align to its normal.
- Pieces go into a `Grungify/<Zone>/Scatter` outliner folder, optionally into **Scatter Data Layers**, and are tagged `Grungify.Scatter` and `Grungify.Ignore` so no zone weathers them.
- Scatter happens on **Apply** and **Bake** only. It's replaced on every Apply and removed by **Clear**.

### Instanced meshes (ISM / HISM)

With **Weather Instanced Meshes** on (default), every instance of an instanced or hierarchical instanced static mesh gets its own four mask values in **per-instance custom data**. These start at *Project Settings → Grungify → Instance Custom Data Index* (default 0). Values are sampled at 7 points on each instance.

The material function reads them when the component's mask mode is `503`. Detail is per instance, not per texel: a chipped crate versus a clean crate, a dusty floor tile versus a swept one. That's the right granularity for foliage-scale scatter.

- Existing per-instance custom data at our four slots is a conflict, so the component is skipped and reported.
- Other per-instance data is preserved, and Clear restores the exact original float count and values.

### Skeletal meshes (props and vehicles)

With **Weather Skeletal Meshes** on (default), zones weather skeletal mesh components as well as static meshes.

- The masks are baked into a texture from LOD0's reference pose, at the mesh's current placement.
- They live in UV space, so they move and deform with the mesh. Skeletal meshes therefore skip the *Include Movable* filter.
- The wear reflects where the mesh stood when the zone ran. Re-apply the zone after moving a vehicle somewhere very different.
- **UVs:** a skeletal mesh needs a clean, non-overlapping UV channel. When the configured mask channel (UV1) is missing or fails validation but UV0 is clean, UV0 is used, and the zone sets `Grungify Use UV0` on the mesh's material instances. If neither works, the mesh is skipped and the report says why.
- **Mesh only:** the bake uses the mesh, not the physics asset or animation.
- **Not supported:** vertex-colour output (skeletal override colours aren't saved with the level) and scatter.

### Landscape

With **Weather Landscape** on (*Landscape*, default on), a zone also weathers the landscape under it:

- dust, runoff along traced water, moisture, pools and moss, through the same rules and stone profile as meshes
- plus **contact wear**: extra dust and damp where the terrain meets walls and props (**Contact Radius**, **Contact Strength**)

How it's stored:

- **Mask texture:** each landscape proxy under the zone gets one world-projected mask texture covering the zone, at **Landscape Texel Size** (default 25 cm, at most 2048 texels a side). The texture is stored inside the proxy, so it saves with it, including on World Partition maps.
- **Material override:** each landscape component in the zone gets a material override, an instance of the landscape's own material with the mask bound.
- **Undo:** Clear and Cancel Preview put the original overrides back.
- **One zone per area:** a landscape component already weathered by another zone is left to that zone and reported. Landscapes have no Take Over.

The landscape material needs `MF_GrungifyLandscape`. **Hook Up Materials In Level** adds it to landscape materials under your zones, and the right-click *Add Grungify* recognises landscape materials by their Landscape Layer nodes. It has the same four inputs and outputs as `MF_GrungifyWeathering`, so put it after your layer blend.

Landscape limitations:

- The mask debug displays don't apply to landscapes.
- Substrate landscape materials aren't supported yet.
- *Bake To Assets* keeps the landscape mask inside the proxy rather than as a separate asset.
- Landscape wear stops at the zone's rectangle: texels outside it are zeroed, so the border of the mask no longer smears wear beyond the zone.

### Height blending and Nanite displacement

**Height blending.** All three weathering functions have an optional **Height** input (0..1, 0.5 = mid). Connect your material's height map and raise `Grungify Height Blend` (0 by default, which changes nothing):

- chips favour the raised parts (brick faces, raised trim)
- dust, damp and moss settle in the low parts (mortar joints, recesses)

**Displacement.** For real depth up close on Nanite meshes, use `MF_GrungifyDisplacement`. It takes your Displacement and returns it with chips and cracks cut in and dust slightly built up.

- **Hookup:** the hookup tool routes Displacement through it automatically when a material already drives Displacement. Displacement is opt-in because Nanite tessellation has a cost.
- **Setup:** enable Nanite tessellation on the mesh and material. The setting names and location vary between 5.x versions; see Epic's Nanite tessellation docs.
- **Depth:** set `Grungify Displacement Depth` on the material or instance, in the material's displacement units, for example 0.1. At 0, the default, it adds nothing.
- **Relief:** `Grungify Relief Strength` still works without tessellation, as a normal-only effect.

### Bake to assets (non-destructive)

**Bake To Assets** (*Bake*) does what Apply does, but stores the result as standalone project assets in **Bake Folder** (default `/Game/Grungify/Baked`):

- `T_Grungify_<Zone>_<Actor>_<Component>`: the mask texture
- `..._Ext`: the extended texture
- `MI_Grungify_<...>_<Slot>`: material instances parented to the original materials

Bake always asks for texture output. Meshes without the mask UV channel fall back to vertex colours, and the report says so. Instanced meshes keep per-instance data.

- **Touch-ups:** with **Export Baked PNGs** on, each texture is also written to `Saved/Grungify/Bakes/<Zone>/<Name>.png` and set as its import source. Paint over the PNG in any image editor, keeping it linear: the channels are masks, not colours. Then right-click the texture and choose **Reimport**.
- **Re-baking** updates the same assets in place, so references stay valid. It overwrites the PNGs, so keep your edited copies.
- **Clear** restores the meshes' original materials, but the baked assets stay. Delete them from the Content Browser if you no longer need them.
- **Sharing:** baked assets can be used by other levels, referenced by hand-authored materials, and reviewed in source control as normal textures.
- **Portable:** a baked instance works on any component that draws the **same mesh build**, even one Grungify never touched (no custom primitive data). The instance carries its own mask mode, feature scale, seed offset and surface type, used only when the component has none. It does not fit another mesh, or the same mesh after a rebuild that changed its UVs; re-bake for those. Each baked instance records what it was baked for, and `UGrungifyInspectionLibrary.CheckBakeCompatibility(material, component)` says whether it fits and why not.

### Material library

*Tools → Grungify → Create Material Library* creates starter looks in `/Game/Grungify/Library`. Each has a preset (`GP_<Look>`) and stone, metal and trim material instances of `M_GrungifyExample` (`MI_Grungify_<Look>_<Stone|Metal|Trim>`).

| Look | What it sets up |
|---|---|
| **Abandoned Industrial** | Heavy dust, long rust runoff, rust bloom on fasteners, light soot, cracked concrete |
| **Old Temple** | Crisp chipped stone, ledge dust, polished hand-height edges, cracks, sky exposure, moss |
| **Wet Sewer** | Traced water flow, moisture and a 40 cm water line, moss everywhere damp, dark streaks, salt bloom |
| **Dry Coated Ceramic** | Fine broken chips on a narrow worn edge, intact faces, restrained local grime (see [Art direction](#art-direction)) |

The library is project content, so plugin-content rebuilds never touch it. Existing assets are kept; **Reset Material Library** overwrites them with the defaults. Library presets don't include a seed, so each zone keeps its own.

### Data Layers and World Partition

- **Include Data Layers** (*Targeting*) limits a zone to actors in any of the listed Data Layer assets, and the report names actors skipped for this reason. Example: weather only the "Ruins" layer.
- **Scatter Data Layers** puts scattered actors into data layers.
- Zones work on whatever is currently loaded. Applied records for actors in unloaded cells are kept and restored by a later Clear once they're loaded. To process a whole World Partition map, use the commandlet below, which loads the region around each zone first.

### Commandlet (build machines and CI)

```
UnrealEditor-Cmd.exe Project.uproject -run=Grungify -Map=/Game/Maps/MyMap[,/Game/Maps/Other]
    -Operation=Apply|Clear|DryRun|Bake [-Zones=LabelA,LabelB] [-Save] [-Report=Path/To/Dir]
    [-LoadPadding=200] [-unattended -nullrhi]
```

- Runs every zone (or just the named ones) in each map, synchronously.
- On World Partition maps, it loads each zone's bounds plus **LoadPadding** (cm) first.
- Writes each zone's JSON report to `-Report` (default `Saved/Grungify/Reports`).
- Nothing is saved without `-Save`.
- Exit codes:
  - `0`: OK
  - `1`: warnings, conflicts or failed meshes
  - `2`: usage error or a missing map

### Blueprint and Python API

Every zone action is a `BlueprintCallable` UFUNCTION. That includes Preview, Apply, Clear, Cancel Preview, Dry Run, New Seed, Load/Save Preset, Bake To Assets, the seed variants and Clear Generation Cache. The getters are `GetLastReport`, `GetLastReportFile`, `GetAppliedCount`, `GetPreviewCount` and `IsGeneratingInBackground`. All of them are reachable from Editor Utility Blueprints and Python.

`UGrungifyBlueprintLibrary` (*Grungify* category) adds level-wide helpers:

| Function | What it does |
|---|---|
| `GetZones`, `FindZoneByLabel`, `GetZonesAffecting(Component)` | Find zones |
| `RunZone(Zone, Operation)`, `RunAllZones(World, Operation)` | Run Preview / Apply / Clear / Cancel Preview / Dry Run / Bake and return the reports |
| `SpawnZone(World, Transform, HalfExtent, Preset)` | Create a zone in the editor world, optionally with a preset |
| `SetZoneSeed(Zone, Seed, bPreview)` | Set a seed and optionally preview it |
| `ReportToJson`, `SaveReport` | Serialise reports |

```python
import unreal
world = unreal.EditorLevelLibrary.get_editor_world()
for report in unreal.GrungifyBlueprintLibrary.run_all_zones(world, unreal.GrungifyOperation.DRY_RUN):
    print(report.zone, report.weathered, report.skipped, report.seconds)
```

The editor builders are also available to Blueprint and Python through `UGrungifyEditorLibrary`:

```python
message = unreal.GrungifyEditorLibrary.build_reference_scene()
# A message string means success; None means failure (the log contains details).
# This explicitly creates/saves /Game/Grungify/Reference and opens its map.
# Run it in an isolated test project or after saving your current work.
message = unreal.GrungifyEditorLibrary.build_material_library(False)
```

### Runtime wear (gameplay)

`UGrungifyRuntimeWearSubsystem` (a world subsystem) paints extra wear into **texture-output** meshes during play: impacts, footsteps, scorch marks.

- **Functions:**
  - `AddWearAtHit(Hit, Layer, Radius, Amount)`
  - `AddWearAtLocation(Location, Direction, Layer, Radius, Amount)`
  - `ResetRuntimeWear(Component)`
- **Component:** `UGrungifyRuntimeWearComponent` paints under its owner with `EmitWear`, for example from footstep notifies.
- **Layers:** all eight. A stroke changes only its layer's channel: R, G, B or A of the mask (Edge Chips, Dust, Runoff, Handling) or of the extended mask (Moisture, Soot, Scorch, Moss). The other seven channels stay bit-exact.
- **How it works:** on a component's first hit, its baked masks are read back from the GPU into a CPU copy, and its slots switch to dynamic material instances that sample a live transient texture. Each stroke adds a smooth falloff to the selected channel of the CPU copy, saturating at 1, then uploads only the changed rectangle. Nothing is blended on the GPU, so alpha and the unselected channels can't be disturbed. Strokes wrap across UV edges when the baked texture wraps. Reset switches back to the baked result and discards the paint.
- **Memory:** each painted mask keeps a CPU copy (4 bytes per texel: 16 MB for a 2048² mask) until Reset.
- **Readback:** `ReadRuntimeWearMask(Component, MaterialIndex, bExtended, Pixels, Width, Height)` draws the live texture into a render target and reads it back, so QA scripts see what the renderer samples. It flushes rendering; use it for tests, not every frame.
- **Setup:** enable *Project Settings → Physics → Support UV From Hit Results*, and trace against complex collision so hits carry a face index. Meshes weathered with vertex-colour or instanced output can't be painted at runtime.
- **Requires a rendering RHI:** under `-nullrhi` (dedicated servers) the baked masks can't be read back, so nothing is painted and the materials are left alone.


### Inspection (read-only)

`UGrungifyInspectionLibrary` (Blueprint category *Grungify | Inspection*, and Python as `unreal.GrungifyInspectionLibrary`) reports exactly what Grungify has done to a component. It never runs Preview or Apply, and it never saves or loads anything.

- **`InspectComponent(Component)`** returns, for a static or skeletal mesh component:
  - **Identity:** the component's path, class, owner, outer and package, and whether it's transient.
  - **State:** none, previewed, applied, previewed+applied or baked, and which zone previews or applied it.
  - **Custom primitive data:** the saved defaults and the live values, *separately*, plus the mask mode and debug channel at the configured base index.
  - **Vertex colours:** override counts per LOD.
  - **Instances:** instance count, float count and Grungify's four per-instance floats.
  - **Per slot (admitted and excluded):**
    - the material, its class and parent
    - whether it's a zone-owned or baked instance
    - Active and the surface override
    - the bound mask and extended textures, with their sizes
    - the UV channel the material actually samples
  - **Unavailable**, a list of anything that couldn't be read, each with its reason.
- **`InspectZone(Zone)`** inspects every component a zone previews or has applied to. Records for components that aren't loaded come back with `bValid` false, their saved path and the reason.
- **`ResolveGrungifyObject(Path)`** finds an already-loaded object by its full path, including embedded subobjects such as a component's mask texture (`/Game/Map.Map:PersistentLevel.Actor.Component.GrungifyMask_0`). It never loads a package.

```python
import unreal
for info in unreal.GrungifyInspectionLibrary.inspect_zone(zone):
    print(info.component, info.state, info.mask_mode_name, list(info.live_custom_data))
    for slot in info.slots:
        print("  ", slot.slot, slot.material, slot.active, slot.mask_texture, slot.unavailable)
```

`Grungify.Inspection.ReadOnlyComponentState` checks it against untouched, applied, previewed and cleared components, and checks that inspecting changes nothing. Save/reload is not yet covered.

### Reports

Every Dry Run, Preview and Apply fills **Last Report** on the zone. It's also readable from Blueprint or Python via `GetLastReport`, and written to `Saved/Grungify/Reports/<Zone>_<Operation>.json`.

The run as a whole records:

- phase timings
- whether it was cancelled, and during which phase
- the preset in use
- cache hits, whether it ran in the background, scattered pieces and baked asset paths
- warnings

The path of the last JSON file is on the zone as **Last Report File**.

Each mesh records:

- the result (vertex, texture, instanced or skipped) and why, including meshes and whole actors inside the zone that a setting or tag filtered out
- vertex count, texture size (and extended texture size), instance count, generation time and whether it came from the cache
- the mean and max of each mask
- whether the mesh is Nanite
- custom data conflicts
- the previous owner
- **Mask UV**: the validation above
- **Slot Details**: one entry per material slot, covering:
  - **Area** (m²) and **Coverage** per channel: the area where the baked mask is above 0.1, that area as a fraction of the slot, the mean value over the covered area only, and the peak. These are the baked masks, before the material's noise and thresholds, so they measure where wear is *permitted* and how strongly. Global means hide sparse, deliberately local permissions; these don't.
  - **Included** and the **Reason**, e.g. *"excluded by rule 'Deck' (Exclude is on; matched by slot name 'Deck_Planks' matches '\*Deck\*')"*, *"included; no slot rule matched"*, or *"excluded: rule 'Rail' has a gate texture that couldn't be read, so no wear is allowed (fail-safe)"*
  - the **Rule** that matched and what matched it
  - the **Surface** and its **Surface Source**: zone override, rule, component or actor tag, physical material, the keyword and which name it was found in, or fallback
  - the **Profile** used: surface profile, the rule's own profile or a named Material Profile
  - **Rail** and why
  - active **Gates**
  - per-slot **Warnings**: a gate UV channel the mesh doesn't have, a Profile Name that doesn't exist, or a slot with no material

*Tools → Grungify* also has **Preview / Apply / Clear / Cancel All** for every zone in the level, and the zones panel.

When a zone is selected, the viewport shows the dust height (tan), the hand-height band (green) and each detected runoff source with its spread cone (blue), or its traced path when Trace Water Flow is on. The blue arrow is the wear direction. The tan and green lines are drawn from the bottom of the box; the actual generation measures from the traced floors.

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
| **Filtering** | Zone Falloff, Floor Mode, Floor Probe Spacing, Include Movable, Weather Instanced Meshes, Excluded Actors | Soft edge of the zone, how floors are found, and what gets skipped. |
| **Extended Layers** | Moisture, Water Line, Moss, Soot, Scorch, Scuffs, Rust Bloom | See [Extended layers](#extended-layers). |
| **Exposure** | Use Sky Exposure, Influence, Probe Spacing, Rays, Ray Length | Sheltered vs exposed. |
| **Stamps / Scatter / Variants / Performance / Bake** | | See the sections above. |

## How surfaces, rails and vents are recognised

Checked in this order (*Project Settings → Plugins → Grungify*):

1. **Tags** on the component or actor: `Grungify.Stone`, `Grungify.Metal`, `Grungify.Trim`, `Grungify.Rail`, `Grungify.Vent`, `Grungify.Floor`, `Grungify.Ignore`, plus `Grungify.Fire` (soot and scorch source) and `Grungify.Fastener` (rust bloom source).
2. **Physical material**: map physical surface types to stone/metal/trim in *Physical Surface Map*.
3. **Name keywords** in the material (and its parents), the material slot, the mesh, and the actor label. Names are split into words (`SM_HVACVent_02` becomes *sm, hvac, vent, 02*), so `Railing` matches *rail* but `BP_EventTrigger` doesn't match *vent*. Trim is checked before metal and stone, so `M_MetalTrim` counts as trim.
4. Otherwise the **Fallback Surface Type** (stone).

Traced floors only count if the mesh is at least *Min Floor Size* (150 cm) across on its short side, so tables and shelves don't act as floors. Tag small floor tiles with `Grungify.Floor`.

## How it works

- Masks are computed per vertex. Edge convexity comes from the welded mesh topology: how sharply faces bend at a vertex, and whether the neighbouring vertices fall away (convex) or rise up (concave). Floors come from a grid of downward multi-hit traces. Runoff is projected down from each source's footprint.
- Results go into per-instance **override vertex colours** on each `StaticMeshComponent` (every LOD), the same storage Mesh Paint uses. They're saved with the level and the meshes' own assets aren't modified.
- **Custom primitive data** holds 5 floats starting at the configured index (default 0): mask mode (`501` vertex, `502` texture or `503` instanced), surface type, feature scale, seed offset and debug channel. If either the saved defaults or live data already has non-zero values at those indices, the zone leaves that mesh untouched and reports a conflict. Writes preserve unrelated live values outside the five owned indices.
- Before touching a mesh, the zone records everything it might change, and Clear and Cancel Preview put back exactly that:
  - override vertex colours per LOD
  - the mesh-paint cache (painted vertices)
  - the LOD data count
  - the static mesh derived-data key
  - complete default and live custom primitive data arrays, including their distinct lengths (originally empty arrays come back empty)
  - the capture-time custom data index, so changing settings does not lose the original snapshot
  - the complete override material array, including its length
  - for instanced meshes, the per-instance custom data float count and every value

Backups made before v0.2.1 have no separate live-data snapshot. Restoring one uses its saved defaults for live data and logs this limitation; values absent from that old backup cannot be recovered.

## Limitations

- **Vertex Color output** follows vertex density, and Nanite may not render it. Use Texture output (the Auto default picks it) for Nanite and sparse meshes.
- **Texture output** needs a non-overlapping UV channel and adds a texture per mesh: about 1 byte per texel compressed (DXT5), plus mips.
- **Instanced / HISM** components get one set of four mask values per instance, not per-texel detail. Extended layers aren't stored per instance.
- **Background generation** reads mesh data on a worker thread while the editor runs. Garbage collection is blocked per mesh, but editing a mesh's geometry while its preview is generating could race; the next change regenerates it.
- **Wear stamps** are actors rather than a free-hand brush. Use **Bake To Assets** and paint over the PNGs for pixel-level touch-ups.
- **Traffic** comes from hand-placed splines, not the NavMesh.
- **Scatter** is capped at 2000 pieces per zone and only placed on Apply and Bake.
- **Runtime wear** needs texture output and complex-collision hits with UVs.
- **Existing vertex colours are replaced** on meshes weathered with Vertex Color output. Clear restores them.
- **Movable** static meshes are skipped unless *Include Movable* is on. Skeletal meshes are always considered.
- **Skeletal meshes** need a clean UV channel (UV1, or UV0 as a fallback), and their wear comes from the reference pose.
- **Landscape** wear is per zone area at Landscape Texel Size, with one zone per landscape component. There's no Substrate landscape support or debug display yet.
- **Height blending** needs the material's own height map connected to the functions' Height input.
- **Displacement** only adds depth where Nanite tessellation is enabled and the material drives Displacement.
- Weathered meshes must stay put: moving a mesh after Apply doesn't move its wear. Re-apply the zone.

## Native verification (UE 5.8.3, 2026-10-11)

- v0.6.1 passed strict Win64 Editor Development, Game Development and Game Shipping builds with unchanged source hashes. All 19 native automation tests passed (18 plain successes and one expected occupied-data warning); the editor exited normally with code 0. Tests include project plugins with `Installed=true`, all eight generated outputs and their saved hashes, landscape actor filtering, excluded-slot UV fallback conservation, and the retained material-hookup/Undo/Redo checks.
- The v0.6.1 isolated reference builder saved seven weathered meshes and four screenshot actors. All four 1280 × 720 screenshot tests rendered and exited normally with code 0; each warned that no approved comparison baseline exists. Coffer/trim edge wear and vent runoff read more strongly than v0.5.1 in matching views. Broad faces remain flat and the thin rail remains visually clean; some edge treatment forms continuous bands. This establishes a visible strength gain, not final environment art acceptance.
- v0.6.1 fixes the new showcase's stamp field and setup-time live-preview ordering, landscape targeting, stale landscape-cache reuse, and UV fallback writes to excluded materials. Landscapes are deliberately retraced on every generation until their sculpt/contact/profile revisions can be keyed correctly. The landscape test uses a transient bounded proxy; it does not prove rendered terrain wear. Landscape/displacement readiness validates graph structure and saved bytes without compiling or rendering those new paths. Substrate remained disabled in the verified host.
- v0.5.1 passed Win64 `BuildPlugin` for UnrealEditor Development, UnrealGame Development and UnrealGame Shipping, with `-NoPCH -NoSharedPCH -DisableUnity` and unchanged source hashes throughout the build.
- v0.5.1 passed all 18 native automation tests (17 plain successes and one expected occupied-data warning). The material-hookup fixture exercises Add/Remove, Undo/Redo, unchanged legacy connections, unsupported-input refusal and backup failure. The cache/scatter, exact instance-layout restoration and runtime-wear admission tests also passed. The actual editor process exited normally with code 0.
- The v0.5.1 reference builder saved seven weathered meshes and four functional screenshot actors in the isolated host project. `Grungify.Runtime.ReferenceSceneCooked` checks editor-loaded bindings and textures; its name does not establish a packaged or cooked rendering pass. Substrate was disabled, so its generated graph was validated but its enabled shader/render path remains unverified.
- All four v0.5.1 functional screenshot tests produced readable 1280 × 720 images and the editor exited normally with code 0. Each reported a missing approved baseline warning. Edge wear is visible on the trim and Nanite coffer; broad surfaces and sparse-wall/runoff treatment remain subtle. These are visual smoke checks, not approved regression comparisons or final art acceptance.
- v0.3.1 passed all nine native automation tests (eight plain successes, one expected occupied-data warning): the six original cases plus `TextureGateOutputConservation`, `MaskUVValidation` and `TextureBakeCancellation`. UV fixtures cover slight boundary overflow, genuinely excessive overflow and all four sides. The texture cancellation test exercises CPU generation cancellation; it does not establish public Preview/Apply cancellation, UI latency or the new report-row behavior.
- All six native automation tests passed: five `Grungify.State` cases covering CPD, paint/material restoration, ownership and level removal, plus `Grungify.Content.InPlaceRegeneration`. The regeneration test rebuilds at CPD indices 0 → 12 → 0 and checks exact node counts, connections, parameter indices, compilation, saves and readiness.
- A disposable native level produced eight reviewed 1280 × 720 captures with fixed exposure. A Nanite-enabled coffer used UV1 texture masks: Preview and Apply visibly weathered its edges/panels, while legacy modes 1/2, Cancel and Clear visually returned the baseline. Raw readable material/CPD state restored exactly; the images are not claimed to be pixel-identical. Ownership transfer, occupied saved/live CPD and five real settings-regeneration checks also passed. The editor exited normally with code 0, and all 3,545 protected production files remained unchanged.

Run the tests in the isolated `HostProject` retained by `BuildPlugin -NoDeleteHostProject`, using `UnrealEditor.exe <Package>/HostProject/HostProject.uproject -unattended -RenderOffscreen -ExecCmds="Automation RunTests StartsWith:Grungify;SoftQuit" -ReportExportPath=<FreshReportDirectory>`. Inspect the exported test counts and actual process exit. The content test refuses to rebuild assets in a production project.

The nine-test and disposable-level observations above are retained v0.2.x/v0.3.1 history. These checks do not establish final environment art quality. Material Attributes/Substrate hookup, general zone undo/redo, save/reload, World Partition cell streaming, channel-preserving GPU runtime painting and cooked game execution still need separate verification.

## Texture bake cost

Texture generation skips edge-distance queries and weathering evaluation when a texel's material slot is excluded, its zone weight is zero, or its facing, height or texture permission is zero. Positive permission values still receive the full calculation. Atlas ownership, covered-sample statistics, surface votes and the four dilation passes stay unchanged, including when an excluded triangle overlaps an admitted one.

`Grungify.Generation.TextureGateOutputConservation` compares final RGBA pixels, means, maxima and surface votes against the original evaluation path for narrow, inverted, zero, facing and height gates, overlapping excluded slots, missing slots and points outside the zone. It also checks that inactive texels avoid edge queries. Texture resolution and the requested physical texel size are unchanged.

## Verification checklist

These cases should be checked in-editor on a test level, never on production rooms:

1. **Build:** the plugin compiles. On first start the Output Log shows content rebuilt to v12 with no errors, and `Saved/Grungify/ContentReport.txt` reports ready content without `ERROR` lines.
2. **Channel masks:** in MF_GrungifyWeathering, the Roughness mask is R only and the Metallic mask is G only.
3. **Exact restore:** on a mesh whose default custom primitive data is `[]`, run Preview → Cancel Preview, and separately Apply → Clear. The data is `[]` again. Repeat on a mesh with painted vertex colours, a mesh with only some material slots overridden, and a mesh whose CPD indices 0–4 are already in use (it should be reported as a conflict and left untouched).
4. **Undo:** run Apply → Ctrl+Z and Clear → Ctrl+Z. Delete a zone while previewing, then Ctrl+Z.
5. **Save/reload:** Preview → save → reload should show no preview on any mesh, and the zone shows no preview active. Apply → save → reload should keep the weathering, and Clear after the reload should restore the originals.
6. **Unload:** Preview, then unload the zone's World Partition cell or sublevel. The meshes should return to their originals.
7. **Inactive path:** untouched and cleared meshes using the function look identical to the same material without it.
8. **Nanite / sparse:** on a Nanite-enabled mesh and a one-quad wall with lightmap UVs, Auto produces `texture` in the report, and the mask debug display shows crisp edge wear and streaks.
9. **Targeting:** check a deck-exclusion rule, a gate texture, and a leak placed with the widget.
10. **Cancel (v0.3.0):** on a zone large enough that the dialog appears, cancel during floor traces and again during mask generation, for both Preview and Apply. Afterwards no mesh should carry new masks, applied meshes should be unchanged, and Last Report should show `bCancelled` with the phase. Undo after a cancelled Apply should bring back only the removed preview.
11. **Presets (v0.3.0):**
    - Save To Preset with no preset assigned creates `GP_<Zone>` under `/Game/Grungify/Presets` and assigns it.
    - Changing a value and pressing Load Preset brings the saved value back (one undo step).
    - A palette override, such as dust colour, shows on the owned material instances after Preview, Apply, Clear → Apply, and Rebuild Plugin Content.
12. **UV and slot reports (v0.3.0):**
    - A mesh with stacked UV1 islands shows Mask UV `failed` with an overlap percentage, and Auto output falls back to vertex colours.
    - Slot Details explain an excluded deck slot, a keyword-classified slot and a fallback slot.
    - Run `Automation RunTests Grungify.Generation`, which includes the new `MaskUVValidation` and `TextureBakeCancellation` tests.
13. **v0.4.0, compile first:**
    - The new code uses several engine APIs that were never compiled against 5.8. Expect small fixes:
      - World Partition loading in the commandlet
      - data layer asset accessors
      - `FImageUtils::PNGCompressImageArray`
      - per-instance custom data material expressions
      - the screenshot functional test options
      - render-target drawing in runtime wear
      - World Partition actor descriptors in the commandlet (`ForEachActorDescInstance`, 5.4+ API), and map loading through `UEditorLoadingAndSavingUtils` inside a commandlet
      - Substrate expression members (`DiffuseAlbedo`, `F0`, `Background`, `Foreground`, `Mix`), custom-node additional outputs, and Get/Set Material Attributes setup in the hookup tool
      - `UStaticMesh::NaniteSettings` access in the reference scene (use the supported `Get/SetNaniteSettings` accessors)
    - Then run `Automation RunTests Grungify` and check that the existing `Grungify.State`, `Grungify.Content` and `Grungify.Generation` tests still pass. The texture-bake conservation test should be unchanged, since every new feature defaults to off.
14. **Reference scene and screenshots:**
    - Run *Tools → Grungify → Build Reference Scene*, then `Automation RunTests Project.Functional Tests` with the map open.
    - Approve the first images in *Window → Test Automation → Screenshot Comparison*. Later runs flag visual differences.
15. **Cooked build:**
    - Cook and package a game with `/Game/Grungify/Reference/L_GrungifyReference` in the maps to cook.
    - Run the packaged build with `-ExecCmds="Automation RunTests Grungify.Runtime" -unattended`.
    - `Grungify.Runtime.ReferenceSceneCooked` loads the map and checks that weathered meshes kept their owned material instances, CPD and masks after cooking.
16. **Cache and background:**
    - With Live Preview, drag a slider on a large zone. The editor stays responsive, and the Status line shows `(background)` and cache hits.
    - Move one mesh: only it regenerates.
    - Flip seed variants back and forth: the second visit is instant.
    - Delete the zone mid-generation: there's no crash and no leftover preview.
17. **Bake:**
    - Bake To Assets creates textures and MIs under `/Game/Grungify/Baked` and PNGs under `Saved/Grungify/Bakes`.
    - Edit a PNG, Reimport, and the change shows.
    - Clear restores the original materials, and the assets remain.
18. **Instanced meshes:** an HISM of crates gets different wear per instance. An ISM that already uses per-instance custom data 0–3 is reported as a conflict and left alone. Clear restores the original float count.
19. **Extended layers:** with the Wet Sewer preset, check pools at the bottom of traced paths, the water line, and moss. A lamp gets soot above it. A tagged fire gets scorch. Bolts get rust bloom. A traffic path scuffs the floor and the bottom of the walls.
20. **Scatter and Data Layers:** scatter entries place pieces only where the mask passes the threshold, in the outliner folder and data layers. Clear removes them. A zone restricted to a Data Layer skips other actors and reports why.
21. **Commandlet:** run `-run=Grungify -Operation=DryRun` on a World Partition map; it reports every zone and exits 0. Then run `-Operation=Apply -Save`.
22. **Material hookup:**
    - Run *Hook Up Materials In Level* on a level with unhooked materials. Check that it covers an ordinary material with constant pins, a *Use Material Attributes* material with layers, and a Substrate slab material.
    - Each compiles, shows weathering after Preview, and looks unchanged on meshes outside zones.
    - Backups appear under `/Game/Grungify/Backups`.
    - *Remove Grungify* restores the original graphs. `Grungify.Content.MaterialHookupRoundTrip` checks this automatically for the legacy path.
    - Run it on a material that's open in the material editor.
23. **Substrate:**
    - In a Substrate project, the content build reports no compile-check note or errors for `MF_GrungifySubstrate`.
    - A hooked-up Substrate material shows chips, dust, runoff and the extended layers, with a look close to the legacy function on the reference scene.
    - *Substrate Material Complexity* view shows one extra closure.
24. **Look strength (v0.6):** on the reference scene, compare v0.5.1 captures against v0.6 at Contrast 0.6, 1 and 2. Check that broad surfaces and runoff read clearly at 1, and that untouched meshes are unchanged.
25. **Skeletal meshes:**
    - A skeletal prop with UV1 and one with only UV0 both report `texture`, and the UV0 one notes the fallback.
    - Both show wear that follows the mesh when it's moved or animated.
    - Clear restores their materials and custom data.
26. **Landscape:**
    - A zone over a landscape reports a `landscape` row, binds its components and shows dust and contact wear at wall bases once the landscape material is hooked up.
    - Clear and Cancel Preview restore the original overrides.
    - A second overlapping zone leaves those components alone.
27. **Height and displacement:**
    - With a height map in Height and `Grungify Height Blend` at 1, chips move to brick faces and dust into the joints.
    - On a Nanite-tessellated material routed through `MF_GrungifyDisplacement`, `Grungify Displacement Depth` cuts the chips in.
28. **Installed copy:** install the packaged plugin into an engine. The content build validates instead of rebuilding, and changing the CPD index explains how to get a writable copy.
29. **Engine range:** build with `Scripts/BuildPlugin.ps1` for 5.5, 5.6, 5.7 and 5.8. Each must have no warnings, then run the automation tests in each.
30. **Art direction (v0.7):**
    - At default values, captures match v0.6.1 exactly.
    - On the pressure-hall coffer, compare Chip Width 0.4 / Coverage 0.35 / Breakup 0.7 and Surface Protection 0.25 against the v0.6.1 "balanced" diagnostic. You should see a narrow edge with occasional chips, intact centres and no flat cap patch.
31. **Coverage report:**
    - Slot Details report Area and per-channel Coverage.
    - On the coffer with its painted gate, Dust and Chips coverage should land near the gate's ~5.8% permitted area, not the global mean.
    - `Grungify.Generation.SlotCoverage` passes.
32. **Inspection:**
    - `Grungify.Inspection.ReadOnlyComponentState` passes.
    - From Python, inspect an applied, a previewed and an excluded-slot component, then save, reload and inspect again. The live and default CPD, owned instance paths and texture paths should resolve.

33. **Landscape (v0.8):** a zone smaller than its landscape shows no wear beyond the rectangle. The content report shows the landscape and displacement compile probes passing.
34. **Hookup edits:** `Grungify.Content.MaterialHookupPreservesArtistEdits` passes. By hand: Add, insert a node upstream of Base Color, Remove. Your node still feeds Base Color.
35. **Portable bake:** in the BuildPlugin HostProject, `Grungify.Bake.PortableAndCompatibility` passes. Assign a baked instance to a fresh copy of the mesh and it looks the same.
36. **Background snapshots:** `Grungify.Background.SnapshotsAndCancellation` passes. Drag a mesh while a live preview is generating: the preview restarts, and no stale masks appear.
37. **Runtime wear:**
    - `Grungify.RuntimeWear.SelectedChannelAccumulationGPU` passes on a rendering RHI (D3D12, Vulkan, Metal). It paints all eight layers over nonzero masks and checks every channel by readback.
    - In PIE, footsteps with Handling and Moss show up, and the other layers don't change.

38. **Quick start:** `Grungify.QuickStart.WeatherSelectedActors` passes. In a level, select three props, right-click → *Weather with Grungify*, accept the hookup. The props preview weathered and their neighbours don't. A single Ctrl+Z removes the zone.
39. **Shaders offline:** `python3 Scripts/CheckShaders.py` (needs `glslang-tools`) prints `SHADERS OK`. CI runs it on every push.

## Packaging for Fab (5.5–5.8)

See `Docs/Fab.md` for the full checklist, listing draft and media shot list, and `Docs/EngineCompatibility.md` for the per-version API audit.

1. **Generate content with the oldest supported engine (5.5).** Content saved by 5.8 won't open in 5.5–5.7.
   - Run *Tools → Grungify → Update Plugin Content*, then *Build Showcase Map*.
   - Commit everything in `Content/`.
2. **Build and package every engine version** with `Scripts/BuildPlugin.ps1` (Windows: Win64, plus Linux with a cross toolchain) or `Scripts/BuildPlugin.sh` (Mac/Linux).
   - The scripts run `BuildPlugin -StrictIncludes` for each version, fail on any compiler warning, and produce a source-only zip per version with `EngineVersion` set.
3. **Run `python3 Scripts/CheckFabReadiness.py`.** It checks copyright headers, descriptor URLs, prebuilt assets and the engine version they were saved with, the icon, and source hygiene. It currently reports that the URLs, the prebuilt content and the showcase map are still to do.
4. **Fill in the descriptor URLs.** `DocsURL`, `SupportURL`, `FabURL` and `CreatedByURL` in `Grungify.uplugin` are empty on purpose. Then set `IsBetaVersion` to false for release.
5. **Replace the icon.** `Resources/Icon128.png` is a generated placeholder; swap in real art.

**Engine copies are read-only.** When Grungify is installed in the engine (from Fab), it validates its shipped content instead of rebuilding it. That content was built for CPD index 0, UV channel 1 and instance data index 0. A project that changes those settings is told to copy the plugin into its own `Plugins` folder, where writable content can be rebuilt. `BuildPlugin` also sets `Installed=true` on packaged project plugins; that metadata alone does not forbid rebuilding their content. Read-only files still refuse regeneration.

## Layout

```
Grungify.uplugin
Shaders/Private/Grungify.ush          Material-side noise and shading
Source/GrungifyShaders/               Maps /Plugin/Grungify for the shader include (PostConfigInit)
Source/Grungify/                      Runtime: zone actor, settings, mask generation, wear stamps, traffic paths,
                                      Blueprint library, runtime wear subsystem, cooked-build test
Source/GrungifyEditor/                Asset builder, material hookup, material library, reference scene, zones panel, commandlet,
                                      viewport visualizer, Tools menu
```
