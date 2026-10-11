#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPtr.h"
#include "GrungifyTypes.generated.h"

class UMaterialInterface;
class UStaticMeshComponent;

GRUNGIFY_API DECLARE_LOG_CATEGORY_EXTERN(LogGrungify, Log, All);

/** What a surface is built from. Drives how much of each wear layer it receives and how the material shades it. */
UENUM(BlueprintType)
enum class EGrungifySurfaceType : uint8
{
	Auto UMETA(ToolTip = "Classify each material slot from tags, physical material and asset names."),
	Stone,
	Metal,
	Trim
};

/** What the meshes show while a preview is active. */
UENUM(BlueprintType)
enum class EGrungifyPreviewDisplay : uint8
{
	FinalMaterial UMETA(DisplayName = "Final Material", ToolTip = "Show the result through the meshes' own materials (they must use MF_GrungifyWeathering)."),
	AllMasks UMETA(DisplayName = "All Masks", ToolTip = "Swap to the mask debug material: red = chips, green = dust, blue = runoff, yellow = handling."),
	EdgeChips UMETA(DisplayName = "Edge Chips Mask"),
	Dust UMETA(DisplayName = "Dust Mask"),
	Runoff UMETA(DisplayName = "Runoff Mask"),
	Handling UMETA(DisplayName = "Handling Mask")
};

/** How the zone finds the floor that "dust at feet" and "hand height" are measured from. */
UENUM(BlueprintType)
enum class EGrungifyFloorMode : uint8
{
	Trace UMETA(DisplayName = "Trace Floors", ToolTip = "Trace down through the zone to find walkable floors, including multiple storeys and stairs."),
	ZoneBottom UMETA(DisplayName = "Zone Bottom", ToolTip = "Treat the bottom face of the zone box as the floor.")
};

/** Per-surface multipliers for each wear layer. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifySurfaceProfile
{
	GENERATED_BODY()

	FGrungifySurfaceProfile() = default;
	FGrungifySurfaceProfile(float InDust, float InRunoff, float InHandling, float InEdgeChips)
		: Dust(InDust), Runoff(InRunoff), Handling(InHandling), EdgeChips(InEdgeChips)
	{
	}

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0", UIMax = "2"))
	float Dust = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0", UIMax = "2"))
	float Runoff = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0", UIMax = "2"))
	float Handling = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0", UIMax = "2"))
	float EdgeChips = 1.f;
};

/** Vertex colours a component had on one LOD before a zone touched it. */
USTRUCT()
struct GRUNGIFY_API FGrungifyLODBackup
{
	GENERATED_BODY()

	UPROPERTY()
	bool bHadOverride = false;

	UPROPERTY()
	TArray<FColor> Colors;
};

/** Everything needed to put a component back the way it was. */
USTRUCT()
struct GRUNGIFY_API FGrungifyComponentRecord
{
	GENERATED_BODY()

	UPROPERTY()
	TSoftObjectPtr<UStaticMeshComponent> Component;

	UPROPERTY()
	TArray<FGrungifyLODBackup> LODs;

	/** Default custom primitive data values at the Grungify indices. */
	UPROPERTY()
	TArray<float> CustomData;

	UPROPERTY()
	bool bMaterialsSwapped = false;

	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> OverrideMaterials;
};

/** A point that runoff streaks flow down from (a vent, drain, spout...). */
struct FGrungifyRunoffSource
{
	FVector Origin = FVector::ZeroVector;
	float Radius = 10.f;
};

namespace Grungify
{
	/** Custom primitive data slots, relative to UGrungifySettings::CustomPrimitiveDataIndex. */
	enum ECustomDataOffset : int32
	{
		CPD_SurfaceType = 0,
		CPD_FeatureScale = 1,
		CPD_SeedOffset = 2,
		CPD_DebugChannel = 3,
		CPD_Count = 4
	};
}
