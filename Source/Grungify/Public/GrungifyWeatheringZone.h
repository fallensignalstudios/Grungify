#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.generated.h"

class UArrowComponent;
class UGrungifyZoneComponent;
class UStaticMeshComponent;
struct FGrungifyComponentResult;
struct FGrungifyGenerationParams;

/**
 * Drop into a level, size the box, then Preview / Apply / Clear.
 *
 * The zone bakes four wear masks into the vertex colours of every static mesh it overlaps:
 *   R = edge chips, G = dust, B = runoff, A = handling.
 * MF_GrungifyWeathering turns those masks into shading, so any material that calls it picks up the result.
 * Everything is driven by Seed, so the same settings always regenerate the same result.
 */
UCLASS(Blueprintable, BlueprintType, meta = (DisplayName = "Grungify Weathering Zone"),
	hidecategories = (Collision, Physics, Replication, Networking, Input, Cooking, HLOD, LOD))
class GRUNGIFY_API AGrungifyWeatheringZone : public AActor
{
	GENERATED_BODY()

public:
	AGrungifyWeatheringZone();

	/** Generate and show the result without committing it. Re-run as often as you like. */
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

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Grungify")
	FString Status;

	UPROPERTY(VisibleInstanceOnly, Category = "Grungify")
	bool bPreviewActive = false;

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

	/** Extra runoff sources besides anything tagged Grungify.Vent or named like a vent / drain / spout. */
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

	UGrungifyZoneComponent* GetZoneComponent() const { return ZoneComponent; }
	const TArray<FGrungifyRunoffSource>& GetCachedRunoffSources() const { return CachedRunoffSources; }

	virtual void OnConstruction(const FTransform& Transform) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditImport() override;
	virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;

	/** Called by the zone box when its extent is edited. */
	void NotifyZoneShapeChanged(const FPropertyChangedEvent& PropertyChangedEvent);

	/** Re-scan the level for vents (also done on every Preview / Apply). */
	void RefreshRunoffSources();
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
	void RestorePreview();
	void BuildParams(FGrungifyGenerationParams& OutParams) const;
	void GenerateResults(TArray<FGrungifyComponentResult>& OutResults, FString& OutSummary);
	bool ShouldConsiderActor(const AActor* Actor, const FBox& ZoneBox) const;
	void WriteResult(const FGrungifyComponentResult& Result, EGrungifyPreviewDisplay Display) const;
#endif
};
