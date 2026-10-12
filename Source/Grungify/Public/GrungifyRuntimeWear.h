// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/HitResult.h"
#include "Engine/TextureDefines.h"
#include "Subsystems/WorldSubsystem.h"
#include "GrungifyTypes.h"
#include "GrungifyRuntimeWear.generated.h"

class AActor;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPrimitiveComponent;
class UStaticMeshComponent;
class UTexture;
class UTexture2D;

/**
 * A baked mask texture and the live copy that replaces it while runtime wear is painted. Strokes accumulate in an
 * exact CPU copy, changing only the selected channel, and upload the changed rectangle to Texture. Nothing is ever
 * blended on the GPU, so the other channels (alpha included) stay bit-exact.
 */
USTRUCT()
struct FGrungifyRuntimeWearTarget
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UTexture> Source;

	/** Transient B8G8R8A8 texture the dynamic instances sample. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Texture;

	/** CPU copy of the mask (Width x Height, row-major), read back from the GPU once at creation. */
	TArray<FColor> Pixels;

	int32 Width = 0;
	int32 Height = 0;

	/**
	 * The source's addressing per axis, as the material samples it: wrapped strokes continue across the opposite edge,
	 * mirrored ones reflect back, clamped ones stop at it (and hits beyond it paint the edge texels they show).
	 */
	TEnumAsByte<TextureAddress> AddressX = TA_Wrap;
	TEnumAsByte<TextureAddress> AddressY = TA_Wrap;
};

/** One material slot switched to a dynamic instance that reads the live mask textures. */
USTRUCT()
struct FGrungifyRuntimeWearSlot
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	int32 MaterialIndex = INDEX_NONE;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> OriginalMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Dynamic;

	/** Indices into FGrungifyRuntimeWearState::Targets. */
	UPROPERTY(Transient)
	int32 MaskTarget = INDEX_NONE;

	UPROPERTY(Transient)
	int32 ExtendedTarget = INDEX_NONE;

	UPROPERTY(Transient)
	float TexelSizeCm = 4.f;
};

USTRUCT()
struct FGrungifyRuntimeWearState
{
	GENERATED_BODY()

	/** The painted component. Weak so a destroyed component is pruned instead of kept alive. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UStaticMeshComponent> Component;

	/** Shared by every slot that bound the same baked texture, so all of them see the same paint. */
	UPROPERTY(Transient)
	TArray<FGrungifyRuntimeWearTarget> Targets;

	UPROPERTY(Transient)
	TArray<FGrungifyRuntimeWearSlot> Slots;

	/** Raw arrays preserve inherited slots, null holes and trailing entries on reset. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> OriginalOverrideMaterials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> RuntimeOverrideMaterials;

	bool bWarnedNoExtended = false;
};

/**
 * Paints wear into Texture-output meshes at runtime (impacts, footsteps, fire). On a component's first wear its
 * baked mask textures are read back from the GPU into exact CPU copies and its slots switch to dynamic instances
 * that read live transient textures; ResetRuntimeWear switches back to the baked result. Only active material
 * instances with an explicit mask override are painted. All eight layers are supported: a stroke changes only its
 * layer's channel (R, G, B or A of the mask or extended mask) and the other seven stay exact.
 *
 * Hit UVs come from UGameplayStatics::FindCollisionUV, which needs Project Settings > Physics >
 * "Support UV From Hit Results" and a complex-collision trace that returns the face index.
 */
UCLASS()
class GRUNGIFY_API UGrungifyRuntimeWearSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static UGrungifyRuntimeWearSubsystem* Get(const UObject* WorldContextObject);

	/** Adds a patch of one layer to an active Texture-output slot with an explicit baked mask. Invalid layer values return false without mutation. */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Runtime Wear")
	bool AddWearAtHit(const FHitResult& Hit, EGrungifyWearLayer Layer, float RadiusCm = 20.f, float Amount = 0.5f);

	/** Traces a short distance from Location along Direction (complex collision, Visibility channel) and paints where it hits. */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Runtime Wear")
	bool AddWearAtLocation(const FVector& Location, const FVector& Direction, EGrungifyWearLayer Layer, float RadiusCm = 20.f, float Amount = 0.5f);

