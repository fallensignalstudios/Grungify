#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPtr.h"
#include "GrungifyTypes.generated.h"

class AActor;
class UMaterialInterface;
class UStaticMeshComponent;
class UStaticMesh;
class UTexture2D;
class UDataLayerAsset;

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
	Handling UMETA(DisplayName = "Handling Mask"),
	Moisture UMETA(DisplayName = "Moisture Mask (texture output)"),
	Soot UMETA(DisplayName = "Soot Mask (texture output)"),
	Scorch UMETA(DisplayName = "Scorch Mask (texture output)"),
	Moss UMETA(DisplayName = "Moss Mask (texture output)"),
	AllExtended UMETA(DisplayName = "All Extended Masks (texture output)")
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

/** Every wear layer, for stamps, scatter and runtime wear. Primary layers live in mask texture 1 / vertex colour; extended layers in mask texture 2. */
UENUM(BlueprintType)
enum class EGrungifyWearLayer : uint8
{
	EdgeChips,
	Dust,
	Runoff,
	Handling,
	Moisture UMETA(ToolTip = "Damp patches, pooling and water lines; the shader adds efflorescence on stone."),
	Soot,
	Scorch,
	Moss UMETA(ToolTip = "Moss and lichen on damp, sheltered, north-facing stone and trim.")
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

