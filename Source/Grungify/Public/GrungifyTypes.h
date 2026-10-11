#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPtr.h"
#include "GrungifyTypes.generated.h"

class AActor;
class UMaterialInterface;
class UStaticMeshComponent;
class UTexture2D;

GRUNGIFY_API DECLARE_LOG_CATEGORY_EXTERN(LogGrungify, Log, All);

/** What a surface is built from. Picks the shading family in MF_GrungifyWeathering and the default wear profile. */
UENUM(BlueprintType)
enum class EGrungifySurfaceType : uint8
{
	Auto UMETA(ToolTip = "Classify each material slot from rules, tags, physical material and asset names."),
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

/** Where the masks are stored. */
UENUM(BlueprintType)
enum class EGrungifyMaskOutput : uint8
{
	Auto UMETA(ToolTip = "Texture for Nanite meshes and sparse geometry (when the mesh has the mask UV channel), vertex colours otherwise."),
	VertexColor UMETA(DisplayName = "Vertex Color", ToolTip = "Per-instance override vertex colours. Detail limited by vertex density; may not render on Nanite."),
	Texture UMETA(ToolTip = "Per-instance mask texture in the mask UV channel, bound through a material instance. Works on Nanite and sparse geometry.")
};

/** What a zone does with meshes another zone has already weathered. */
UENUM(BlueprintType)
enum class EGrungifyOverlapPolicy : uint8
{
	SkipOwned UMETA(ToolTip = "Leave meshes owned by another zone alone and report them."),
	TakeOver UMETA(ToolTip = "Apply takes ownership (and the original backup) from the other zone.")
};

UENUM(BlueprintType)
enum class EGrungifyRailMode : uint8
{
	Inherit UMETA(ToolTip = "Use tags / keywords on the mesh and actor."),
	Rail UMETA(ToolTip = "Treat this slot as a rail or handle."),
	NotRail UMETA(ToolTip = "Never treat this slot as a rail, even if the mesh is one.")
};

UENUM(BlueprintType)
enum class EGrungifyChannel : uint8
{
	R,
	G,
	B,
	A
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

/** Optional limits on where a slot rule lets wear appear. All enabled gates multiply together. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyGate
{
	GENERATED_BODY()

	/** Only faces whose world normal Z is within the range (1 = facing up, -1 = facing down). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	bool bUseFacingGate = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bUseFacingGate", ClampMin = "-1", ClampMax = "1"))
	float MinUpFacing = -1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bUseFacingGate", ClampMin = "-1", ClampMax = "1"))
	float MaxUpFacing = 1.f;

	/** Only between these heights above the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	bool bUseHeightGate = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bUseHeightGate", Units = "cm"))
	float MinHeightCm = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bUseHeightGate", Units = "cm"))
	float MaxHeightCm = 10000.f;

	/** Painted gate in mesh UV space: white = wear allowed, black = none. Source must be 8-bit (BGRA8 or G8). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	TObjectPtr<UTexture2D> GateTexture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0", ClampMax = "7"))
	int32 GateUVChannel = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	EGrungifyChannel GateChannel = EGrungifyChannel::R;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	bool bInvertGate = false;
};

/**
 * Targets material slots precisely. The first enabled rule that matches a slot decides it.
 * A rule needs at least one of Actor, Name Pattern or Material.
 */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifySlotRule
{
	GENERATED_BODY()

	/** Shown in reports, e.g. "Ceramic", "Bridge deck". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	FString Label;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	bool bEnabled = true;

	/** Only slots on this actor. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Grungify|Match")
	TSoftObjectPtr<AActor> Actor;

	/** Wildcard (e.g. "*Deck*") tested against the slot name, material names (and parents), mesh, component and actor label. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Match")
	FString NamePattern;

	/** Matches the slot's material or any material it derives from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Match")
	TSoftObjectPtr<UMaterialInterface> Material;

	/** Leave matching slots completely untouched. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Effect")
	bool bExclude = false;

	/** Shading family for matching slots (Auto = classify as usual). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Effect", meta = (EditCondition = "!bExclude"))
	EGrungifySurfaceType Surface = EGrungifySurfaceType::Auto;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Effect", meta = (InlineEditConditionToggle))
	bool bOverrideProfile = false;

	/** Independent wear profile for matching slots (ceramic, steel, bronze...). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Effect", meta = (EditCondition = "bOverrideProfile"))
	FGrungifySurfaceProfile Profile;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Effect", meta = (EditCondition = "!bExclude"))
	EGrungifyRailMode Rail = EGrungifyRailMode::Inherit;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Effect", meta = (EditCondition = "!bExclude"))
	FGrungifyGate Gate;
};

/** A hand-placed leak that runoff streaks flow down from. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyLeak
{
	GENERATED_BODY()

	/** Relative to the zone. Drag the diamond widget in the viewport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (MakeEditWidget))
	FVector Location = FVector(0.0, 0.0, 150.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "1", UIMax = "200", Units = "cm"))
	float RadiusCm = 10.f;

	/** 0 = use the zone's Runoff Length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0", UIMax = "1000", Units = "cm"))
	float LengthCm = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0", UIMax = "2"))
	float Strength = 1.f;
};

/** Mirror of the engine's mesh-paint cache entry, so it can be backed up and saved. */
USTRUCT()
struct GRUNGIFY_API FGrungifyPaintedVertex
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Position = FVector::ZeroVector;

	UPROPERTY()
	FVector4 Normal = FVector4(0.0, 0.0, 0.0, 0.0);

	UPROPERTY()
	FColor Color = FColor(0, 0, 0, 0);
};

/** One LOD's vertex colour state before a zone touched it. */
USTRUCT()
struct GRUNGIFY_API FGrungifyLODBackup
{
	GENERATED_BODY()

