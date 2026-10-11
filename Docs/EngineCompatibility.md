# Engine compatibility: UE 5.5 to 5.8

Grungify was written against UE 5.8. Fab needs a separate package for each engine version we sell, so this page checks
every engine API the plugin calls and records how it behaves on 5.5, 5.6, 5.7 and 5.8.

**Nothing on this page was compiled or checked against engine source.** The audit was done without an engine install.
Per the README, v0.4.0 and v0.5.0 have not been compiled on 5.8 either. Every status below comes from memory of the
engine API. Read the confidence column like this:

- **high**: a stable API that has been in place since 5.3 or earlier and that I have no reason to think moved.
- **medium**: I believe the status is right, but it sits in an area that changes often.
- **low**: a guess. Run `Scripts/BuildPlugin.ps1 -EngineVersions 5.5` first and believe the compiler, not this table.

Rows marked **verify on first compile** are the ones to watch in the first build log for each version.

---

## 1. Top risks (read first)

| # | Risk | Versions | Confidence | What to do |
|---|---|---|---|---|
| 1 | **Content saved by 5.8 cannot be opened by 5.5, 5.6 or 5.7.** Unreal packages are not backward compatible, so a `.uasset`/`.umap` saved by a newer editor fails to load in an older one. This is not an API question; it is how package versioning works. | 5.5 to 5.7 | **high** | Generate and save `Content/` with **UE 5.5** (the oldest target) and ship that copy in all four zips. The other option is to generate per version and pass `-ContentRoot`/`--content-root` (layout `<root>/<ver>/...`). `CheckFabReadiness.py --max-content-engine <ver>` reads each asset's SavedByEngineVersion and fails if it is newer. The build scripts already run it per version. |
| 2 | `AActor::AddDataLayer(const UDataLayerAsset*)` may not be public on any of these versions. My recollection is that it is private and called through `UDataLayerInstance::AddActor`. | all, 5.8 included | **low** | Verify on first compile. If it fails (C2248 / "is a private member") or warns (C4996), apply edit E5 below. |
| 3 | `UStaticMesh::NaniteSettings` direct member access may be deprecated in favour of accessors in some versions. | 5.5 to 5.8 | **medium** | Use `Grungify::Compat::SetNaniteEnabled` (edit E1). It suppresses the deprecation, and one switch moves it to the accessors. |
| 4 | `UMaterial::GetMaterialResource(EShaderPlatform)`. I believe this overload exists on 5.5, with the `ERHIFeatureLevel` one as the older form, but I am not sure in which version the switch happened. | 5.5 (5.6 less likely) | **medium** | Use `GRUNGIFY_GET_MAX_RHI_MATERIAL_RESOURCE` (edits E2/E3). If 5.5 reports "cannot convert EShaderPlatform", add `PublicDefinitions.Add("GRUNGIFY_MATERIAL_RESOURCE_BY_SHADER_PLATFORM=0")` for that engine. |
| 5 | Strict includes, unity off and IWYU differ per version. Engine headers stop including each other transitively between versions, so a file can compile on 5.8 but not on 5.5, or the other way round. | all | **medium** | The build scripts pass `-StrictIncludes`. Add the missing `#include` the compiler names. Likely candidates: `GMaxRHIShaderPlatform` (`RHIGlobals.h`, otherwise `RHI.h`), `FDrawToRenderTargetContext` (`Kismet/KismetRenderingLibrary.h`, already included). |
| 6 | Clang (Mac, Linux) warns about things MSVC accepts: shadowed variables (`-Wshadow`, an error under UE's default `ShadowVariableWarningLevel`), unused-but-set variables, `-Wrange-loop-construct`, implicit float/double conversions in `FMath` calls, and the order of `override`/`final`. | Mac, Linux | **medium** | Build Linux from Windows with the cross-compile toolchain (`LINUX_MULTIARCH_ROOT[_5_x]`), and build Mac on a Mac with `Scripts/BuildPlugin.sh`. Fix whatever the scripts report; any warning fails the run. |
| 7 | HLSL in `Shaders/Private/Grungify.ush` and in the generated custom nodes compiles through DXC to SPIR-V (Vulkan, Linux) and Metal (Mac). Integer `asuint`/hash code is portable, but implicit `float`↔`int` truncation and `uint` overflow constants are worth a look. | Mac, Linux | **medium** | Open `M_GrungifyExample`, `M_GrungifyMaskDebug` and the showcase map on a Mac and on Linux (Vulkan SM5/SM6) and check that no material errors appear in the log. |
| 8 | The Substrate default differs per version: projects created on older versions have Substrate off, and newer templates may have it on. This is behaviour, not compilation; `MF_GrungifySubstrate` builds either way, but the compile check only runs when Substrate is enabled. | 5.5 to 5.8 | **medium** | Test the Substrate hookup on each version in a project with Substrate turned on. |

---

## 2. API audit

Columns 5.5, 5.6, 5.7 and 5.8 give the expected status: OK means it compiles as written today; Guard means use the compat
wrapper; ? means verify on first compile.

### World Partition, Data Layers (runtime module, editor-only code)

| API (where used) | 5.5 | 5.6 | 5.7 | 5.8 | Conf. | Notes / recommendation |
|---|---|---|---|---|---|---|
| `FWorldPartitionHelpers::ForEachActorDescInstance<T>(UWorldPartition*, TFunctionRef<bool(const FWorldPartitionActorDescInstance*)>)` (`GrungifyCommandlet.cpp` `ProcessWorldPartitionMap`, ~l.272) | OK | OK | OK | OK | medium | Added in 5.4, when actor desc *instances* split from descs; 5.3 used `ForEachActorDesc` with `FWorldPartitionActorDesc`. Our minimum is 5.5, so no guard is needed. Verify on first compile on 5.5. |
| `FWorldPartitionActorDescInstance::GetActorLabel()` / `GetGuid()` / `GetEditorBounds()` | OK | OK | OK | OK | medium | `GetEditorBounds` replaced `GetBounds` in 5.2. The instance forwards these from the desc. Verify on first compile. |
| `FLoaderAdapterShape(UWorld*, const FBox&, const FString&)` + `Load()` (`GrungifyCommandlet.cpp` ~l.292) | OK | OK | OK | OK | medium-high | Constructor signature unchanged since 5.0, as far as I know. |
| `AActor::GetDataLayerAssets()` (`GrungifyWeatheringZone.cpp` `ShouldConsiderActor`, ~l.1207; in `WITH_EDITOR`) | OK | OK | OK | OK | medium | Editor-only. A `bool bIncludeExternalDataLayerAsset = true` parameter came with External Data Layers, but it has a default, so the call compiles unchanged. |
| `AActor::AddDataLayer(const UDataLayerAsset*)` (`GrungifyWeatheringZone.cpp` `ScatterFromResults`, ~l.2250) | ? | ? | ? | ? | **low** | See risk 2 and edit E5. |
| `UDataLayerAsset`, `TArray<TObjectPtr<const UDataLayerAsset>>` UPROPERTYs (`GrungifyWeatheringZone.h`) | OK | OK | OK | OK | high | The engine itself uses the same property type on `AActor` since 5.1. |

### Static meshes, Nanite, render data

| API | 5.5 | 5.6 | 5.7 | 5.8 | Conf. | Notes / recommendation |
|---|---|---|---|---|---|---|
| `UStaticMesh::NaniteSettings.bEnabled = true` (`GrungifyReferenceScene.cpp` `MakeNaniteCube`, l.126) | Guard | Guard | Guard | Guard | medium | Use `Grungify::Compat::SetNaniteEnabled(Mesh, true)` (edit E1). The member has been public since 5.0. Whether it is deprecated in favour of `Get/SetNaniteSettings()` in 5.6+ I don't know for certain, so the wrapper suppresses the deprecation, and `GRUNGIFY_STATICMESH_NANITE_ACCESSORS=1` moves it to the accessors if the member ever goes private. |
| `UStaticMesh::HasValidNaniteData()` (`GrungifyMaskGeneration.cpp`) | OK | OK | OK | OK | high | |
| `GetRenderData()->LODResources`, `FStaticMeshLODResources(bool bAddRef)`, `VertexBuffers.*`, `IndexBuffer.GetCopy/SetIndices(…, EIndexBufferStride::Force32Bit)` | OK | OK | OK | OK | medium-high | `FStaticMeshLODResources(false)` is used for stack-owned test fixtures. Its constructor parameter has been there since the intrusive refcount arrived (4.26/5.0). |
| `FStaticMeshComponentLODInfo::OverrideVertexColors`, `ReleaseOverrideVertexColorsAndBlock`, `PaintedVertices`, `UStaticMeshComponent::SetLODDataCount` (`GrungifyComponentState.cpp`) | OK | OK | OK | OK | high | Editor-only paths, all inside `WITH_EDITOR`. |
| `BeginInitResource(FRenderResource*)` | OK | OK | OK | OK | high | 5.3 added an optional `FRenderCommandPipe*` parameter with a default. |
| `UStaticMesh::GetStaticMaterials()`, `UMeshComponent::GetMaterialSlotNames()` | OK | OK | OK | OK | high | |

### Materials (editor module)

| API | 5.5 | 5.6 | 5.7 | 5.8 | Conf. | Notes / recommendation |
|---|---|---|---|---|---|---|
| `UMaterial::GetEditorOnlyData()` → `UMaterialEditorOnlyData` (`FrontMaterial`, `MaterialAttributes`, …) | OK | OK | OK | OK | high | Added in 5.1. |
| `UMaterial::GetExpressions()` / `UMaterialFunction::GetExpressions()` | OK | OK | OK | OK | high | 5.1+, returns `TConstArrayView<TObjectPtr<UMaterialExpression>>`. The snapshot-then-delete pattern in `ClearFunctionExpressions` is correct on every version. |
| `UMaterial::GetMaterialResource(GMaxRHIShaderPlatform)` (`GrungifyContentBuilder.cpp` `CompileAndCheck` ~l.668; `GrungifyMaterialHookup.cpp` `CompileErrors` ~l.210) | ? | OK | OK | OK | medium | Risk 4. Use the `GRUNGIFY_GET_MAX_RHI_MATERIAL_RESOURCE` macro (edits E2, E3). |
| `FMaterialResource::FinishCompilation()`, `GetCompileErrors()` | OK | OK | OK | OK | high | |
| `GMaxRHIShaderPlatform` via `RHIGlobals.h` | OK | OK | OK | OK | medium | `RHIGlobals.h` exists since 5.2. If a strict-includes build cannot see the symbol, include `RHI.h`. |
| `UMaterialEditingLibrary::CreateMaterialExpression`, `CreateMaterialExpressionInFunction`, `DeleteMaterialExpression`, `DeleteMaterialExpressionInFunction`, `ConnectMaterialExpressions`, `ConnectMaterialProperty`, `RecompileMaterial`, `UpdateMaterialFunction` | OK | OK | OK | OK | high | Stable Editor Scripting API. |
| `UMaterialExpressionCustom::AdditionalOutputs` (`TArray<FCustomOutput>`, `{FName, ECustomMaterialOutputType}` aggregate init), `CMOT_Float1/2/3`, `IncludeFilePaths`, `Inputs` (`FCustomInput`) | OK | OK | OK | OK | high | Unchanged for many versions. `PostEditChangeProperty` on `AdditionalOutputs` rebuilds the outputs on every version. |
| `UMaterialExpressionPerInstanceCustomData` (`DataIndex`) | OK | OK | OK | OK | high | |
| `UMaterialExpressionScalarParameter::bUseCustomPrimitiveData` / `PrimitiveDataIndex` | OK | OK | OK | OK | high | |
| `UMaterialExpressionGetMaterialAttributes::AttributeGetTypes`, `MaterialAttributes`; `UMaterialExpressionSetMaterialAttributes::Inputs`, `AttributeSetTypes` | OK | OK | OK | OK | high | |
| `UMaterialExpressionFunctionInput` (`InputType`, `PreviewValue` as `FVector4f`, `bUsePreviewValueAsDefault`, `SortPriority`) | OK | OK | OK | OK | high | `PreviewValue` became `FVector4f` with LWC in 5.0. |
| `FunctionInput_Substrate` | OK | OK | OK | OK | high | Renamed from `FunctionInput_Strata` in 5.3. |
| `UMaterialExpressionSubstrateSlabBSDF` (`DiffuseAlbedo`, `F0`, `Roughness`, `Normal`), header `Materials/MaterialExpressionSubstrate.h` | OK | OK | OK | OK | medium-high | Strata→Substrate rename landed in 5.3. Slab inputs have been added over time (F90, SSS, specular profile and so on), but the four used here are stable. |
| `UMaterialExpressionSubstrateHorizontalMixing` (`Background`, `Foreground`, `Mix`) | OK | OK | OK | OK | medium-high | |
| `Substrate::IsSubstrateEnabled()` (`RenderUtils.h`) | OK | OK | OK | OK | high | 5.3+. |
| `MP_FrontMaterial`, `MP_EmissiveColor`, `MSM_Unlit`, `UMaterial::SetShadingModel` | OK | OK | OK | OK | high | |
| `UMaterialExpressionMaterialFunctionCall::SetMaterialFunction` | OK | OK | OK | OK | high | |
| `UMaterialInstanceConstant::Set*ParameterValueEditorOnly`, `ClearParameterValuesEditorOnly`, `FMaterialParameterInfo` | OK | OK | OK | OK | high | |

### Textures, render targets, images

| API | 5.5 | 5.6 | 5.7 | 5.8 | Conf. | Notes / recommendation |
|---|---|---|---|---|---|---|
| `FImageUtils::PNGCompressImageArray(int32, int32, TArrayView64<const FColor>, TArray64<uint8>&)` (`GrungifyWeatheringZone.cpp` `WriteBakedAssets`/`BakeTexture`, ~l.2439) | Guard | Guard | Guard | Guard | medium | Present since 4.26. The `TArray<uint8>` output overload was deprecated in 5.x; the `TArray64` one may follow. Use `Grungify::Compat::CompressPNG<FImageUtils>` (edit E4), which suppresses deprecation and returns success. I did **not** switch to `FImageUtils::CompressImage(…, FImageView)`, because I am not sure how it treats `EGammaSpace::Linear` BGRA8 data (it could convert to sRGB and change the masks). |
| `UTexture::Source.Init(…, TSF_BGRA8, const uint8*)`, `SRGB`, `CompressionSettings`, `MipGenSettings`, `LODGroup` | OK | OK | OK | OK | high | |
| `UTexture2D::CreateTransient`, `GetPlatformData()->Mips[0].BulkData.Lock/Unlock`, `UpdateResource` (`GrungifyRuntimeWear.cpp`) | OK | OK | OK | OK | medium-high | Runtime path. Writing mip bulk data on a transient texture is supported. |
| `UTextureRenderTarget2D` (`RenderTargetFormat`, `bForceLinearGamma`, `InitAutoFormat`, `UpdateResourceImmediate`) | OK | OK | OK | OK | medium-high | |
| `UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget/EndDrawCanvasToRenderTarget`, `FDrawToRenderTargetContext` | OK | OK | OK | OK | high | |
| `UCanvas::K2_DrawTexture(…, EBlendMode)` with `BLEND_Opaque`, `BLEND_Additive`, `BLEND_AlphaHoldout` | OK | OK | OK | OK | high | `BLEND_AlphaHoldout` exists in every UE5 version. Check visually that holdout accumulation behaves the same on Metal and Vulkan. |
| `UGameplayStatics::FindCollisionUV` | OK | OK | OK | OK | high | Needs *Support UV From Hit Results*, which is already documented. |
| `UAssetImportData::Update(File)` | OK | OK | OK | OK | high | |

### Core, async, GC, UObject

| API | 5.5 | 5.6 | 5.7 | 5.8 | Conf. | Notes |
|---|---|---|---|---|---|---|
| `FTSTicker::GetCoreTicker().AddTicker/RemoveTicker`, `FTSTicker::FDelegateHandle` | OK | OK | OK | OK | high | 5.0+ (`FTicker` was removed). |
| `FGCScopeGuard` (`UObject/GarbageCollection.h`) on a worker thread | OK | OK | OK | OK | high | |
| `Async(EAsyncExecution::ThreadPool, …)`, `TFuture::Wait`, `ParallelFor` | OK | OK | OK | OK | high | |
| `MarkAsGarbage()` | OK | OK | OK | OK | high | 5.0+. |
| `TObjectPtr`, `FVector4f`, `FVector2f`, `FVector3f` (UE::Math) | OK | OK | OK | OK | high | |
| `UPackage::PreSavePackageWithContextEvent`, `FObjectPreSaveContext` | OK | OK | OK | OK | high | 5.0+. |
| `FindFProperty`, `GET_MEMBER_NAME_CHECKED`, `ContainerPtrToValuePtr` | OK | OK | OK | OK | high | |
| `IPluginManager`, `AddShaderSourceDirectoryMapping`, `AllShaderSourceDirectoryMappings` (`GrungifyShadersModule.cpp`) | OK | OK | OK | OK | high | |
| `FJsonObjectConverter` | OK | OK | OK | OK | high | |

### Editor framework

| API | 5.5 | 5.6 | 5.7 | 5.8 | Conf. | Notes |
|---|---|---|---|---|---|---|
| `IContentBrowserSingleton::GetSelectedAssets(TArray<FAssetData>&)` (`GrungifyMaterialHookup.cpp` ~l.556) | OK | OK | OK | OK | high | |
| `IAssetTools::DuplicateAsset(Name, PackagePath, Object)` (`GrungifyMaterialHookup.cpp` ~l.249) | OK | OK | OK | OK | high | |
| `UAssetEditorSubsystem` (via `GEditor->GetEditorSubsystem`) | OK | OK | OK | OK | high | |
| `UToolMenus::ExtendMenu("LevelEditor.MainMenu.Tools")`, `"ContentBrowser.AssetContextMenu.Material"`, `"…MaterialInstanceConstant"` | OK | OK | OK | OK | medium-high | Asset context menu names follow `ContentBrowser.AssetContextMenu.<Class>`. If an entry is missing on one version, check the name with `ToolMenus.Edit 1`. Nothing fails to compile either way. |
| `FAppStyle::GetBrush("ToolPanel.GroupBorder")`, icon `"LevelEditor.Tabs.Outliner"` | OK | OK | OK | OK | medium | A renamed style just draws blank. It is cosmetic. |
| `FGlobalTabmanager::RegisterNomadTabSpawner`, `FComponentVisualizer`, `GUnrealEd->RegisterComponentVisualizer` | OK | OK | OK | OK | high | |
| `UEditorLoadingAndSavingUtils::SavePackages/LoadMap`, `FKismetEditorUtilities::CreateBlueprint/CompileBlueprint`, `GEditor->ResetTransaction` | OK | OK | OK | OK | high | |
| `AScreenshotFunctionalTest`, `FAutomationScreenshotOptions(EComparisonTolerance::Low)`, reflected access to protected `ScreenshotOptions` (`GrungifyReferenceScene.cpp` ~l.221–244) | OK | OK | OK | OK | medium-high | FunctionalTesting module. The reflection lookup checks the struct type at runtime and reports cleanly if it changes. |
| `EAutomationTestFlags::EditorContext \| EngineFilter` etc. (all tests) | OK | OK | OK | OK | medium-high | 5.5 made `EAutomationTestFlags` an `enum class` and moved the combined masks to `EAutomationTestFlags_ApplicationContextMask` and similar. OR-ing single flags, as Grungify does, compiles on all four. `GRUNGIFY_AUTOMATION_APPLICATION_CONTEXT_MASK` is in the compat header in case a mask is ever needed. |
| `AutomationOpenMap`, `FFunctionLatentCommand` (`Tests/AutomationCommon.h`) | OK | OK | OK | OK | high | |

### Landscape (new: `GrungifyZoneLandscape.cpp`, added while this audit ran)

| API | 5.5 | 5.6 | 5.7 | 5.8 | Conf. | Notes |
|---|---|---|---|---|---|---|
| `ALandscapeProxy::GetHeightAtLocation(FVector)` → `TOptional<float>` | OK | OK | OK | OK | medium | |
| `ALandscapeProxy::LandscapeComponents`, `ULandscapeComponent::OverrideMaterial`, `GetLandscapeMaterial()`, `ALandscapeProxy::UpdateAllComponentMaterialInstances()` | ? | ? | ? | ? | medium-low | Direct member writes on landscape classes are the kind of thing Epic encapsulates between versions. `UpdateAllComponentMaterialInstances` is editor-only, so keep it in `WITH_EDITOR`. Verify on first compile. If `OverrideMaterial` turns out deprecated, wrap the write the same way as the Nanite one. |

### Showcase map and editor library (`GrungifyShowcase.cpp`, `GrungifyEditorLibrary.cpp`, added while this audit ran)

| API | 5.5 | 5.6 | 5.7 | 5.8 | Conf. | Notes |
|---|---|---|---|---|---|---|
| `ATextRenderActor::GetTextRender()`, `UTextRenderComponent::SetText(FText)`, `SetTextRenderColor`, `ACameraActor`, `APlayerStart` | OK | OK | OK | OK | high | |
| `FEditorFileUtils::SaveDirtyPackages(bool, bool, bool)`, `FEditorFileUtils::SaveLevel`, `UEditorLoadingAndSavingUtils::LoadMap/SaveMap` | OK | OK | OK | OK | high | |
| Showcase map saved in plugin content (`/Grungify/Showcase/L_GrungifyShowcase`) | see risk 1 | | | | high | It is a `.umap` and subject to the same rule as all content: save it with the oldest engine you ship to. |

---

## 3. `Source/Grungify/Public/GrungifyCompat.h`

The header is header-only. It includes only `CoreMinimal.h` and `Runtime/Launch/Resources/Version.h`. It provides:

| Item | Kind | Purpose |
|---|---|---|
| `GRUNGIFY_UE_AT_LEAST(Major, Minor)`, `GRUNGIFY_UE_BELOW(Major, Minor)` | macro | Version tests built on `ENGINE_MAJOR_VERSION`/`ENGINE_MINOR_VERSION`. A `static_assert` rejects engines older than 5.5. |
| `GRUNGIFY_MATERIAL_RESOURCE_BY_SHADER_PLATFORM` (default 1) | switch | Selects `GetMaterialResource(EShaderPlatform)` or the `ERHIFeatureLevel` overload. |
| `GRUNGIFY_STATICMESH_NANITE_ACCESSORS` (default 0) | switch | Selects direct `NaniteSettings` access (deprecation suppressed) or `Get/SetNaniteSettings()`. |
| `GRUNGIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material)` | macro, `WITH_EDITOR` | Material resource for the max RHI platform. A macro so `GMaxRHI*` resolves at the call site. |
| `Grungify::Compat::SetNaniteEnabled(Mesh, bool)` / `IsNaniteSettingEnabled(Mesh)` | template, `WITH_EDITOR` | Nanite build-setting toggle. A template so the header needs no `StaticMesh.h`. |
| `Grungify::Compat::CompressPNG<FImageUtils>(W, H, Pixels, OutPng)` | template, `WITH_EDITOR` | Linear BGRA8 to PNG with deprecation suppressed. Returns false on empty output. |
| `GRUNGIFY_AUTOMATION_APPLICATION_CONTEXT_MASK`, `GRUNGIFY_AUTOMATION_FILTER_MASK` | macro | 5.5+ free-constant masks versus the old enum members. Unused today. |

To flip a switch for one engine without touching code, add to `Grungify.Build.cs`:

```csharp
// e.g. only if UE 5.5 rejects GetMaterialResource(EShaderPlatform):
if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 5)
{
    PublicDefinitions.Add("GRUNGIFY_MATERIAL_RESOURCE_BY_SHADER_PLATFORM=0");
}
```

(`Target.Version` is `ReadOnlyBuildVersion` and has been available to ModuleRules for many versions. Confidence: high.)

---

## 4. Recommended source edits (not applied: other engineers own these files)

Line numbers are from the working tree at the time of writing and will drift, so use the function names to find each
spot. Apply E1 to E4 now. Apply E5 only if the first compile shows it is needed.

### E1. `Source/GrungifyEditor/Private/GrungifyReferenceScene.cpp`, `MakeNaniteCube()`, line 126

Add `#include "GrungifyCompat.h"` with the other includes. Then:

```cpp
// before
Mesh->NaniteSettings.bEnabled = true;
// after
Grungify::Compat::SetNaniteEnabled(Mesh, true);
```

(The GrungifyEditor module already depends on Grungify, and `GrungifyCompat.h` is in Grungify's `Public/`.)

### E2. `Source/GrungifyEditor/Private/GrungifyContentBuilder.cpp`, `CompileAndCheck()`, line ~668

Add `#include "GrungifyCompat.h"`. Then:

```cpp
// before
if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
// after
if (FMaterialResource* Resource = GRUNGIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material))
```

### E3. `Source/GrungifyEditor/Private/GrungifyMaterialHookup.cpp`, `CompileErrors()`, line ~210

Add `#include "GrungifyCompat.h"`. Then:

```cpp
// before
FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
// after
FMaterialResource* Resource = GRUNGIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material);
```

### E4. `Source/Grungify/Private/GrungifyWeatheringZone.cpp`, `WriteBakedAssets()` (the `BakeTexture` lambda), line ~2439

Add `#include "GrungifyCompat.h"` inside the existing `#if WITH_EDITOR` include block (`ImageUtils.h` is already included at l.18). Then:

```cpp
// before
TArray64<uint8> Png;
FImageUtils::PNGCompressImageArray(Result.TextureSize, Result.TextureSize, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
const FString File = ...;
if (IFileManager::Get().MakeDirectory(*PngDirectory, true) && FFileHelper::SaveArrayToFile(Png, *File))
// after
TArray64<uint8> Png;
const bool bEncoded = Grungify::Compat::CompressPNG<FImageUtils>(Result.TextureSize, Result.TextureSize,
	TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
const FString File = ...;
if (bEncoded && IFileManager::Get().MakeDirectory(*PngDirectory, true) && FFileHelper::SaveArrayToFile(Png, *File))
```

### E5 (conditional). `Source/Grungify/Private/GrungifyWeatheringZone.cpp`, `ScatterFromResults()`, line ~2250

Apply this only if `Spawned->AddDataLayer(DataLayer)` fails to compile (private) or warns (deprecated). Confidence in
the replacement API is **medium-low**, so verify it on first compile.

```cpp
// includes (inside #if WITH_EDITOR)
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"

// before
Spawned->AddDataLayer(DataLayer);
// after
if (const UDataLayerManager* Manager = UDataLayerManager::GetDataLayerManager(Spawned))
{
	if (const UDataLayerInstance* Instance = Manager->GetDataLayerInstanceFromAsset(DataLayer))
	{
		Instance->AddActor(Spawned);
	}
}
```

`UDataLayerManager` arrived in 5.3 and `UDataLayerInstance::AddActor` is editor-only. Both live in the Engine module,
so no Build.cs change is needed. If neither API compiles, `UDataLayerEditorSubsystem::AddActorToDataLayer(AActor*,
UDataLayerInstance*)` in the DataLayerEditor module is a fallback. The Grungify runtime module would then need
`PrivateDependencyModuleNames.Add("DataLayerEditor")` inside its `if (Target.bBuildEditor)` block.

### E6. Every `.h`, `.cpp`, `.cs` and `.ush` file: copyright line

**Already done.** A concurrent edit added the notice while this audit ran: the first check found 47 files missing it,
and a re-run found all 54 source files carrying it. Keep it on new files. Fab requires a notice at the top of each
source file; verify the exact wording against Fab's current guidelines. `python3 Scripts/CheckFabReadiness.py --only
copyright` checks it, and the build scripts run that check on every staged package.

### E7. `README.md`

- Line ~19, the **Status** paragraph: replace "v0.5.0 targets UE 5.8." with "v0.5.0 supports UE 5.5, 5.6, 5.7 and 5.8 (Win64; Mac and Linux built from source)." Keep the "not compiled yet" sentence until the per-version builds pass.
- **Install**, step 4: the Fab package ships the generated assets prebuilt. Replace "Uasset files can't ship as text, so they're generated locally. Commit them once they exist." with "Fab packages ship these assets prebuilt (saved with UE 5.5 so every supported version can load them). From a source checkout they are generated on first editor start; commit them once they exist."
- **Native verification**: add a line per engine version once `Scripts/BuildPlugin.ps1` passes for it.
- Add a short **Packaging** section that links `Docs/Fab.md` and `Scripts/`.

### E8. Build.cs files

No change is required for 5.5 to 5.8. All three already set `PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs`. Optional:

- `Grungify.Build.cs`: the conditional `PublicDefinitions` shown in section 3, only if a switch has to flip.
- Do **not** add `bEnforceIWYU` (deprecated since 5.2; `IWYUSupport` replaced it) or `bUseUnity = false` (`-StrictIncludes` covers it at package time).

### E9. New files the checker expects (outside what I was allowed to create)

- `Resources/Icon128.png`: a 128×128 PNG plugin icon. Fab and the Plugins browser use it.
- `Config/FilterPlugin.ini`: only matters if you use BuildPlugin's `-Package` output directly. The scripts zip the staged source instead. Suggested content:

  ```ini
  [FilterPlugin]
  /Shaders/...
  /README.md
  ```

---

## 5. Descriptor (`Grungify.uplugin`)

- Added `"FabURL": ""` and `"SupportURL": ""`. Both are empty because I don't know the real URLs. `"DocsURL"`, `"MarketplaceURL"` and `"CreatedByURL"` were already present and are still empty. Fill all of them before submitting; the checker fails until `DocsURL`, `SupportURL` and `FabURL` (or `MarketplaceURL`) are set.
- I kept `MarketplaceURL` because engine versions before Fab read it for the Plugins browser link. When the Fab listing exists, put the listing URL in both fields. I am not certain `FabURL` is read by any engine version. Unknown descriptor fields are ignored when the plugin loads, but the editor's *Edit plugin* dialog may drop them when it rewrites the file.
- `EngineVersion` is **not** set in the repo. The build scripts write `"EngineVersion": "5.x.0"` into each staged copy.
- `SupportedTargetPlatforms` and per-module `PlatformAllowList` are **not** set. The runtime `Grungify` module holds `AGrungifyWeatheringZone`, and zones are saved into levels. Restricting the runtime module to Win64/Mac/Linux would leave those levels with missing classes when a project cooks for consoles or mobile. If Fab's current rules require an allow-list, put it on `GrungifyEditor` only (`"PlatformAllowList": ["Win64", "Mac", "Linux"]`), which is harmless for an editor module, and state the tested platforms in the listing.
- `"IsBetaVersion": true` is left alone because it is a release decision. Set it to `false` for the Fab release, or the editor shows a beta warning. `"Version"` and `"VersionName"` are unchanged.
