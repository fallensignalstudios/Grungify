// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "GrungifyTypes.h"
#include <atomic>

#if WITH_EDITOR

class AActor;
class AGrungifyWeatheringZone;
class UActorComponent;
class UMaterialInterface;
class UPrimitiveComponent;
class UStaticMeshComponent;
class UMeshComponent;
class USkeletalMeshComponent;
class UTexture2D;
class UMaterialInstanceConstant;
class UInstancedStaticMeshComponent;
class UWorld;
struct FStaticMeshLODResources;

/** Floor heights sampled on a grid over the zone. Each cell can hold several floors (storeys, stairs). */
class FGrungifyFloorField
{
public:
	void BuildFlat(double InFloorZ);
	/** Returns false if ShouldCancel asked to stop (the field is then flat at the zone bottom). */
	bool BuildTraced(UWorld* World, const FBox& InBounds, double Spacing, const AActor* IgnoreActor,
		const TFunction<bool()>& ShouldCancel = TFunction<bool()>());

	int32 NumCells() const { return NumX * NumY; }

	/** Highest floor at or just below P. */
	double FloorBelow(const FVector& P) const;

	uint32 Hash() const;

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
/** How open each point is to the sky, from ray casts on a grid over the zone (1 = open sky, 0 = fully sheltered). */
class FGrungifyExposureField
{
public:
	/** Returns false if cancelled. */
	bool Build(UWorld* World, const FBox& InBounds, double Spacing, int32 NumRays, double RayLength, const AActor* IgnoreActor,
		const TFunction<bool()>& ShouldCancel = TFunction<bool()>());

	/** Trilinear lookup. Callers offset surface points along their normal so they sample the air side. */
	float At(const FVector& P) const;

	bool IsValid() const { return NumX > 0; }
	int32 NumNodes() const { return NumX * NumY * NumZ; }
	uint32 Hash() const;

private:
	FBox Bounds = FBox(ForceInit);
	FVector Cell = FVector::OneVector;
	int32 NumX = 0;
	int32 NumY = 0;
	int32 NumZ = 0;
	TArray<float> Values;
};

/** Capsules (segments with a radius) in a uniform grid, for fast "near a path" queries over many samples. */
class FGrungifySegmentField
{
public:
	struct FSegment
	{
		FVector A;
		FVector B;
		float Radius = 10.f;
		/** Caller-defined strength (traffic weight, water amount...). */
		float Value = 1.f;
		/** 0 at the path start, 1 at its end. */
		float AlongA = 0.f;
		float AlongB = 1.f;
	};

	void Reset(double InCellSize);
	void Add(const FSegment& Segment);
	bool IsEmpty() const { return Segments.Num() == 0; }
	uint32 Hash() const;

	/** Calls Fn(Segment, Distance, AlongFraction) for every segment whose capsule contains P. */
	template <typename FnType>
	void ForEachNear(const FVector& P, FnType&& Fn) const
	{
		const TArray<int32>* List = Cells.Find(CellOf(P));
		if (!List)
		{
			return;
		}
		for (const int32 Index : *List)
		{
			const FSegment& Seg = Segments[Index];
			const FVector Closest = FMath::ClosestPointOnSegment(P, Seg.A, Seg.B);
			const double Distance = FVector::Dist(P, Closest);
			if (Distance < Seg.Radius)
			{
				const double Length = FVector::Dist(Seg.A, Seg.B);
				const float T = Length > UE_KINDA_SMALL_NUMBER ? float(FVector::Dist(Seg.A, Closest) / Length) : 0.f;
				Fn(Seg, float(Distance), FMath::Lerp(Seg.AlongA, Seg.AlongB, T));
			}
		}
	}

private:
	TArray<FSegment> Segments;
	TMap<FIntVector, TArray<int32>> Cells;
	double CellSize = 50.0;

