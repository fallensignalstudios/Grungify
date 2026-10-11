#pragma once

#include "CoreMinimal.h"
#include "GrungifyTypes.h"

#if WITH_EDITOR

class AActor;
class UActorComponent;
class UMaterialInterface;
class UPrimitiveComponent;
class UStaticMeshComponent;
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
	float FloorKickHeight = 30.f;

	/** Indexed by surface type - 1 (Stone, Metal, Trim). */
	FGrungifySurfaceProfile Profiles[3];

	const FGrungifyFloorField* Floor = nullptr;
	TArray<FGrungifyRunoffSource> Sources;
};

struct FGrungifyComponentContext
{
	/** Resolved surface per material slot (never Auto). */
	TArray<EGrungifySurfaceType> SlotSurfaces;
	bool bIsRail = false;
};

struct FGrungifyComponentResult
{
	UStaticMeshComponent* Component = nullptr;
	/** One colour per render vertex, per LOD. Empty for LODs that could not be read. */
	TArray<TArray<FColor>> LODColors;
	EGrungifySurfaceType DominantSurface = EGrungifySurfaceType::Stone;
	bool bNanite = false;
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

	bool GenerateComponent(UStaticMeshComponent* Component, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult);

	void CaptureRecord(UStaticMeshComponent* Component, bool bCaptureMaterials, FGrungifyComponentRecord& OutRecord);
	void RestoreRecord(const FGrungifyComponentRecord& Record);
	void WriteVertexColors(UStaticMeshComponent* Component, const TArray<TArray<FColor>>& LODColors);
	void WriteCustomData(UStaticMeshComponent* Component, const float (&Values)[CPD_Count]);
	void SwapMaterials(UStaticMeshComponent* Component, UMaterialInterface* Material);
}

#endif // WITH_EDITOR
