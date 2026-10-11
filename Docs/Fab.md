# Selling Grungify on Fab: checklist and listing kit

This is a working checklist for getting Grungify onto Fab for **UE 5.5, 5.6, 5.7 and 5.8**: Win64 at minimum, plus Mac
and Linux if they build cleanly.

> **Verify against Fab's current published guidelines before you submit.** The technical requirements below are my
> understanding of Fab's code-plugin rules, which carried over most of the Unreal Marketplace rules. Fab revises its
> guidelines, and I could not check the live pages while writing this. Whenever this document and Fab's
> guidelines disagree, Fab's guidelines win.

Related files:

- `Docs/EngineCompatibility.md`: per-version API audit and the source edits still to make.
- `Scripts/BuildPlugin.ps1` (Windows) and `Scripts/BuildPlugin.sh` (Mac/Linux): build every version, fail on any
  warning, and produce the upload zips.
- `Scripts/CheckFabReadiness.py`: static checks (copyright lines, descriptor, prebuilt content and which engine saved
  it, icon, hygiene, Build.cs).

---

## 1. Technical requirements (as understood; verify)

| # | Requirement | How we meet it / status |
|---|---|---|
| 1 | One upload per supported engine version. Each one is a zip of the plugin folder with source, without `Binaries/` or `Intermediate/`. Epic builds the binaries. | The build scripts write `Saved/FabBuild/Grungify_<VersionName>_UE5.x.zip`. Each zip has a single top folder, `Grungify/`, containing `Source/`, `Content/`, `Shaders/`, `Resources/`, `Config/` (if present) and `Grungify.uplugin`. |
| 2 | The plugin compiles with **no errors and no warnings** through `RunUAT BuildPlugin` on every declared engine version and platform. | The scripts run BuildPlugin with `-StrictIncludes` (no PCH, no unity) and fail on any `warning C####`, `: warning:`, `error …` or linker error. |
| 3 | The `.uplugin` has `EngineVersion` set to the version of that upload, plus a valid description, category, author, docs URL and support URL. | The scripts set `"EngineVersion": "5.x.0"` in the staged copy only. Fill in `DocsURL`, `SupportURL`, `FabURL`/`MarketplaceURL` and `CreatedByURL` (all empty now). Set `IsBetaVersion` to `false` for release. |
| 4 | Every source file starts with a copyright notice in the seller's name (not Epic's). | `// Copyright Fallen Signal Studios. All Rights Reserved.` The checker enforces it and currently passes. |
| 5 | A plugin icon at `Resources/Icon128.png` (128×128). | **Missing.** Add one; the checker fails without it. |
| 6 | Content works on every listed version: assets must load in the oldest engine you list. | **Save `Content/` with UE 5.5** (section 3). The checker reads each asset's SavedByEngineVersion and fails if it is newer than the target. |
| 7 | No unused, placeholder, or third-party/Epic-owned assets shipped without rights. Plugin content references only `/Grungify/...` and `/Engine/...`. | The generated assets are ours. The showcase map is documented to use only plugin and `/Engine` content; check with *Reference Viewer* before packaging. |
| 8 | No absolute paths, developer names, debug code, `#pragma optimize` or `PRAGMA_DISABLE_OPTIMIZATION` left in. | The checker's hygiene section. It currently passes. |
| 9 | Documentation link that works without logging in. | Needs hosting (public README page or docs site). Put the URL in `DocsURL` and the listing. |
| 10 | Support contact (email or URL). | Put it in `SupportURL` and the listing. |
| 11 | Plugin works in a fresh project: enable, restart, use, with no manual setup beyond what the docs describe. | Test matrix in section 5. The plugin creates missing assets on first start; with the prebuilt content shipped it should not need to. Check this (section 3, step 7). |
| 12 | Listing technical details are accurate (modules, platforms, versions, network replication, input). | Draft in section 6. |
| 13 | Declared platforms are the ones actually tested. | Declare Win64. Add Mac and Linux only once `BuildPlugin.sh`/`.ps1` passes for them **and** the showcase renders correctly there (section 5). |

Descriptor decisions (details in `Docs/EngineCompatibility.md` section 5):

