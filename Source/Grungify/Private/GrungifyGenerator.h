#pragma once

#include "CoreMinimal.h"
#include "GrungifyTypes.h"

#if WITH_EDITOR

class AActor;
class UActorComponent;
class UMaterialInterface;
class UPrimitiveComponent;
class UStaticMeshComponent;
class UTexture2D;
class UWorld;

/** Floor heights sampled on a grid over the zone. Each cell can hold several floors (storeys, stairs). */
class FGrungifyFloorField
{
public:
	void BuildFlat(double InFloorZ);
	void BuildTraced(UWorld* World, const FBox& InBounds, double Spacing, const AActor* IgnoreActor);

	/** Highest floor at or just below P. */
	double FloorBelow(const FVector& P) const;

private:
	FBox Bounds = FBox(ForceInit);
	double FlatZ = 0.0;
	double CellX = 1.0;
	double CellY = 1.0;
	int32 NumX = 0;
	int32 NumY = 0;
	/** Per cell, highest first. */
	TArray<TArray<double, TInlineAllocator<4>>> Heights;
};

/** CPU copy of an 8-bit gate texture, sampled nearest with wrap. */
class FGrungifyGateSampler
{
public:
	bool Init(UTexture2D* Texture, FString& OutError);
	float Sample(const FVector2f& UV, EGrungifyChannel Channel) const;

private:
	int32 Width = 0;
	int32 Height = 0;
	int32 BytesPerPixel = 0;
	TArray64<uint8> Pixels;
};

struct FGrungifyGenerationParams
{
	uint32 Seed = 0;
	float Intensity = 1.f;
	float FeatureScale = 25.f;

	FTransform ZoneTransform;
	FVector ZoneExtent = FVector::ZeroVector;
	float ZoneFalloff = 0.f;

	FVector WearDir = FVector::ForwardVector;
	float WearInfluence = 0.f;

	float DustIntensity = 1.f;
	float DustHeight = 60.f;
	float LedgeDust = 0.f;
	float CreviceDust = 0.f;
	float FloorDust = 0.f;

	float RunoffIntensity = 1.f;
	float RunoffLength = 250.f;
	float RunoffSpread = 0.f;
	float AmbientStreaks = 0.f;

	float HandlingIntensity = 1.f;
	float HandMin = 80.f;
	float HandMax = 120.f;
	float GeneralHandling = 0.f;

	float ChipIntensity = 1.f;
	float EdgeAngleDeg = 60.f;
	float EdgeWearWidth = 8.f;
	float FloorKickHeight = 30.f;

	EGrungifyMaskOutput MaskOutput = EGrungifyMaskOutput::Auto;
	float MaskTexelSize = 4.f;
	int32 MaxMaskResolution = 2048;
	int32 MaskUVChannel = 1;

	const FGrungifyFloorField* Floor = nullptr;
	TArray<FGrungifyRunoffSource> Sources;
};

/** Everything resolved for one material slot. */
struct FGrungifySlotInfo
{
	EGrungifySurfaceType Surface = EGrungifySurfaceType::Stone;
	FGrungifySurfaceProfile Profile;
	bool bRail = false;
	bool bExcluded = false;
	const FGrungifyGate* Gate = nullptr;
	const FGrungifyGateSampler* GateSampler = nullptr;
	FString Description;
};

struct FGrungifyComponentContext
{
	TArray<FGrungifySlotInfo> Slots;
};

struct FGrungifyComponentResult
{
	UStaticMeshComponent* Component = nullptr;

	/** VertexColor or Texture (never Auto). */
	EGrungifyMaskOutput Output = EGrungifyMaskOutput::VertexColor;

	/** One colour per render vertex, per LOD (vertex output). */
	TArray<TArray<FColor>> LODColors;

	/** Square BGRA mask texture in the mask UV channel (texture output). */
	int32 TextureSize = 0;
	TArray<FColor> TexturePixels;

	EGrungifySurfaceType DominantSurface = EGrungifySurfaceType::Stone;
	bool bNanite = false;
	int32 Vertices = 0;
	FLinearColor MeanMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);
	FLinearColor MaxMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);
	FString Notes;

	/** Index of this component's entry in the zone's run report. */
	int32 ReportIndex = INDEX_NONE;
};

namespace Grungify
{
	float Hash3(int32 X, int32 Y, int32 Z, uint32 Seed);
	float ValueNoise(const FVector& P, uint32 Seed);
	float Fbm(const FVector& P, uint32 Seed, int32 Octaves = 3);

	/** Splits "SM_HVACVent_02" into {"sm", "hvac", "vent", "02"}. */
	void TokenizeName(const FString& Name, TArray<FString>& OutTokens);
	/** True if any token starts with any keyword (so "railing" matches "rail" but "event" does not match "vent"). */
	bool NameMatches(const FString& Name, const TArray<FString>& Keywords);

	EGrungifySurfaceType ClassifySlot(const UStaticMeshComponent* Component, int32 MaterialIndex, EGrungifySurfaceType Override);
	bool IsRail(const UStaticMeshComponent* Component);
	/** Pass either an actor or a component. */
	bool IsVent(const AActor* Actor, const UPrimitiveComponent* Component);
	bool HasIgnoreTag(const AActor* Actor, const UActorComponent* Component);

	/** True if the rule targets this slot. */
	bool RuleMatchesSlot(const FGrungifySlotRule& Rule, const UStaticMeshComponent* Component, int32 MaterialIndex);

	bool GenerateComponent(UStaticMeshComponent* Component, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult);

	/** Snapshot of everything Grungify can change on a component. */
	void CaptureRecord(UStaticMeshComponent* Component, FGrungifyComponentRecord& OutRecord);
	/** Puts the component back exactly as captured. Calls Modify() only when bTransactional. */
	void RestoreRecord(const FGrungifyComponentRecord& Record, bool bTransactional = true);

	void WriteVertexColors(UStaticMeshComponent* Component, const TArray<TArray<FColor>>& LODColors);
	/** Writes the Grungify custom data values. Returns false (and writes nothing) if the indices hold someone else's data. */
	bool CustomDataSlotsFree(const TArray<float>& OriginalData);
	void WriteCustomData(UStaticMeshComponent* Component, const float (&Values)[CPD_Count]);
	UTexture2D* CreateMaskTexture(UStaticMeshComponent* Component, int32 Size, const TArray<FColor>& Pixels);
	/** Puts a material instance carrying the mask texture on every slot, parented to the slot's current material (or to ParentOverride). */
	void BindMaskTexture(UStaticMeshComponent* Component, UTexture2D* Texture, UMaterialInterface* ParentOverride);
	void SwapMaterials(UStaticMeshComponent* Component, UMaterialInterface* Material);
}

#endif // WITH_EDITOR
