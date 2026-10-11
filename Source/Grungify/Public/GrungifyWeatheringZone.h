// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GameFramework/Actor.h"
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.generated.h"

class UArrowComponent;
class UDataLayerAsset;
class UGrungifyPreset;
struct FGrungifyCacheEntry;
struct FGrungifyGenerationJob;
struct FGrungifyLandscapeResult;
class UGrungifyZoneComponent;
class UMeshComponent;
struct FGrungifyComponentContext;
struct FGrungifyComponentResult;
struct FGrungifyGenerationParams;
class FGrungifyGateSampler;

/**
 * Drop into a level, size the box, then Preview / Apply / Clear.
 *
 * The zone bakes four wear masks into every static mesh it overlaps, as vertex colours or as a per-instance
 * mask texture (Nanite and sparse geometry):  R = edge chips, G = dust, B = runoff, A = handling.
 * MF_GrungifyWeathering turns those masks into shading; meshes the zone hasn't touched keep their original look.
 * Everything is driven by Seed, so the same settings always regenerate the same result.
 */
UCLASS(Blueprintable, BlueprintType, meta = (DisplayName = "Grungify Weathering Zone"),
	hidecategories = (Collision, Physics, Replication, Networking, Input, Cooking, HLOD, LOD))
class GRUNGIFY_API AGrungifyWeatheringZone : public AActor
{
	GENERATED_BODY()

public:
	AGrungifyWeatheringZone();

	/** Generate without committing. Other zones' applied meshes are skipped; Take Over happens only on Apply. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify")
	void Preview();

	/** Commit the result to the meshes as one undoable step. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify")
	void Apply();

	/** Put every mesh this zone touched back the way it was (undoable). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify")
	void Clear();

	/** Drop the preview and go back to the last applied state. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify")
	void CancelPreview();

	/** Pick a new random seed (and refresh the preview if live preview is on). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify")
	void NewSeed();

	/** Generate and report what would happen, without touching any mesh. See Last Report and Saved/Grungify/Reports. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify")
	void DryRun();

	/** Copy the look (layer settings, profiles, material profiles, palette, optionally the seed) from Preset into this zone. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify|Preset")
	void LoadPreset();

	/** Write this zone's look into Preset, creating a new preset asset in Preset Folder if none is assigned. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify|Preset")
	void SaveToPreset();

	/**
	 * Bake this zone's result into standalone assets in Bake Folder: a mask texture (plus extended texture) and a material
	 * instance per slot, exported as PNGs to Saved/Grungify/Bakes for touch-ups (Reimport picks the edited PNG up),
	 * then apply them to the meshes like Apply does (Clear restores the originals; the assets stay).
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify|Bake")
	void BakeToAssets();

	/** Fill Seed Variants with three seeds (current, plus two new) to compare. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify|Variants")
	void MakeSeedVariants();

	/** Preview the next seed in Seed Variants (cached, so flipping back is instant). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify|Variants")
	void PreviewNextVariant();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify|Variants")
	void PreviewPreviousVariant();

	/** Forget cached per-mesh results (they are also invalidated automatically when anything relevant changes). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grungify|Performance")
	void ClearGenerationCache();

	UFUNCTION(BlueprintPure, Category = "Grungify")
	int32 GetAppliedCount() const { return AppliedRecords.Num(); }

	UFUNCTION(BlueprintPure, Category = "Grungify")
	int32 GetPreviewCount() const { return PreviewRecords.Num(); }

	UFUNCTION(BlueprintPure, Category = "Grungify")
	bool IsGeneratingInBackground() const;

	UFUNCTION(BlueprintPure, Category = "Grungify")
	const FString& GetLastReportFile() const { return LastReportFile; }

	UFUNCTION(BlueprintPure, Category = "Grungify")
	const FGrungifyRunReport& GetLastReport() const { return LastReport; }

	// ---------------------------------------------------------------- General

	/** Same seed + same settings = same result, every time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	int32 Seed = 1337;

	/** Overall strength of every layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0", UIMax = "2"))
	float Intensity = 1.f;

	/** Physical size of the wear features (chip clusters, dust patches, streak spacing). Also drives the shader detail. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "1", UIMin = "5", UIMax = "200", Units = "cm"))
	float FeatureScaleCm = 25.f;

	/** Auto reads tags, physical materials and names per material slot. Anything else forces every mesh in the zone to that type. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	EGrungifySurfaceType SurfaceType = EGrungifySurfaceType::Auto;

	/** Regenerate the preview whenever a setting changes or the zone is moved. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	bool bLivePreview = true;

	/** What the meshes show while previewing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	EGrungifyPreviewDisplay PreviewDisplay = EGrungifyPreviewDisplay::FinalMaterial;

	/** Vertex colours, or a per-instance mask texture that works on Nanite and sparse geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	EGrungifyMaskOutput MaskOutput = EGrungifyMaskOutput::Auto;

	/** Physical size of one mask texel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0.25", UIMin = "1", UIMax = "20", Units = "cm"))
	float MaskTexelSizeCm = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "32", ClampMax = "4096"))
	int32 MaxMaskResolution = 2048;

	/** Write every run's report to Saved/Grungify/Reports as JSON. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	bool bWriteReportFiles = true;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Grungify")
	FString Status;

	UPROPERTY(VisibleInstanceOnly, Category = "Grungify")
	bool bPreviewActive = false;

	/** Where the last report was written. */
	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Grungify")
	FString LastReportFile;

