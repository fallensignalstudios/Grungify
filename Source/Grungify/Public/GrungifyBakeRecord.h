// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "GrungifyBakeRecord.generated.h"

/**
 * Stored on every baked material instance (Bake To Assets): what the masks were baked for. A baked instance is
 * portable to any component drawing the same mesh build with the same UV layout; on anything else its masks land
 * in the wrong places. Editor-only; inspection reads it (see UGrungifyInspectionLibrary::CheckBakeCompatibility).
 */
UCLASS()
class GRUNGIFY_API UGrungifyBakeRecord : public UAssetUserData
{
	GENERATED_BODY()

public:
	virtual bool IsEditorOnly() const override { return true; }

	/** Static or skeletal mesh asset the masks were baked on. */
	UPROPERTY(VisibleAnywhere, Category = "Grungify")
	FSoftObjectPath SourceMesh;

	/** Mesh build identity at bake time (derived-data key hash for static meshes, LOD0 shape for skeletal ones). */
	UPROPERTY(VisibleAnywhere, Category = "Grungify")
	int64 SourceMeshBuild = 0;

	/** UV channel the masks were baked in. */
	UPROPERTY(VisibleAnywhere, Category = "Grungify")
	int32 UVChannel = 1;

	UPROPERTY(VisibleAnywhere, Category = "Grungify")
	int32 TextureSize = 0;

	UPROPERTY(VisibleAnywhere, Category = "Grungify")
	int32 Slot = 0;

	/** Where it came from, for people reading the asset. */
	UPROPERTY(VisibleAnywhere, Category = "Grungify")
	FString SourceComponent;

	UPROPERTY(VisibleAnywhere, Category = "Grungify")
	FString Zone;

	UPROPERTY(VisibleAnywhere, Category = "Grungify")
	int32 Seed = 0;

	UPROPERTY(VisibleAnywhere, Category = "Grungify")
	FString BakedAt;
};
