#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/HitResult.h"
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
class UTextureRenderTarget2D;

/** A baked mask texture and the render target that replaces it while runtime wear is painted. */
USTRUCT()
struct FGrungifyRuntimeWearTarget
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UTexture> Source;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;
};

/** One material slot switched to a dynamic instance that reads the render targets. */
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

	bool bWarnedNoExtended = false;
};

/**
 * Paints wear into Texture-output meshes at runtime (impacts, footsteps, fire). On a component's first wear its
 * baked mask textures are copied into render targets and its slots switch to dynamic instances that read them;
 * ResetRuntimeWear switches back to the baked result.
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

	/** Adds a soft round patch of Layer at a complex-collision hit on a Texture-output static mesh. Returns false if nothing was painted. */
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

	virtual void Deinitialize() override;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	FGrungifyRuntimeWearState* FindOrCreateState(UStaticMeshComponent* Component);
	int32 FindOrCreateTarget(FGrungifyRuntimeWearState& State, UTexture* Source);
	UTexture2D* GetBrushTexture();
	void PaintBrush(UTextureRenderTarget2D* Target, const FVector2D& UV, float DiameterTexels, int32 Channel, float Amount);
	static void RestoreState(UStaticMeshComponent* Component, const FGrungifyRuntimeWearState& State);
	void PruneStaleStates();

	/**
	 * One entry per painted component. A UPROPERTY array (rather than a map keyed by a weak pointer, which reflection
	 * does not support) so GC sees the render targets, dynamic instances and original materials the states hold.
	 */
	UPROPERTY(Transient)
	TArray<FGrungifyRuntimeWearState> States;

	int32 FindStateIndex(const UStaticMeshComponent* Component) const;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> BrushTexture;

	bool bWarnedMissingUV = false;
	bool bWarnedMissingTexelSize = false;
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

	/** Traces straight down from the owner and paints Layer where the trace lands. */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Runtime Wear")
	bool EmitWear(EGrungifyWearLayer Layer = EGrungifyWearLayer::Handling, float RadiusCm = 20.f, float Amount = 0.5f);
};