	UPROPERTY(VisibleInstanceOnly, Transient, BlueprintReadOnly, Category = "Grungify")
	FGrungifyRunReport LastReport;

	// ---------------------------------------------------------- Wear direction

	/** Direction weather and traffic travel (the blue arrow). Edges facing into it chip more. Relative to the zone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Wear Direction")
	FRotator WearDirection = FRotator(-30.f, 0.f, 0.f);

	/** 0 = edges chip evenly all round, 1 = only edges facing into the wear direction chip. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Wear Direction", meta = (ClampMin = "0", ClampMax = "1"))
	float WearDirectionInfluence = 0.35f;

	// -------------------------------------------------------------------- Dust

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Dust At Feet", meta = (ClampMin = "0", UIMax = "2"))
	float DustIntensity = 1.f;

	/** Dust builds up from the floor to this height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Dust At Feet", meta = (ClampMin = "0", UIMax = "300", Units = "cm"))
	float DustHeightCm = 60.f;

	/** Dust on upward-facing ledges above the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Dust At Feet", meta = (ClampMin = "0", ClampMax = "1"))
	float LedgeDust = 0.6f;

	/** Grime packed into concave creases and inside corners. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Dust At Feet", meta = (ClampMin = "0", ClampMax = "1"))
	float CreviceDust = 0.5f;

	/** Dust on the open floor itself (lower than at the base of walls, where it collects). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Dust At Feet", meta = (ClampMin = "0", ClampMax = "1"))
	float FloorDust = 0.35f;

	// ------------------------------------------------------------------ Runoff

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Runoff Below Vents", meta = (ClampMin = "0", UIMax = "2"))
	float RunoffIntensity = 1.f;

	/** How far streaks run down from a vent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Runoff Below Vents", meta = (ClampMin = "10", UIMax = "1000", Units = "cm"))
	float RunoffLengthCm = 250.f;

	/** How much streaks fan out as they run down (0 = straight, 1 = twice the vent width at the bottom). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Runoff Below Vents", meta = (ClampMin = "0", UIMax = "2"))
	float RunoffSpread = 0.6f;

	/** Faint general streaking on all vertical surfaces, independent of vents. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Runoff Below Vents", meta = (ClampMin = "0", ClampMax = "1"))
	float AmbientStreaks = 0.15f;

	/** Hand-placed leaks at the actual leak locations. Drag the diamonds in the viewport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Runoff Below Vents")
	TArray<FGrungifyLeak> Leaks;

	/** Also treat actors tagged Grungify.Vent or named like a vent / drain / spout as sources. Turn off to use only Leaks and Runoff Sources. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Runoff Below Vents")
	bool bAutoDetectVents = true;

	/**
	 * Trace how water actually travels from each source: down walls, along sloped ledges, over edges, and into pools
	 * on flat ground (pools feed Moisture). Off = straight streaks below each source.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Runoff Below Vents")
	bool bTraceWaterFlow = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Runoff Below Vents", meta = (ClampMin = "2", UIMax = "50", Units = "cm", EditCondition = "bTraceWaterFlow"))
	float FlowStepCm = 10.f;

	/** Actors whose footprint is a runoff source. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Grungify|Runoff Below Vents")
	TArray<TSoftObjectPtr<AActor>> RunoffSources;

	// ---------------------------------------------------------------- Handling

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Handling On Rails", meta = (ClampMin = "0", UIMax = "2"))
	float HandlingIntensity = 1.f;

	/** Bottom of the band hands touch, measured from the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Handling On Rails", meta = (ClampMin = "0", UIMax = "250", Units = "cm"))
	float HandHeightMinCm = 80.f;

	/** Top of the band hands touch, measured from the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Handling On Rails", meta = (ClampMin = "0", UIMax = "250", Units = "cm"))
	float HandHeightMaxCm = 120.f;

	/** Handling on edges at hand height of things that are not rails (door frames, corners, counters). Rails always get full handling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Handling On Rails", meta = (ClampMin = "0", ClampMax = "1"))
	float GeneralHandling = 0.2f;

	// --------------------------------------------------------------- Edge chips

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Edge Chips", meta = (ClampMin = "0", UIMax = "2"))
	float ChipIntensity = 1.f;

	/** Edges this sharp or sharper count fully as exposed edges. Lower it to catch bevelled edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Edge Chips", meta = (ClampMin = "5", ClampMax = "170", Units = "deg"))
	float EdgeAngleDegrees = 60.f;

	/** Width of edge wear in mask textures (vertex colours follow the mesh's own edge spacing). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Edge Chips", meta = (ClampMin = "0.5", UIMax = "50", Units = "cm"))
	float EdgeWearWidthCm = 8.f;

	/** Edges below this height get extra chipping from feet and carts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Edge Chips", meta = (ClampMin = "0", UIMax = "100", Units = "cm"))
	float FloorKickHeightCm = 30.f;

	// ---------------------------------------------------------------- Surfaces

	/** Stone weathers to dust and chips; handling darkens it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Surfaces")
	FGrungifySurfaceProfile StoneProfile = FGrungifySurfaceProfile(1.0f, 1.0f, 0.7f, 1.0f);

	/** Metal takes runoff (rust) and handling polish; paint chips reveal bare metal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Surfaces")
	FGrungifySurfaceProfile MetalProfile = FGrungifySurfaceProfile(0.85f, 1.0f, 1.0f, 0.85f);

	/** Trim sits on exposed edges, so it chips the most. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Surfaces")
	FGrungifySurfaceProfile TrimProfile = FGrungifySurfaceProfile(0.9f, 0.85f, 0.9f, 1.3f);

	/** Named profiles slot rules can use via Profile Name (e.g. Ceramic, Steel, Bronze). Saved in presets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Surfaces")
	TMap<FName, FGrungifySurfaceProfile> MaterialProfiles;

	// ----------------------------------------------------------------- Palette

	/** Material look written onto the zone's own material instances on every Preview and Apply, so it survives regeneration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Palette")
	FGrungifyPalette Palette;

	// ------------------------------------------------------------------ Preset

	// -------------------------------------------------------- Extended layers

	/** Moisture, moss, soot, scorch, traffic scuffs and rust bloom. All off by default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Extended Layers", meta = (ShowOnlyInnerProperties))
	FGrungifyExtendedLayers Extended;

	// --------------------------------------------------------------- Exposure

	/**
	 * Measure how open each spot is to the sky (rays on a grid over the zone). Sheltered spots collect dust, damp and moss;
	 * exposed ones weather harder (chips, ambient streaks). Off = facing-based exposure only.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Exposure")
	bool bUseSkyExposure = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Exposure", meta = (ClampMin = "0", ClampMax = "1", EditCondition = "bUseSkyExposure"))
	float SkyExposureInfluence = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Exposure", meta = (ClampMin = "20", UIMax = "400", Units = "cm", EditCondition = "bUseSkyExposure"))
	float SkyProbeSpacingCm = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Exposure", meta = (ClampMin = "4", ClampMax = "128", EditCondition = "bUseSkyExposure"))
	int32 SkyRays = 24;

	/** Rays longer than this count as open sky. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Exposure", meta = (ClampMin = "100", UIMax = "20000", Units = "cm", EditCondition = "bUseSkyExposure"))
	float SkyRayLengthCm = 3000.f;

	// ----------------------------------------------------------------- Stamps

	/** Apply Grungify Wear Stamp actors that overlap the zone (hand-placed add/erase touch-ups). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Stamps")
	bool bApplyWearStamps = true;

	// ---------------------------------------------------------------- Scatter

	/** Decals or small meshes placed where layers are strong, on Apply. Removed by Clear and replaced on the next Apply. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Scatter")
	TArray<FGrungifyScatterEntry> Scatter;

	/** Data Layers scattered actors are added to (World Partition levels). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Scatter")
	TArray<TObjectPtr<const UDataLayerAsset>> ScatterDataLayers;

	// ------------------------------------------------------------- Variants

	/** Seeds to flip between with Preview Next / Previous Variant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Variants")
	TArray<int32> SeedVariants;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Grungify|Variants")
	int32 VariantIndex = 0;

	// ------------------------------------------------------------ Performance

	/** Reuse per-mesh results when nothing affecting that mesh changed (makes live preview and variant flipping fast). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Performance")
	bool bUseGenerationCache = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Performance", meta = (ClampMin = "16", UIMax = "4096", Units = "MB"))
	int32 CacheBudgetMB = 512;

	/** Live preview generates masks on a worker thread and shows them when ready, so the editor stays responsive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Performance")
	bool bBackgroundGeneration = true;

	// ------------------------------------------------------------------- Bake

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Bake", meta = (ContentDir))
	FDirectoryPath BakeFolder;

	/** Also write PNGs to Saved/Grungify/Bakes and point the texture assets at them for reimport after touch-ups. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Bake")
	bool bExportBakedPNGs = true;

	/** Assigning a preset loads it into the zone. Use Save To Preset to store changes back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Preset")
	TObjectPtr<UGrungifyPreset> Preset;

	/** Where Save To Preset creates a new preset asset when none is assigned. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Preset", meta = (ContentDir))
	FDirectoryPath PresetFolder;

	// -------------------------------------------------------------- Filtering

	/** Wear fades out over this distance inside the zone's faces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Filtering", meta = (ClampMin = "0", UIMax = "500", Units = "cm"))
	float ZoneFalloffCm = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Filtering")
	EGrungifyFloorMode FloorMode = EGrungifyFloorMode::Trace;

	/** Spacing of the floor traces. Smaller is more accurate on stairs and uneven ground, but slower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Filtering", meta = (ClampMin = "10", UIMax = "200", Units = "cm", EditCondition = "FloorMode == EGrungifyFloorMode::Trace"))
	float FloorProbeSpacingCm = 50.f;

	/** Also weather Movable static meshes (off by default, since they usually move out of the zone). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Filtering")
	bool bIncludeMovable = false;

	/**
	 * Weather instanced meshes (ISM/HISM, foliage) through per-instance custom data: one set of mask values per instance,
	 * broken up by world-space noise in the shader. Cruder than per-vertex or texture masks, but each instance differs.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Filtering")
	bool bWeatherInstancedMeshes = true;

	/**
	 * Weather skeletal meshes (props, vehicles) with a mask texture in their reference pose. Needs a non-overlapping
	 * mask UV channel; UV0 is used when it's the only clean one. Movable skeletal meshes are included: the wear is in UV
	 * space and moves with the mesh, but it's computed where the mesh stands now.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Filtering")
	bool bWeatherSkeletalMeshes = true;

	// -------------------------------------------------------------- Landscape

	/**
	 * Weather landscapes under the zone: dust, runoff, moisture and moss in a world-projected mask, plus extra dust and
	 * damp where the terrain meets walls and props. The landscape material needs MF_GrungifyLandscape (Hook Up Materials
	 * adds it). Each landscape component gets a material override, which Clear removes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Landscape")
	bool bWeatherLandscape = true;

	/** Size of one landscape mask texel. Coarser than meshes: terrain wear is broad. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Landscape", meta = (ClampMin = "5", UIMax = "200", Units = "cm", EditCondition = "bWeatherLandscape"))
	float LandscapeTexelSizeCm = 25.f;

	/** How far from a wall or prop the terrain collects extra dust and damp. 0 turns contact wear off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Landscape", meta = (ClampMin = "0", UIMax = "300", Units = "cm", EditCondition = "bWeatherLandscape"))
	float LandscapeContactRadiusCm = 80.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Landscape", meta = (ClampMin = "0", UIMax = "2", EditCondition = "bWeatherLandscape"))
	float LandscapeContactStrength = 0.7f;

	/** Actors this zone never touches (the Grungify.Ignore tag does the same). */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Grungify|Filtering")
	TArray<TSoftObjectPtr<AActor>> ExcludedActors;