	FIntVector CellOf(const FVector& P) const
	{
		return FIntVector(FMath::FloorToInt(P.X / CellSize), FMath::FloorToInt(P.Y / CellSize), FMath::FloorToInt(P.Z / CellSize));
	}
};

/** A lamp or fire that leaves soot (and scorch, for fire). */
struct FGrungifyHeatSource
{
	FVector Location = FVector::ZeroVector;
	bool bFire = false;
};

/** Copy of a wear stamp, safe to read from worker threads. */
struct FGrungifyStampData
{
	FTransform Transform;
	float Radius = 50.f;
	float Falloff = 0.6f;
	float EdgeNoise = 0.35f;
	FGrungifyStampLayers Layers;
};

/** Where pooled water collects (end of a traced water path on flat ground). */
struct FGrungifyPool
{
	FVector Center = FVector::ZeroVector;
	float Radius = 40.f;
};

class FGrungifyGateSampler
{
public:
	bool Init(UTexture2D* Texture, FString& OutError);
	float Sample(const FVector2f& UV, EGrungifyChannel Channel) const;
	/** Identity of the copied source pixels and layout, computed once on Init. */
	uint32 Hash() const { return ContentHash; }

private:
	int32 Width = 0;
	int32 Height = 0;
	int32 BytesPerPixel = 0;
	TArray64<uint8> Pixels;
	uint32 ContentHash = 0;
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

	/** Polled on the game thread between work batches. Returning true stops generation of the current mesh. */
	TFunction<bool()> ShouldCancel;

	bool CancelRequested() const { return ShouldCancel && ShouldCancel(); }

	// --- Optional inputs; everything below is off by default so the original layers are unchanged.

	/** Sky openness; null = off. */
	const FGrungifyExposureField* Exposure = nullptr;
	float SkyInfluence = 0.f;

	FGrungifyExtendedLayers Extended;
	/** Produce the second (extended) mask texture. */
	bool bExtended = false;

	TArray<FGrungifyHeatSource> HeatSources;
	TArray<FVector> Fasteners;
	TArray<FGrungifyStampData> Stamps;
	TArray<FGrungifyPool> Pools;

	/** Traced water paths (replace straight streaks for their sources). */
	const FGrungifySegmentField* Flow = nullptr;
	/** Traffic paths (scuffs). */
	const FGrungifySegmentField* Traffic = nullptr;

	/** Candidate points per m² to collect for decal/mesh scatter (0 = none). */
	float ScatterDensity = 0.f;
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
	UMeshComponent* Component = nullptr;

	/** VertexColor or Texture (never Auto). */
	EGrungifyMaskOutput Output = EGrungifyMaskOutput::VertexColor;

	/** One colour per render vertex, per LOD (vertex output). */
	TArray<TArray<FColor>> LODColors;

	/** UV channel the texture was baked in (skeletal meshes may fall back to UV0). */
	int32 MaskUVChannelUsed = INDEX_NONE;

	/** Square BGRA mask texture in the mask UV channel (texture output). */
	int32 TextureSize = 0;
	TArray<FColor> TexturePixels;

	EGrungifySurfaceType DominantSurface = EGrungifySurfaceType::Stone;
	bool bNanite = false;

	/** Generation stopped because ShouldCancel returned true; the result must not be written. */
	bool bCancelled = false;

	/** Mask UV channel checks (texture output candidates). */
	FGrungifyUVReport MaskUV;
	int32 Vertices = 0;
	FLinearColor MeanMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);
	FLinearColor MaxMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);
	FString Notes;

	/** Index of this component's entry in the zone's run report. */
	int32 ReportIndex = INDEX_NONE;

	/** Auto marks an excluded slot; admitted slots carry their resolved material family. */
	TArray<EGrungifySurfaceType> MaterialSurfaces;

	/** Second mask texture (moisture, soot, scorch, moss); same size as TexturePixels. Empty when unused. */
	TArray<FColor> ExtendedPixels;

	/** World centimetres per mask texel, for runtime wear. */
	float TexelSizeCm = 0.f;

	/** Instanced meshes: chips, dust, runoff, handling per instance. */
	bool bInstanced = false;
	TArray<FVector4f> InstanceMasks;

	/** Surface points with their masks, for scatter. */
	struct FScatterSample
	{
		FVector P;
		FVector N;
		FLinearColor Primary;
		FLinearColor Extended;
		EGrungifySurfaceType Surface = EGrungifySurfaceType::Stone;
		/** Deterministic 0..1 value for thinning and random sizes. */
		float Random = 0.f;
		float Random2 = 0.f;
	};
	TArray<FScatterSample> ScatterSamples;
};

/**
 * One landscape proxy's share of a zone: a grid over the zone's XY rectangle, traced on the game thread
 * (height, normal, distance to the nearest wall or prop), then turned into masks by GenerateLandscapeMasks.
 */
struct FGrungifyLandscapeResult
{
	TWeakObjectPtr<AActor> Proxy;
	FString ProxyPath;
	/** World-space XY rectangle covered by the textures. */
	FVector2D Min = FVector2D::ZeroVector;
	FVector2D Size = FVector2D::ZeroVector;
	int32 SizeX = 0;
	int32 SizeY = 0;