	/** Use a named profile from the zone's Material Profiles (saved in presets). Ignored when Override Profile is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Effect", meta = (EditCondition = "!bExclude && !bOverrideProfile"))
	FName ProfileName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Effect", meta = (EditCondition = "!bExclude"))
	EGrungifyRailMode Rail = EGrungifyRailMode::Inherit;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify|Effect", meta = (EditCondition = "!bExclude"))
	FGrungifyGate Gate;
};

/**
 * Shader-side look written onto every material instance the zone owns, on every Preview and Apply.
 * Only fields whose override is ticked are written; the rest inherit from the mesh's own material.
 */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyPalette
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideDustColor = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideDustColor"))
	FLinearColor DustColor = FLinearColor(0.55f, 0.50f, 0.42f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideRunoffColor = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideRunoffColor"))
	FLinearColor RunoffColor = FLinearColor(0.16f, 0.11f, 0.07f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideTrimChipColor = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideTrimChipColor"))
	FLinearColor TrimChipColor = FLinearColor(0.45f, 0.42f, 0.38f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideStrength", ClampMin = "0", UIMax = "2"))
	float Strength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideChipStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideChipStrength", ClampMin = "0", UIMax = "2"))
	float ChipStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideDustStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideDustStrength", ClampMin = "0", UIMax = "2"))
	float DustStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideRunoffStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideRunoffStrength", ClampMin = "0", UIMax = "2"))
	float RunoffStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideHandlingStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideHandlingStrength", ClampMin = "0", UIMax = "2"))
	float HandlingStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideReliefStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideReliefStrength", ClampMin = "0", UIMax = "2"))
	float ReliefStrength = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideCrackStrength = false;

	/** Crack Strength: hairline cracks across stone where it is chipped and exposed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideCrackStrength", ClampMin = "0", UIMax = "2"))
	float CrackStrength = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideMoistureStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideMoistureStrength", ClampMin = "0", UIMax = "2"))
	float MoistureStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideSootStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideSootStrength", ClampMin = "0", UIMax = "2"))
	float SootStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideScorchStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideScorchStrength", ClampMin = "0", UIMax = "2"))
	float ScorchStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideMossStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideMossStrength", ClampMin = "0", UIMax = "2"))
	float MossStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideMossColor = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideMossColor"))
	FLinearColor MossColor = FLinearColor(0.12f, 0.18f, 0.05f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideSootColor = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideSootColor"))
	FLinearColor SootColor = FLinearColor(0.03f, 0.028f, 0.025f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (InlineEditConditionToggle))
	bool bOverrideEfflorescenceColor = false;

	/** Salt bloom left on damp stone as it dries. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bOverrideEfflorescenceColor"))
	FLinearColor EfflorescenceColor = FLinearColor(0.78f, 0.76f, 0.70f);
};

/**
 * Layers beyond the four primary ones. They are stored in a second mask texture, so they need Texture mask output
 * (Auto switches to texture for any mesh that has the mask UV channel when one of these is above zero).
 */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyExtendedLayers
{
	GENERATED_BODY()

	/** Damp ground around pools, along traced water paths and below the water line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0", UIMax = "2"))
	float MoistureIntensity = 0.f;

	/** How far dampness spreads around a pool. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0", UIMax = "300", Units = "cm"))
	float PoolDampRadiusCm = 60.f;

	/** Height of an old flood line above the floor (0 = none). Everything below is damp, with a tide mark at the top. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0", UIMax = "300", Units = "cm"))
	float WaterLineHeightCm = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moss", meta = (ClampMin = "0", UIMax = "2"))
	float MossIntensity = 0.f;

	/** Which way is north, as a yaw in the world (0 = +X). Moss favours north-facing surfaces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moss", meta = (UIMin = "-180", UIMax = "180", Units = "deg"))
	float NorthYawDegrees = 0.f;

	/** 0 = moss grows on any facing, 1 = only north-facing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moss", meta = (ClampMin = "0", ClampMax = "1"))
	float MossNorthBias = 0.6f;

	/** Soot plumes above lights and fire sources (actors tagged Grungify.Fire or named like fire, torch, brazier, candle). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soot & Scorch", meta = (ClampMin = "0", UIMax = "2"))
	float SootIntensity = 0.f;

	/** Also treat point and spot lights in the zone as soot sources (lamps, sconces). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soot & Scorch")
	bool bSootFromLights = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soot & Scorch", meta = (ClampMin = "10", UIMax = "500", Units = "cm"))
	float SootRadiusCm = 120.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soot & Scorch", meta = (ClampMin = "10", UIMax = "1000", Units = "cm"))
	float SootPlumeHeightCm = 300.f;

	/** Burn marks right around fire sources (not lights). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soot & Scorch", meta = (ClampMin = "0", UIMax = "2"))
	float ScorchIntensity = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soot & Scorch", meta = (ClampMin = "5", UIMax = "300", Units = "cm"))
	float ScorchRadiusCm = 70.f;

	/** Scuffs along Grungify Traffic Path splines: polish on the floor, kick chips low on walls. Uses primary layers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic", meta = (ClampMin = "0", UIMax = "2"))
	float ScuffIntensity = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic", meta = (ClampMin = "0", UIMax = "50", Units = "cm"))
	float ScuffWallHeightCm = 35.f;

	/** Rust stains blooming around fasteners on metal (meshes tagged Grungify.Fastener or named bolt, rivet, screw, nut). Uses the runoff layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rust Bloom", meta = (ClampMin = "0", UIMax = "2"))
	float RustBloomIntensity = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rust Bloom", meta = (ClampMin = "1", UIMax = "50", Units = "cm"))
	float RustBloomRadiusCm = 10.f;

	bool UsesExtendedTexture() const { return MoistureIntensity > 0.f || MossIntensity > 0.f || SootIntensity > 0.f || ScorchIntensity > 0.f; }
};

/** Scatters decals or small meshes where a wear layer is strong. Placed on Apply, removed by Clear. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyScatterEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter")
	EGrungifyWearLayer Layer = EGrungifyWearLayer::EdgeChips;

	/** Mask strength needed before anything is placed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter", meta = (ClampMin = "0", ClampMax = "1"))
	float Threshold = 0.4f;

	/** Placements per square metre where the layer is above the threshold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter", meta = (ClampMin = "0", UIMax = "20"))
	float DensityPerM2 = 0.5f;

	/** Deferred decal material (projected along the surface normal). Leave empty to scatter Mesh instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter")
	TObjectPtr<UMaterialInterface> DecalMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter")
	TObjectPtr<UStaticMesh> Mesh;

	/** Random size (decal width, or uniform mesh scale in cm of its bounds) between X and Y. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter")
	FVector2D SizeRangeCm = FVector2D(20.0, 60.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter", meta = (ClampMin = "1", UIMax = "100", Units = "cm"))
	float DecalDepthCm = 10.f;

	/** Only on these surfaces (empty = all). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter")
	TArray<EGrungifySurfaceType> Surfaces;
};

/** How much a wear stamp adds (positive) or erases (negative) on each layer. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyStampLayers
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float EdgeChips = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Dust = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Runoff = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Handling = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Moisture = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Soot = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Scorch = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Moss = 0.f;
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

	/** Refuse destructive restoration after the component's mesh has been replaced. */
	UPROPERTY()
	TSoftObjectPtr<UStaticMesh> StaticMesh;

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

	/** The complete live array; it can differ from the saved defaults. */
	UPROPERTY()
	TArray<float> LiveCustomData;

	/** Distinguishes new exact backups from records saved by older plugin versions. */
	UPROPERTY()
	bool bHasLiveCustomData = false;

	/** Custom data base when this record was captured; restoration never consults current settings. */
	UPROPERTY()
	int32 CustomDataBase = INDEX_NONE;

	/** The complete override material array, including its length. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> OverrideMaterials;

	/** Instanced meshes: per-instance custom data before the zone wrote its masks. */
	UPROPERTY()
	bool bHasInstanceData = false;

	/** Instance layout at capture; unknown for records saved before this field existed. */
	UPROPERTY()
	int32 InstanceCount = INDEX_NONE;

	UPROPERTY()
	int32 NumInstanceCustomDataFloats = 0;

	UPROPERTY()
	TArray<float> InstanceCustomData;
};

/** Why one material slot was or wasn't weathered, and how it was classified. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifySlotReport
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 Slot = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString SlotName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Material;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	bool bIncluded = true;

	/** Plain-language reason it was included or excluded. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Reason;

	/** Matched slot rule (label and what matched), or empty. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Rule;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Surface;

	/** Where the surface came from: rule, tag, physical material, keyword (and which), zone override or fallback. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString SurfaceSource;

	/** Which wear profile was used. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Profile;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	bool bRail = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString RailSource;

	/** Active gates (facing, height, texture) in readable form. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Gates;

	/** Problems specific to this slot (e.g. a gate UV channel the mesh doesn't have). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Warnings;
};

/** Checks on the UV channel the mask texture is baked into. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyUVReport
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 Channel = 0;

	/** False when the check did not run (vertex output requested, no readable mesh). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	bool bChecked = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	bool bPresent = false;

	/** "ok", "warning" or "failed". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Verdict;

	/** Share of the mesh's UV area that lands outside 0..1. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	float OutOfRange = 0.f;

	/** Share of covered texels claimed by more than one triangle (overlapping islands). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	float Overlap = 0.f;

	/** Share of triangles with zero UV area. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	float Degenerate = 0.f;

	/** Share of the 0..1 square the UVs cover. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	float Coverage = 0.f;

	/** Ratio between the most and least stretched triangles (world area per UV area); 1 = uniform texel density. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	float DensitySpread = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Problems;
};

/** Wall-clock time spent in one phase of a run. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyPhaseTiming
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Phase;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	float Seconds = 0.f;

	/** Items processed (floor cells, meshes, writes...). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 Items = 0;
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

	/** "vertex", "texture", "skipped" or "cancelled_before_write". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Result;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Notes;

	/** One line per material slot: index, material, surface, rule, flags. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Slots;

	/** Structured per-slot decisions: why each slot was included or excluded, and how it was classified. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FGrungifySlotReport> SlotDetails;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FGrungifyUVReport MaskUV;

	/** Time spent generating this mesh's masks. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	float Seconds = 0.f;

	/** Reused from the generation cache (nothing that affects this mesh changed). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	bool bFromCache = false;

	/** Instanced meshes: number of instances weathered. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 Instances = 0;

	/** Size of the extended-layer mask texture (0 if not used). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 ExtendedTextureSize = 0;

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

	/** Time per phase: floor traces, runoff sources, scan, generation, writes. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FGrungifyPhaseTiming> Phases;

	/** The run was cancelled; Cancelled During names the phase. No meshes were written after cancellation. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	bool bCancelled = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString CancelledDuring;

	/** Preset the zone's look was loaded from, if any. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	FString Preset;

	/** Meshes reused from the generation cache. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 CacheHits = 0;

	/** Decals and meshes placed by Scatter. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	int32 Scattered = 0;

	/** Generation ran on a background thread (Background Generation). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	bool bBackground = false;

	/** Assets written by Bake To Assets (package names). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FString> BakedAssets;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Warnings;
};

/** A point that runoff streaks flow down from (vent, drain, spout or hand-placed leak). */
struct FGrungifyRunoffSource
{
	/** Traced water path (world points from the source down), when Trace Water Flow is on. Empty = straight down. */
	TArray<FVector> Path;
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
		/** Only signed Grungify modes activate an explicitly owned material instance. */
		CPD_MaskMode = 0,
		CPD_SurfaceType = 1,
		CPD_FeatureScale = 2,
		CPD_SeedOffset = 3,
		CPD_DebugChannel = 4,
		CPD_Count = 5
	};

	/** Bumped whenever the generated content changes; older assets are rebuilt in place. */
	constexpr int32 ContentVersion = 6;
	constexpr float MaskModeVertex = 501.f;
	constexpr float MaskModeTexture = 502.f;
	/** Instanced meshes: masks come from per-instance custom data. */
	constexpr float MaskModeInstanced = 503.f;
	/** Per-instance custom data floats Grungify writes (chips, dust, runoff, handling). */
	constexpr int32 InstanceDataCount = 4;

	inline FName MaskTextureParameterName() { return FName(TEXT("Grungify Mask Texture")); }
	/** Second mask texture: moisture, soot, scorch, moss. */
	inline FName ExtendedMaskTextureParameterName() { return FName(TEXT("Grungify Extended Mask Texture")); }
	/** Physical size of one mask texel, stored on owned instances for runtime wear. */
	inline FName MaskTexelSizeParameterName() { return FName(TEXT("Grungify Mask Texel Size")); }
	inline FName ActiveParameterName() { return FName(TEXT("Grungify Active")); }
	inline FName SurfaceOverrideParameterName() { return FName(TEXT("Grungify Surface Override")); }
}