	/** If set, only actors in one of these Data Layers are weathered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Targeting")
	TArray<TObjectPtr<const UDataLayerAsset>> IncludeDataLayers;

	/** If set, only these actors are weathered. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Grungify|Targeting")
	TArray<TSoftObjectPtr<AActor>> IncludeOnlyActors;

	/** If set, only components carrying one of these tags are weathered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Targeting")
	TArray<FName> RequiredComponentTags;

	/** Per-slot targeting: exclude slots, give them their own profile (ceramic, steel, bronze...), mark rails, gate by facing, height or a painted texture. First match wins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Targeting")
	TArray<FGrungifySlotRule> SlotRules;

	/** What to do with meshes another zone has already weathered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Targeting")
	EGrungifyOverlapPolicy OverlapPolicy = EGrungifyOverlapPolicy::SkipOwned;

	UGrungifyZoneComponent* GetZoneComponent() const { return ZoneComponent; }
	const TArray<FGrungifyRunoffSource>& GetCachedRunoffSources() const { return CachedRunoffSources; }

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Destroyed() override;
	virtual void PostUnregisterAllComponents() override;
	virtual void BeginDestroy() override;

	/** The record this zone holds for a component it has applied to, or null. */
	const FGrungifyComponentRecord* FindAppliedRecord(const UMeshComponent* Component) const;
	bool IsPreviewing(const UMeshComponent* Component) const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditImport() override;
	virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;

