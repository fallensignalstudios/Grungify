// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GrungifyInspection.generated.h"

class AGrungifyWeatheringZone;
class UMeshComponent;

/** One material slot of an inspected component. */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifySlotInspection
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	int32 Slot = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString SlotName;

	/** Full object path of the slot's current material (resolvable with ResolveGrungifyObject while loaded). */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Material;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString MaterialClass;

	/** The material is a zone-owned instance inside this component (preview or apply). */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	bool bOwnedInstance = false;

	/** The material is a standalone baked instance (Bake To Assets). */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	bool bBakedInstance = false;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Parent;

	/** False when the material has no Grungify Active parameter (it doesn't use Grungify). */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	bool bHasActiveParameter = false;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	float Active = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	float SurfaceOverride = -1.f;

	/** Bound mask textures (full object paths; embedded textures are subobjects of the component). */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString MaskTexture;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FIntPoint MaskTextureSize = FIntPoint::ZeroValue;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString ExtendedMaskTexture;

	/** The UV channel the material samples masks from (the settings channel, or 0 when Grungify Use UV0 is set). */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	int32 EffectiveUVChannel = INDEX_NONE;

	/** Read failures for this slot, each with the reason. */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Unavailable;
};

/**
 * Everything Grungify has done to one component, read without changing anything (no Preview, Apply, save or load).
 * Fields that couldn't be read are listed in Unavailable with the reason.
 */
USTRUCT(BlueprintType)
struct GRUNGIFY_API FGrungifyComponentInspection
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	bool bValid = false;

	// ---------------------------------------------------------------- Identity and persistence

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Component;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString ComponentClass;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Owner;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Outer;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Package;

	/** The component or its package is transient, so nothing on it is saved. */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	bool bTransient = false;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString MeshAsset;

	// ---------------------------------------------------------------- Ownership

	/** "none", "previewed", "applied", "previewed+applied" (another zone previews this zone's applied mesh) or "baked". */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString State;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString PreviewedBy;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString AppliedBy;

	// ---------------------------------------------------------------- Custom primitive data

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	int32 CustomDataBase = 0;

	/** Saved defaults and live values, separately (they can legitimately differ). */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<float> DefaultCustomData;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<float> LiveCustomData;

	/** Live mask mode at the base index (0 when absent). */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	float MaskMode = 0.f;

	/** "vertex", "texture", "instanced" or "none". */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString MaskModeName;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	float DebugChannel = 0.f;

	// ---------------------------------------------------------------- Vertex colours and instances

	/** Vertex count of each LOD's override colours (0 = no override on that LOD). Static meshes only. */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<int32> OverrideVertexColorCounts;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	bool bInstanced = false;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	int32 InstanceCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	int32 NumInstanceCustomDataFloats = 0;

	/** Per instance, Grungify's four floats (chips, dust, runoff, handling) at Instance Custom Data Index. */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<FLinearColor> InstanceMasks;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<FGrungifySlotInspection> Slots;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Unavailable;
};

/** Read-only inspection for QA scripts (Blueprint and Python). Nothing here modifies, saves or loads anything. */
UCLASS()
class GRUNGIFY_API UGrungifyInspectionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Inspect one static or skeletal mesh component, admitted and excluded slots alike. */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Inspection")
	static FGrungifyComponentInspection InspectComponent(UMeshComponent* Component);

	/**
	 * Inspect every component a zone previews or has applied to. Records for components that aren't loaded
	 * (another World Partition cell) come back with bValid false, their saved path and the reason.
	 */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Inspection")
	static TArray<FGrungifyComponentInspection> InspectZone(AGrungifyWeatheringZone* Zone);

	/**
	 * Finds an already-loaded object by full path, including subobjects such as a component's embedded mask texture
	 * ("/Game/Map.Map:PersistentLevel.Actor.Component.GrungifyMask_0"). Never loads a package; null if not loaded.
	 */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Inspection")
	static UObject* ResolveGrungifyObject(const FString& ObjectPath);
};