	UPROPERTY()
	bool bHadOverride = false;

	UPROPERTY()
	TArray<FColor> Colors;

	UPROPERTY()
	TArray<FGrungifyPaintedVertex> PaintedVertices;
};

/** Everything needed to put a component back exactly the way it was. */
USTRUCT()
struct GRUNGIFY_API FGrungifyComponentRecord
{
	GENERATED_BODY()

	UPROPERTY()
	TSoftObjectPtr<UStaticMeshComponent> Component;

	/** LODData.Num() before the zone touched it. */
	UPROPERTY()
	int32 LODDataCount = 0;

	UPROPERTY()
	TArray<FGrungifyLODBackup> LODs;

	UPROPERTY()
	FString StaticMeshDerivedDataKey;

	/** The complete default custom primitive data array. */
	UPROPERTY()
	TArray<float> CustomData;

	/** The complete override material array, including its length. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> OverrideMaterials;
};

/** What happened to one component in a run. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyComponentReport
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Actor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Component;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Mesh;

	/** "vertex", "texture" or "skipped". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Result;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Notes;

	/** One line per material slot: index, material, surface, rule, flags. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Slots;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 Vertices = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 TextureSize = 0;

	/** Mean of each mask over covered samples: R = chips, G = dust, B = runoff, A = handling. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FLinearColor MeanMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FLinearColor MaxMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	bool bNanite = false;

	/** The Grungify custom primitive data indices were already used by something else, so the material path stays inactive. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	bool bCustomDataConflict = false;

	/** Zone that owned this mesh before this run (empty if none). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString PreviousOwner;
};

/** Structured result of a Dry Run, Preview or Apply. Also written to Saved/Grungify/Reports as JSON. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyRunReport
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Operation;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Zone;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 Seed = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Time;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	float Seconds = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 RunoffSources = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 Weathered = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 Skipped = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FGrungifyComponentReport> Components;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Warnings;
};

/** A point that runoff streaks flow down from (vent, drain, spout or hand-placed leak). */
struct FGrungifyRunoffSource
{
	FVector Origin = FVector::ZeroVector;
	float Radius = 10.f;
	/** 0 = the zone's runoff length. */
	float Length = 0.f;
	float Strength = 1.f;
	bool bManual = false;
};

namespace Grungify
{
	/** Custom primitive data slots, relative to UGrungifySettings::CustomPrimitiveDataIndex. */
	enum ECustomDataOffset : int32
	{
		/** 0 = inactive (material untouched), 1 = vertex colour masks, 2 = mask texture. */
		CPD_MaskMode = 0,
		CPD_SurfaceType = 1,
		CPD_FeatureScale = 2,
		CPD_SeedOffset = 3,
		CPD_DebugChannel = 4,
		CPD_Count = 5
	};

	/** Bumped whenever the generated content changes; older assets are rebuilt in place. */
	constexpr int32 ContentVersion = 3;

	inline FName MaskTextureParameterName() { return FName(TEXT("Grungify Mask Texture")); }
}