	/** Called by the zone box when its extent is edited. */
	void NotifyZoneShapeChanged(const FPropertyChangedEvent& PropertyChangedEvent);

	/** Re-scan the level for vents (also done on every Preview / Apply). */
	void RefreshRunoffSources();

	/** Puts previewed meshes back without an undo entry. Used before saves and when the zone is unloaded or destroyed. */
	void DiscardPreview(const TCHAR* Reason);

	/** Live preview (on a worker thread if Background Generation is on; a new request cancels the running one). Used by stamps and other actors that affect this zone. */
	void RequestLivePreview();

	/** Restores then hands over the original record; failed restoration retains ownership. */
	bool ReleaseAppliedRecord(const UMeshComponent* Component, FGrungifyComponentRecord& OutRecord);
#endif

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TObjectPtr<UGrungifyZoneComponent> ZoneComponent;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> WearArrow;
#endif

	/** Components currently showing a preview, with what they looked like before it. Not copied when the zone is duplicated. */
	UPROPERTY()
	TArray<FGrungifyComponentRecord> PreviewRecords;

	/** Components this zone has applied to, with what they looked like before the first Apply. Not copied when the zone is duplicated. */
	UPROPERTY()
	TArray<FGrungifyComponentRecord> AppliedRecords;

	/** Landscape components showing this zone's preview, with their original material overrides. */
	UPROPERTY()
	TArray<FGrungifyLandscapeRecord> PreviewLandscapeRecords;