	/** Restores the baked masks on Component, or on every painted component when it is null. */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Runtime Wear")
	void ResetRuntimeWear(UPrimitiveComponent* Component = nullptr);

	/** AddWearAtLocation with an explicit trace length and an actor the trace ignores (usually the emitter). */
	bool TraceAndAddWear(const FVector& Location, const FVector& Direction, float TraceDistanceCm, const AActor* IgnoredActor, EGrungifyWearLayer Layer, float RadiusCm, float Amount);

	/**
	 * Reads a painted slot's live mask (or extended mask) back from the GPU: the texture the material samples is drawn
	 * into a render target and read, so the result is what the renderer sees, not the CPU copy. False when the
	 * component has no runtime wear on that slot, or there is no rendering RHI. Flushes rendering; for tests and QA.
	 */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Runtime Wear")
	bool ReadRuntimeWearMask(UPrimitiveComponent* Component, int32 MaterialIndex, bool bExtended, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight);

	virtual void Deinitialize() override;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	static bool SupportsRuntimeLayer(EGrungifyWearLayer Layer);
	static bool IsEligibleMaterial(const UMaterialInterface* Material);
	FGrungifyRuntimeWearState* FindOrCreateState(UStaticMeshComponent* Component);
	int32 FindOrCreateTarget(FGrungifyRuntimeWearState& State, UTexture* Source);
	bool PaintSlot(FGrungifyRuntimeWearState& State, int32 SlotIndex, const FVector2D& UV, EGrungifyWearLayer Layer, float RadiusCm, float Amount, bool& bOutMissingExtended);
	static bool PaintTarget(FGrungifyRuntimeWearTarget& Target, const FVector2D& UV, float RadiusTexels, int32 Channel, float Amount);
	/** What a stroke adds to a texel whose centre is (DX, DY) texels from the stroke centre. Smooth falloff to zero at the radius. */
	static uint8 StrokeIncrement(float DX, float DY, float RadiusTexels, float Amount);
	static bool ReadTextureOnGPU(UTexture* Texture, int32 Width, int32 Height, TArray<FColor>& OutPixels);
	/** The mask UV the material computes at the hit: the settings channel blended towards UV0 by Grungify Use UV0. */
	static bool ResolveMaskUV(const FHitResult& Hit, const UMaterialInterface* Material, FVector2D& OutUV);
	/** Where a mask UV lands in texture space (0..1) under an axis's address mode. */
	static double AddressUV(double UV, TextureAddress Address);
	/** The texel an out-of-range index addresses (wrap, mirror), or INDEX_NONE where clamped addressing has none. */
	static int32 AddressTexel(int32 Index, int32 Size, TextureAddress Address);
	static void RestoreState(UStaticMeshComponent* Component, const FGrungifyRuntimeWearState& State);
	void PruneStaleStates();

	/**
	 * One entry per painted component. A UPROPERTY array (rather than a map keyed by a weak pointer, which reflection
	 * does not support) so GC sees the live textures, dynamic instances and original materials the states hold.
	 */
	UPROPERTY(Transient)
	TArray<FGrungifyRuntimeWearState> States;

	int32 FindStateIndex(const UStaticMeshComponent* Component) const;

	bool bWarnedMissingUV = false;
	bool bWarnedMissingTexelSize = false;
	bool bWarnedUnsupportedLayer = false;

#if WITH_DEV_AUTOMATION_TESTS
	friend struct FGrungifyRuntimeWearTestAccess;
#endif
};

/** Emits runtime wear under its owner, e.g. from footstep anim notifies. */
UCLASS(ClassGroup = Grungify, meta = (BlueprintSpawnableComponent))
class GRUNGIFY_API UGrungifyRuntimeWearComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGrungifyRuntimeWearComponent();

	/** How far below the owner's origin the downward trace reaches. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (ClampMin = "0", Units = "cm"))
	float TraceDistanceCm = 250.f;

	/** Traces down and paints one layer under the owner (Handling by default, e.g. footsteps). */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Runtime Wear")
	bool EmitWear(EGrungifyWearLayer Layer = EGrungifyWearLayer::Handling, float RadiusCm = 20.f, float Amount = 0.5f);
};