- `SupportedTargetPlatforms` and per-module `PlatformAllowList` are deliberately unset. Restricting the runtime module
  would break projects that cook for other platforms with zones in their levels.
- If Fab's current rules demand an allow-list, add one to `GrungifyEditor` only.

---

## 2. One-time setup per build machine

- **Windows:**
  - Install UE 5.5, 5.6, 5.7 and 5.8 from the Epic launcher. The script finds them through the launcher registry or `C:\Program Files\Epic Games\UE_5.x`; otherwise set `UE_5_5_ROOT` … `UE_5_8_ROOT`.
  - Visual Studio 2022 with the C++ game development workload and the MSVC/Windows SDK versions each engine's release notes ask for.
  - For Linux binaries from Windows, install the cross-compile toolchain matching each engine version. Point `LINUX_MULTIARCH_ROOT_5_5` … `LINUX_MULTIARCH_ROOT_5_8` at them, or `LINUX_MULTIARCH_ROOT` if one toolchain serves all.
- **Mac:**
  - Install the engines in `/Users/Shared/Epic Games/UE_5.x` (launcher default) or set `UE_5_x_ROOT`.
  - Install the Xcode version each engine requires. Older engines may need an older Xcode; switch with `xcode-select`.
- **Linux:** source-built engines. Set `UE_5_x_ROOT` or pass `--engine 5.6=/path/to/UE_5.6`.
- Python 3.8+ on every machine (the checker, and the bash script's zip/descriptor steps).

---

## 3. Produce the prebuilt content (before every release)

The plugin generates its material assets in code. They must be generated, saved and committed in `Content/` **before**
packaging, and saved by the **oldest** supported engine (5.5) so all four versions can load them.

1. Create a throwaway **UE 5.5** Blank C++ project (no starter content). Keep the default project settings, because the
   generated assets record the custom-data and UV indices (`[GrungifyContent v6 cpd=0 uv=1 inst=0]`).
2. Copy the repo into `<Project>/Plugins/Grungify`. Leave out `Binaries/` and `Intermediate/`.
3. Build the editor target and open the project.
4. Run **Tools > Grungify > Update Plugin Content**. Use **Rebuild Plugin Content** if the assets already exist from
   another engine version.
   - Check the Output Log and `Saved/Grungify/ContentReport.txt` for `ERROR` lines.
   - Check that `Saved/Grungify/ContentReady.txt` exists.
   - The expected assets are `BP_WeatheringZone`, `Materials/MF_GrungifyWeathering`, `Materials/MF_GrungifySubstrate`,
     `Materials/M_GrungifyMaskDebug`, `Materials/M_GrungifyExample` and `Materials/T_GrungifyNoMask`.
5. Run **Tools > Grungify > Build Showcase Map**. This creates `/Grungify/Showcase/L_GrungifyShowcase` plus its
   material instances and presets in `/Grungify/Showcase`. Open the map, check every bay, and save.
6. Copy `<Project>/Plugins/Grungify/Content/` back over the repo's `Content/` and commit it. `Content/` is not in
   `.gitignore`; consider Git LFS for `*.uasset` and `*.umap`.
7. **Check the "already up to date" path on every engine version.**
   - Open a 5.6, 5.7 and 5.8 project with the committed content.
   - Confirm the plugin does **not** rebuild or resave its content on startup. The log should report no
     "content rebuilt" line.
   - A Fab install lives in the engine's plugin folder, which may be read-only. A rebuild there would also re-save the
     assets with the newer engine.
   - If it does rebuild, find out why (the version marker, the settings indices or the `ContentReady.txt` token)
     before shipping.
8. Run `python3 Scripts/CheckFabReadiness.py`. Every section must pass, except the URL fields until the listing exists.

If you choose per-version content instead (each version generates and saves its own):

- Put it under `<dir>/5.5/`, `<dir>/5.6/` and so on, with the same layout as `Content/`.
- Pass `-ContentRoot <dir>` (PowerShell) or `--content-root <dir>` (bash).

---

## 4. Build and package each version

Windows (primary). This builds Win64, plus Linux when a cross toolchain is configured:

```powershell
# all four versions, engines auto-discovered
powershell -ExecutionPolicy Bypass -File Scripts\BuildPlugin.ps1

# one version, explicit engine
powershell -ExecutionPolicy Bypass -File Scripts\BuildPlugin.ps1 -EngineVersions 5.6 -EngineRoot "D:\Engines\UE_5.6"

# several, explicit roots and platforms
pwsh Scripts\BuildPlugin.ps1 -EngineVersions 5.5,5.8 -EngineRoots @{ '5.5'='D:\UE_5.5'; '5.8'='D:\UE_5.8' } -TargetPlatforms Win64+Linux
```

Mac or Linux (builds the host platform):

```bash
Scripts/BuildPlugin.sh                                  # 5.5 5.6 5.7 5.8
Scripts/BuildPlugin.sh --versions "5.7 5.8" --engine 5.7=/opt/UE_5.7 --engine 5.8=/opt/UE_5.8
```

Each version goes through these steps:

1. Stage a clean copy and set `EngineVersion`.
2. Run the static checks: `Saved/FabBuild/<ver>/FabReadiness.txt`.
3. Run `RunUAT BuildPlugin -StrictIncludes`: `Saved/FabBuild/<ver>/BuildPlugin.log`. Any compiler warnings and errors
   are listed in `CompilerWarnings.txt` and `CompilerErrors.txt`.
4. Zip: `Saved/FabBuild/Grungify_<VersionName>_UE<ver>.zip`. A zip is only written when the build passed.

A summary table follows, and the script exits non-zero if any version failed. Useful switches:

- `-SkipBuild` / `--skip-build` stages and zips only.
- `-KeepStaging` / `--keep-staging` keeps the staged copy for inspection.

The zip is built from the staged source, not from BuildPlugin's `-Package` output, so build products never leak in.
For Mac binaries, run the bash script on a Mac **in addition to** the Windows run. The zips have identical content;
the Mac run only proves that the code compiles there.

---

## 5. Test matrix (per engine version, before upload)

Do this in a **fresh** project of each version (5.5, 5.6, 5.7, 5.8):

- [ ] Unzip the version's zip into `<Project>/Plugins/`, generate project files, and build the Development Editor
      target. Expect no warnings.
- [ ] Enable the plugin, restart, and confirm there are no errors or "content rebuilt" lines in the log (section 3,
      step 7).
- [ ] Open `/Grungify/Showcase/L_GrungifyShowcase`. Every bay renders, there are no material compile errors, and the
      cameras frame correctly.
- [ ] Place `BP_WeatheringZone` on a test level. Preview, Apply, Clear and Undo work.
- [ ] Tools > Grungify > Hook Up Materials on a copy of a Starter Content material. Then repeat with Substrate
      enabled (Project Settings > Rendering > Substrate) and `MF_GrungifySubstrate`.
- [ ] Run *Session Frontend > Automation* `Grungify.*`; all pass.
- [ ] Package a Win64 Shipping game containing the showcase map. It launches, and the weathering is visible (cooked
      masks).
- [ ] Mac (if declared): the editor build passes and the showcase renders under Metal.
- [ ] Linux (if declared): the editor build passes and the showcase renders under Vulkan.
- [ ] Commandlet smoke test:
      `UnrealEditor-Cmd <proj> -run=Grungify -Map=/Grungify/Showcase/L_GrungifyShowcase -Operation=DryRun -unattended -nullrhi`
      exits 0.

---

## 6. Store listing draft

**Title:** Grungify: Procedural Weathering Zones

**Short description** (one line):
Drop a zone, press Apply: procedural dust, runoff, edge chips and handling wear that follow how your level is built
and used.

**Long description:**

Grungify adds believable wear to your levels without hand painting. Place a **Weathering Zone**, size its box and press
**Preview**, **Apply** or **Clear**. The zone finds every static mesh it overlaps and bakes four kinds of weathering into
them, each driven by how the space is built and used:

- **Edge chips** on exposed, convex edges, strongest where traffic kicks them.
- **Dust at feet** that builds up from the traced floor, on ledges and in creases, and works across storeys and stairs.
- **Runoff below vents**: streaks running from vents, drains and spouts, fanning out as they fall.
- **Handling on rails**: polish and grime on rails, handles and edges at hand height.

Each material slot is classified as stone, metal or trim, so the wear follows the construction. Trim chips, metal
rusts under runoff and polishes under hands, and stone collects dust. Everything is seeded, so the same seed always
gives the same result.

Features:

- **One-click material hookup.** Grungify wires itself into your master materials (legacy, Material Attributes and
  **Substrate**), with automatic backups and one-click removal.
- **Nanite and sparse meshes.** Choose per-instance mask textures or vertex colours. The Auto mode picks the right one.
- **Optional extended layers.** Moisture and water lines, soot above lamps and fires, scorch, moss and lichen, rust
  bloom around fasteners, cracks and traffic scuffs.
- **Light- and sky-aware exposure, and traced water flow** for runoff that follows real geometry.
- **Decal and mesh scatter, instanced meshes (ISM/HISM), and wear stamps** for art-directed touch-ups.
- **Non-destructive workflow.** Preview before applying, full undo, seed variants for A/B comparison, and a background
  generation cache.
- **Bake to assets.** Masks are exported as textures and PNGs, so you can paint over them in any image editor and
  reimport.
- **Presets and a starter material library**: Abandoned Industrial, Old Temple and Wet Sewer.
- **Weathering Zones panel**, listing every zone in the level with its status and batch actions.
- **Data Layers and World Partition.** Target zones by Data Layer. A commandlet processes whole World Partition maps
  on build machines or CI.
- **Blueprint and Python API** for every zone action, plus level-wide helpers and JSON reports.
- **Runtime wear.** A world subsystem and component paint impacts, footsteps and scorch marks into weathered meshes
  during gameplay.
- **Showcase map** included.

**Technical details** (fill in the bracketed items):

| Field | Value |
|---|---|
| Features | See the feature list above |
| Code modules | `GrungifyShaders` (Runtime, PostConfigInit: maps the plugin's shader folder), `Grungify` (Runtime: zone actor, mask generation, runtime wear subsystem, Blueprint library), `GrungifyEditor` (Editor: asset builder, material hookup, zones panel, commandlet, showcase builder) |
| Number of Blueprints | 1 (`BP_WeatheringZone`), plus the showcase map |
| Number of C++ classes | [count at release] |
| Input | None. The plugin defines no input bindings. Runtime wear is driven from your own gameplay code or Blueprints. |
| Network replicated | No |
| Supported development platforms | Windows (Win64) [add Mac and Linux once verified] |
| Supported target build platforms | Win64 [add Mac and Linux once verified; the runtime module has no platform-specific code] |
| Supported engine versions | 5.5, 5.6, 5.7, 5.8 |
| Documentation | [DOCS URL: public page, the same as DocsURL in the .uplugin] |
| Example project / map | `/Grungify/Showcase/L_GrungifyShowcase` (included in the plugin) |
| Important notes | Weathering is baked in the editor. Texture-output masks need a non-overlapping UV channel. Runtime wear needs *Support UV From Hit Results* and complex-collision hits. Substrate support needs Substrate enabled in the project. |
| Support | [SUPPORT URL or email, the same as SupportURL in the .uplugin] |

**Tags (suggestions):** weathering, grunge, procedural, materials, environment, level design, decals, Substrate, Nanite,
tools

---

## 7. Media shot list

General rules:

- Capture in the **showcase map**.
- Use fixed exposure, 16:9, at least 1920×1080 (2560×1440 or 4K preferred).
- Show nothing from third-party asset packs unless you have the rights. Use plugin and `/Engine` content only.
- Check Fab's current image count, size and thumbnail requirements.

**Screenshots** (in listing order):

1. **Hero / thumbnail.** A wide shot of a weathered bay, with the plugin name added in post. The Fab thumbnail is
   cropped, so keep the subject centred.
2. **Before / After split.** The same camera (the Before / After bay's `Cam_<Bay>`), left half clean and right half applied.
3. **The four layers.** A 2×2 grid of the mask debug view, one panel each for edge chips, dust, runoff and handling.
4. **Runoff under vents.** A close-up of streaks fanning down a wall below a vent.
5. **Dust at feet and on stairs.** Dust following the traced floor across a stair or second storey.
6. **Handling on rails.** Polish and grime on a metal rail at hand height.
7. **Extended layers.** The Fire & Soot bay: soot above a lamp or fire, scorch, moss in a damp corner.
8. **Library looks.** Abandoned Industrial, Old Temple and Wet Sewer side by side.
9. **Instanced meshes and wear stamps.** The ISM/HISM bay with a stamp gizmo visible.
10. **Editor UI.** The zone Details panel (Preview / Apply / Clear, Seed), with the Weathering Zones panel docked.
11. **Material hookup.** The Content Browser right-click "Add Grungify" next to a material graph showing the inserted
    function call (legacy and Substrate).
12. **Bake to assets.** Exported PNG masks open in an image editor next to the reimported result.
13. **Blueprint / Python.** A short Python snippet (`run_all_zones`) and an Editor Utility Blueprint calling `RunZone`.

**Video storyboard** (about 75 seconds, 1080p60 or 4K30, music with no voice-over, short captions):

| Time | Shot | Caption |
|---|---|---|
| 0:00–0:05 | Slow dolly through a fully weathered bay; the title fades in. | Grungify: procedural weathering zones |
| 0:05–0:12 | Clean bay. Drag `BP_WeatheringZone` in and scale the box. | Drop a zone |
| 0:12–0:20 | Press **Preview**; wear appears. Press **Apply**. | Preview. Apply. |
| 0:20–0:30 | Mask debug view cycles through chips → dust → runoff → handling, then cuts back to shaded. | Four layers that follow how the space is used |
| 0:30–0:37 | Change the Seed three times quickly: variants A, B, C. | Seeded and repeatable |
| 0:37–0:45 | Extended layers turn on: soot above a lamp, moss, rust bloom, cracks. | Extended layers |
| 0:45–0:52 | Content Browser right-click **Add Grungify** on a Substrate material; the graph updates. | One-click material hookup, Substrate included |
| 0:52–0:58 | Nanite mesh with texture masks; the ISM bay; a wear stamp dragged over a wall. | Nanite, instances, wear stamps |
| 0:58–1:05 | PIE in a test level with `UGrungifyRuntimeWearComponent` on the player: footsteps and impacts paint runtime wear onto a floor. | Runtime wear |
| 1:05–1:10 | Zones panel with *Apply All*; commandlet output in a terminal. | Scales to whole levels and CI |
| 1:10–1:15 | Before/After wipe on the hero shot; logo and "UE 5.5 – 5.8". | Grungify, on Fab |

Capture tips:

- Use Sequencer with the showcase's `Cam_*` cameras so shots can be reproduced for later versions.
- Lock exposure and turn off motion blur for the UI segments.
- Record UI shots at 100% DPI scale so the text stays legible.

---

## 8. Final pre-submit checklist

- [ ] The source edits in `Docs/EngineCompatibility.md` section 4 are applied, or confirmed unnecessary by a clean
      compile.
- [ ] `Content/` was generated with UE 5.5 and committed. Startup on 5.6, 5.7 and 5.8 does not rebuild it.
- [ ] `Resources/Icon128.png` is added.
- [ ] `DocsURL`, `SupportURL`, `FabURL`/`MarketplaceURL` and `CreatedByURL` are filled in, and `IsBetaVersion` is
      `false`.
- [ ] `VersionName`/`Version` are bumped for the release by the plugin owner. The scripts never touch them.
- [ ] `python3 Scripts/CheckFabReadiness.py` passes.
- [ ] `Scripts/BuildPlugin.ps1` passes for 5.5, 5.6, 5.7 and 5.8 with zero warnings. The Mac run passes too, if Mac is
      declared.
- [ ] The test matrix (section 5) is done for each version.
- [ ] README is updated per `Docs/EngineCompatibility.md` E7, and the docs page is public.
- [ ] Listing text, technical details, screenshots and video are uploaded. Each version's zip is uploaded against the
      matching engine version.
- [ ] Re-read Fab's current seller and technical guidelines one last time.