	/** Landscape components this zone has applied to. */
	UPROPERTY()
	TArray<FGrungifyLandscapeRecord> AppliedLandscapeRecords;

	/** Decals and meshes this zone scattered on Apply. */
	UPROPERTY()
	TArray<TSoftObjectPtr<AActor>> ScatteredActors;

private:
	TArray<FGrungifyRunoffSource> CachedRunoffSources;

	/** Per-component results keyed by component path (editor only, never saved). */
	TMap<FString, TSharedPtr<FGrungifyCacheEntry>> GenerationCache;

	/** Live preview running on a worker thread. */
	TSharedPtr<FGrungifyGenerationJob> BackgroundJob;
	FTSTicker::FDelegateHandle BackgroundTicker;

	void UpdateWearArrow();

#if WITH_EDITOR
	bool CanRunInEditorWorld() const;
	void ForgetRecords();
	void RefreshPreviewIfLive();
	bool RestorePreview();
	bool CancelDependentPreviews();
	void BuildParams(FGrungifyGenerationParams& OutParams) const;
	void BuildContext(UMeshComponent* Component, TMap<UTexture2D*, TUniquePtr<FGrungifyGateSampler>>& Samplers, FGrungifyComponentContext& OutContext, FGrungifyComponentReport& Report);
	/** Returns false if the user cancelled; nothing has been written at that point. Shows a progress dialog for long runs. */
	bool GenerateResults(const TCHAR* Operation, TArray<FGrungifyComponentResult>& OutResults, TMap<UMeshComponent*, AGrungifyWeatheringZone*>& OutTakeOvers);