	// Inputs (per texel, row-major). bValid is false where there's no landscape under the texel.
	TArray<FVector> Positions;
	TArray<FVector3f> Normals;
	TArray<float> Contact;
	TArray<bool> Valid;

	// Outputs.
	TArray<FColor> Pixels;
	TArray<FColor> ExtendedPixels;
	FLinearColor MeanMasks = FLinearColor::Transparent;
	int32 Covered = 0;
	float Seconds = 0.f;
	bool bCancelled = false;
};

/** A cached per-component result and the inputs it was made from. */
struct FGrungifyCacheEntry
{
	uint32 Key = 0;
	TSharedPtr<FGrungifyComponentResult> Result;
	FGrungifyComponentReport Report;
	int64 Bytes = 0;
	double LastUsed = 0.0;
};

/**
 * One run's inputs and outputs. Prepared on the game thread; items are generated either there (with a progress
 * dialog) or on a worker thread (background live preview), then collected on the game thread.
 */
struct FGrungifyGenerationJob
{
	FString Operation;
	FGrungifyGenerationParams Params;
	FGrungifyFloorField Floor;
	FGrungifyExposureField Exposure;
	FGrungifySegmentField Flow;
	FGrungifySegmentField Traffic;
	TMap<UTexture2D*, TUniquePtr<FGrungifyGateSampler>> Samplers;
	/** Gate copies, so worker threads never read the zone's (editable) rule array. */
	TArray<TUniquePtr<FGrungifyGate>> GateCopies;
	uint32 EnvironmentHash = 0;

	struct FItem
	{
		TWeakObjectPtr<UMeshComponent> Component;
		FString Path;
		uint32 Key = 0;
		bool bInstanced = false;
		bool bSkeletal = false;
		FGrungifyComponentContext Context;
		FGrungifyComponentReport Report;
		FGrungifyComponentResult Result;
		bool bGenerated = false;
		bool bFromCache = false;
		/** Generation found nothing usable (reported as skipped). */
		bool bFailed = false;
		float Seconds = 0.f;
		/** Apply with Take Over: the zone that owns this mesh now. */
		TWeakObjectPtr<AGrungifyWeatheringZone> TakeOverFrom;
	};
	TArray<FItem> Items;

	/** Set from the game thread to stop a background run. */
	std::atomic<bool> bCancelRequested { false };
	std::atomic<bool> bFinished { false };
	bool bCancelled = false;
	FString CancelledDuring;
	double StartSeconds = 0.0;
	TFuture<void> Future;

	/** Generates one item (thread-safe given the GC guard the caller holds). */
	void GenerateItem(FItem& Item);
};

namespace Grungify
{
	float Hash3(int32 X, int32 Y, int32 Z, uint32 Seed);
	float ValueNoise(const FVector& P, uint32 Seed);
	float Fbm(const FVector& P, uint32 Seed, int32 Octaves = 3);

	/** Splits "SM_HVACVent_02" into {"sm", "hvac", "vent", "02"}. */
	void TokenizeName(const FString& Name, TArray<FString>& OutTokens);
	/** True if any token starts with any keyword (so "railing" matches "rail" but "event" does not match "vent"). */
	bool NameMatches(const FString& Name, const TArray<FString>& Keywords, FString* OutKeyword = nullptr);

	/** OutSource explains the decision (tag, physical material, keyword and where it was found, fallback...). */
	/** The static or skeletal mesh asset a component draws (null for other mesh components). */
	const UObject* MeshAssetOf(const UMeshComponent* Component);
	/** Material slot name, for static and skeletal meshes alike. */
	FString SlotNameOf(const UMeshComponent* Component, int32 MaterialIndex);
	/** UV channels of LOD0 (static or skeletal render data). */
	int32 NumUVChannelsOf(const UMeshComponent* Component);
	/** Changes when the mesh is rebuilt (derived-data key for static meshes, LOD0 shape for skeletal ones). */
	uint32 MeshBuildHash(const UMeshComponent* Component);
	/**
	 * Landscape masks from a traced grid: dust, runoff, moisture and moss through the same evaluation as meshes (as a
	 * stone floor with Profile), plus ContactStrength extra dust and damp where Contact is high.
	 */
	bool GenerateLandscapeMasks(FGrungifyLandscapeResult& InOut, const FGrungifySurfaceProfile& Profile, float ContactStrength, const FGrungifyGenerationParams& Params);

