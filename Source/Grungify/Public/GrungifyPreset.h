// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GrungifyTypes.h"
#include "GrungifyPreset.generated.h"

/**
 * A saved weathering look: layer settings, surface profiles, named material profiles and the material palette.
 * Lives in the project's content (not the plugin's generated content), so it survives plugin-content rebuilds,
 * re-generation and Clear. Zones copy it in with Load Preset (or by assigning it) and write it back with Save To Preset.
 *
 * Scene-specific settings (wear direction, leaks, actors, slot rules, filtering) stay on the zone.
 * Field names match AGrungifyWeatheringZone's; see GRUNGIFY_PRESET_FIELDS.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Grungify Preset"))
class GRUNGIFY_API UGrungifyPreset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset", meta = (MultiLine = true))
	FString Notes;

	/** Loading also sets the zone's seed. Turn off to keep each zone's own seed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset")
	bool bIncludeSeed = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset", meta = (EditCondition = "bIncludeSeed"))
	int32 Seed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "0", UIMax = "2"))
	float Intensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "1", UIMax = "200", Units = "cm"))
	float FeatureScaleCm = 25.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "0.25", UIMax = "20", Units = "cm"))
	float MaskTexelSizeCm = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "0", ClampMax = "1"))
	float WearDirectionInfluence = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0", UIMax = "2"))
	float DustIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0", UIMax = "300", Units = "cm"))
	float DustHeightCm = 60.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0", ClampMax = "1"))
	float LedgeDust = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0", ClampMax = "1"))
	float CreviceDust = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dust", meta = (ClampMin = "0", ClampMax = "1"))
	float FloorDust = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runoff", meta = (ClampMin = "0", UIMax = "2"))
	float RunoffIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runoff", meta = (ClampMin = "10", UIMax = "1000", Units = "cm"))
	float RunoffLengthCm = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runoff", meta = (ClampMin = "0", UIMax = "2"))
	float RunoffSpread = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runoff", meta = (ClampMin = "0", ClampMax = "1"))
	float AmbientStreaks = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Handling", meta = (ClampMin = "0", UIMax = "2"))
	float HandlingIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Handling", meta = (ClampMin = "0", UIMax = "250", Units = "cm"))
	float HandHeightMinCm = 80.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Handling", meta = (ClampMin = "0", UIMax = "250", Units = "cm"))
	float HandHeightMaxCm = 120.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Handling", meta = (ClampMin = "0", ClampMax = "1"))
	float GeneralHandling = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Chips", meta = (ClampMin = "0", UIMax = "2"))
	float ChipIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Chips", meta = (ClampMin = "5", ClampMax = "170", Units = "deg"))
	float EdgeAngleDegrees = 60.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Chips", meta = (ClampMin = "0.5", UIMax = "50", Units = "cm"))
	float EdgeWearWidthCm = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Chips", meta = (ClampMin = "0", UIMax = "100", Units = "cm"))
	float FloorKickHeightCm = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfaces")
	FGrungifySurfaceProfile StoneProfile = FGrungifySurfaceProfile(1.0f, 0.8f, 0.5f, 0.8f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfaces")
	FGrungifySurfaceProfile MetalProfile = FGrungifySurfaceProfile(0.6f, 1.0f, 1.0f, 0.6f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfaces")
	FGrungifySurfaceProfile TrimProfile = FGrungifySurfaceProfile(0.8f, 0.7f, 0.8f, 1.3f);

	/** Named profiles slot rules can refer to (e.g. Ceramic, Steel, Bronze). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfaces")
	TMap<FName, FGrungifySurfaceProfile> MaterialProfiles;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	FGrungifyPalette Palette;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Extended Layers", meta = (ShowOnlyInnerProperties))
	FGrungifyExtendedLayers Extended;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exposure")
	bool bUseSkyExposure = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exposure", meta = (ClampMin = "0", ClampMax = "1"))
	float SkyExposureInfluence = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runoff")
	bool bTraceWaterFlow = false;
};

/** Every field shared by UGrungifyPreset and AGrungifyWeatheringZone (Seed is handled separately). */
#define GRUNGIFY_PRESET_FIELDS(X) \
	X(Intensity) X(FeatureScaleCm) X(MaskTexelSizeCm) X(WearDirectionInfluence) \
	X(DustIntensity) X(DustHeightCm) X(LedgeDust) X(CreviceDust) X(FloorDust) \
	X(RunoffIntensity) X(RunoffLengthCm) X(RunoffSpread) X(AmbientStreaks) \
	X(HandlingIntensity) X(HandHeightMinCm) X(HandHeightMaxCm) X(GeneralHandling) \
	X(ChipIntensity) X(EdgeAngleDegrees) X(EdgeWearWidthCm) X(FloorKickHeightCm) \
	X(StoneProfile) X(MetalProfile) X(TrimProfile) X(MaterialProfiles) X(Palette) \
	X(Extended) X(bUseSkyExposure) X(SkyExposureInfluence) X(bTraceWaterFlow)