	/** Game-thread part of a run: floors, exposure, sources, flow, scan, contexts and cache lookups. False if cancelled. */
	bool PrepareJob(FGrungifyGenerationJob& Job, const TCHAR* Operation, const TFunction<bool()>& ShouldCancel);
	/** Turns finished job items into results and report entries (and fills the cache). */
	void CollectJob(FGrungifyGenerationJob& Job, TArray<FGrungifyComponentResult>& OutResults, TMap<UMeshComponent*, AGrungifyWeatheringZone*>& OutTakeOvers);
	void StartBackgroundPreview();
	void CancelBackgroundJob();
	bool TickBackgroundJob(float DeltaTime);
	/** Shared write step of Preview (synchronous or background). */
	void WritePreview(TArray<FGrungifyComponentResult>& Results, double StartSeconds);

	void ScatterFromResults(const TArray<FGrungifyComponentResult>& Results);
	void ClearScatteredActors();
	void TrimCache();
	/** Apply and Bake share everything except how texture results are stored. */
	void ApplyInternal(const TCHAR* Operation);
	/** Bake: standalone texture and material-instance assets instead of component-owned ones. */
	bool WriteBakedAssets(const FGrungifyComponentResult& Result);
	/** Lights, fires, fasteners, stamps, traffic paths and traced water for this run. */
	void GatherScene(FGrungifyGenerationJob& Job, const FBox& ZoneBox);

	/** Landscape (GrungifyZoneLandscape.cpp): generate masks for every landscape proxy under the zone. Game thread. */
	bool GenerateLandscapes(const FGrungifyGenerationJob& Job, const TFunction<bool()>& ShouldCancel);
	/** Writes PendingLandscapes into OutRecords (preview or applied). */
	void WriteLandscapes(TArray<FGrungifyLandscapeRecord>& OutRecords);
	/** Puts landscape components back; returns false if any couldn't be restored (their records stay). */
	bool RestoreLandscapes(TArray<FGrungifyLandscapeRecord>& Records);
	TArray<TSharedPtr<FGrungifyLandscapeResult>> PendingLandscapes;
	TMap<FString, TSharedPtr<FGrungifyLandscapeResult>> LandscapeCache;

	bool bBakingToAssets = false;
	TArray<UPackage*> BakedPackages;
	/** OutReason is set when an actor inside the zone is filtered out by a zone setting or tag (for the report). */
	bool ShouldConsiderActor(const AActor* Actor, const FBox& ZoneBox, FString* OutReason = nullptr) const;
	void CopyFromPreset(const UGrungifyPreset& Source);
	/** Writes masks, custom data and materials only when both original CPD arrays leave our slots free. */
	bool WriteResult(const FGrungifyComponentResult& Result, EGrungifyPreviewDisplay Display, const FGrungifyComponentRecord& OriginalState);
	const FGrungifyComponentRecord* FindOriginalRecord(const UMeshComponent* Component) const;
	void FinishReport(double StartSeconds);
#endif
};