	/** Skeletal meshes: texture masks baked from the reference pose of LOD0. */
	bool GenerateSkinned(USkeletalMeshComponent* Component, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult);

	EGrungifySurfaceType ClassifySlot(const UMeshComponent* Component, int32 MaterialIndex, EGrungifySurfaceType Override, FString* OutSource = nullptr);
	bool IsRail(const UMeshComponent* Component, FString* OutSource = nullptr);
	/** Pass either an actor or a component. */
	bool IsVent(const AActor* Actor, const UPrimitiveComponent* Component);
	bool HasIgnoreTag(const AActor* Actor, const UActorComponent* Component);

	/** True if the rule targets this slot. */
	bool RuleMatchesSlot(const FGrungifySlotRule& Rule, const UMeshComponent* Component, int32 MaterialIndex, FString* OutWhy = nullptr);

	/** Hashes effective generation inputs, not rounded diagnostic labels. */
	uint32 HashSlotContext(const FGrungifyComponentContext& Context);

	/**
	 * Checks the mask UV channel of a mesh LOD: presence, 0..1 range, overlapping islands, zero-area triangles,
	 * coverage and texel-density spread. Overlap is measured on a coarse raster, independent of the bake.
	 */
	FGrungifyUVReport ValidateMaskUVs(const FStaticMeshLODResources& LOD, const FTransform& Transform, int32 Channel);

	bool GenerateComponent(UStaticMeshComponent* Component, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult);

	/** Instanced meshes: one set of masks per instance (sampled around each instance's bounds). */
	bool GenerateInstanced(UInstancedStaticMeshComponent* Component, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult);

	/**
	 * Follows water from Start: down walls, along sloped surfaces, over edges, until it pools on flat ground or runs out.
	 * Game thread only (line traces).
	 */
	void TraceWaterPath(UWorld* World, const FVector& Start, float StepCm, int32 MaxSteps, const AActor* IgnoreActor,
		TArray<FVector>& OutPath, TArray<FGrungifyPool>& OutPools);

	/** Stamp weight (0..1) at P, matching AGrungifyWearStamp::WeightAt. */
	float StampWeight(const FGrungifyStampData& Stamp, const FVector& P, float FeatureScale);

	/** Snapshot of everything Grungify can change on a component. */
	void CaptureRecord(UMeshComponent* Component, FGrungifyComponentRecord& OutRecord);
	/** Puts the component back exactly as captured. Calls Modify() only when bTransactional. */
	bool RestoreRecord(const FGrungifyComponentRecord& Record, bool bTransactional = true);

	bool WriteVertexColors(UStaticMeshComponent* Component, const TArray<TArray<FColor>>& LODColors,
		const TArray<EGrungifySurfaceType>* MaterialSurfaces = nullptr);
	/** Both saved and live values must leave the complete requested range free. */
	bool CustomDataSlotsFree(const TArray<float>& OriginalData, const TArray<float>& OriginalLiveData, int32 Base);
	/** Updates only our fields in each array, preserving unrelated live-only values. */
	bool WriteCustomData(UMeshComponent* Component, const float (&Values)[CPD_Count]);
	UTexture2D* CreateMaskTexture(UMeshComponent* Component, int32 Size, const TArray<FColor>& Pixels);
	/** Owns only admitted slots; Texture is null for vertex masks. Excluded material identities are retained. */
	bool BindOwnedMaterials(UMeshComponent* Component, UTexture2D* Texture,
		const TArray<EGrungifySurfaceType>& MaterialSurfaces, UMaterialInterface* ParentOverride,
		const FGrungifyPalette* Palette = nullptr, UTexture2D* ExtendedTexture = nullptr, float TexelSizeCm = 0.f,
		bool bUseUV0 = false);

	/** Writes per-instance masks at UGrungifySettings::InstanceCustomDataIndex. False (nothing written) if those indices are in use. */
	bool WriteInstanceMasks(UInstancedStaticMeshComponent* Component, const TArray<FVector4f>& Masks);

	/** True if none of the per-instance custom data floats Grungify uses hold another system's values. */
	bool InstanceSlotsFree(const FGrungifyComponentRecord& OriginalState);

	/** Shared setup of an owned or baked instance: textures, activation, surface, texel size, palette. */
	void ConfigureOwnedInstance(UMaterialInstanceConstant* Instance, UMaterialInterface* Parent, UTexture2D* Texture, UTexture2D* ExtendedTexture,
		EGrungifySurfaceType Surface, const FGrungifyPalette* Palette, float TexelSizeCm);
}

#endif // WITH_EDITOR
