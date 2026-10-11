#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.generated.h"

class UArrowComponent;
class UGrungifyPreset;
class UGrungifyZoneComponent;
class UStaticMeshComponent;
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
	FGrungifySurfaceProfile StoneProfile = FGrungifySurfaceProfile(1.0f, 0.8f, 0.5f, 0.8f);

	/** Metal takes runoff (rust) and handling polish; paint chips reveal bare metal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Surfaces")
	FGrungifySurfaceProfile MetalProfile = FGrungifySurfaceProfile(0.6f, 1.0f, 1.0f, 0.6f);

	/** Trim sits on exposed edges, so it chips the most. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Surfaces")
	FGrungifySurfaceProfile TrimProfile = FGrungifySurfaceProfile(0.8f, 0.7f, 0.8f, 1.3f);

	/** Named profiles slot rules can use via Profile Name (e.g. Ceramic, Steel, Bronze). Saved in presets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Surfaces")
	TMap<FName, FGrungifySurfaceProfile> MaterialProfiles;

	// ----------------------------------------------------------------- Palette

	/** Material look written onto the zone's own material instances on every Preview and Apply, so it survives regeneration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Palette")
	FGrungifyPalette Palette;

	// ------------------------------------------------------------------ Preset

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

	/** Actors this zone never touches (the Grungify.Ignore tag does the same). */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Grungify|Filtering")
	TArray<TSoftObjectPtr<AActor>> ExcludedActors;

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

	/** The record this zone holds for a component it has applied to, or null. */
	const FGrungifyComponentRecord* FindAppliedRecord(const UStaticMeshComponent* Component) const;
	bool IsPreviewing(const UStaticMeshComponent* Component) const;

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

	/** Restores then hands over the original record; failed restoration retains ownership. */
	bool ReleaseAppliedRecord(const UStaticMeshComponent* Component, FGrungifyComponentRecord& OutRecord);
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

private:
	TArray<FGrungifyRunoffSource> CachedRunoffSources;

	void UpdateWearArrow();

#if WITH_EDITOR
	bool CanRunInEditorWorld() const;
	void ForgetRecords();
	void RefreshPreviewIfLive();
	bool RestorePreview();
	bool CancelDependentPreviews();
	void BuildParams(FGrungifyGenerationParams& OutParams) const;
	void BuildContext(UStaticMeshComponent* Component, TMap<UTexture2D*, TUniquePtr<FGrungifyGateSampler>>& Samplers, FGrungifyComponentContext& OutContext, FGrungifyComponentReport& Report);
	/** Returns false if the user cancelled; nothing has been written at that point. Shows a progress dialog for long runs. */
	bool GenerateResults(const TCHAR* Operation, TArray<FGrungifyComponentResult>& OutResults, TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*>& OutTakeOvers);
	/** OutReason is set when an actor inside the zone is filtered out by a zone setting or tag (for the report). */
	bool ShouldConsiderActor(const AActor* Actor, const FBox& ZoneBox, FString* OutReason = nullptr) const;
	void CopyFromPreset(const UGrungifyPreset& Source);
	/** Writes masks, custom data and materials only when both original CPD arrays leave our slots free. */
	bool WriteResult(const FGrungifyComponentResult& Result, EGrungifyPreviewDisplay Display, const FGrungifyComponentRecord& OriginalState);
	const FGrungifyComponentRecord* FindOriginalRecord(const UStaticMeshComponent* Component) const;
	void FinishReport(double StartSeconds);
#endif
};
