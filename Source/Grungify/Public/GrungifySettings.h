#pragma once

#include "CoreMinimal.h"
#include "Chaos/ChaosEngineInterface.h"
#include "Engine/DeveloperSettings.h"
#include "GrungifyTypes.h"
#include "GrungifySettings.generated.h"

class UMaterialInterface;

/** Project-wide Grungify settings (Project Settings > Plugins > Grungify). */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Grungify"))
class GRUNGIFY_API UGrungifySettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UGrungifySettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	static const UGrungifySettings* Get() { return GetDefault<UGrungifySettings>(); }

	/** Actor or component tag that forces a surface to Stone. */
	UPROPERTY(config, EditAnywhere, Category = "Tags")
	FName StoneTag = TEXT("Grungify.Stone");

	UPROPERTY(config, EditAnywhere, Category = "Tags")
	FName MetalTag = TEXT("Grungify.Metal");

	UPROPERTY(config, EditAnywhere, Category = "Tags")
	FName TrimTag = TEXT("Grungify.Trim");

	/** Marks a mesh as a rail, handle or other surface that hands wear down. */
	UPROPERTY(config, EditAnywhere, Category = "Tags")
	FName RailTag = TEXT("Grungify.Rail");

	/** Marks an actor or component as a runoff source (vent, drain, spout). */
	UPROPERTY(config, EditAnywhere, Category = "Tags")
	FName VentTag = TEXT("Grungify.Vent");

	/** Forces a mesh to count as a floor when tracing, regardless of size. */
	UPROPERTY(config, EditAnywhere, Category = "Tags")
	FName FloorTag = TEXT("Grungify.Floor");

	/** Zones never touch actors or components with this tag. */
	UPROPERTY(config, EditAnywhere, Category = "Tags")
	FName IgnoreTag = TEXT("Grungify.Ignore");

	/** Name tokens (from material, slot, mesh and actor names) that mean Stone. */
	UPROPERTY(config, EditAnywhere, Category = "Classification")
	TArray<FString> StoneKeywords;

	UPROPERTY(config, EditAnywhere, Category = "Classification")
	TArray<FString> MetalKeywords;

	/** Trim is checked first, so "MetalTrim" counts as trim. */
	UPROPERTY(config, EditAnywhere, Category = "Classification")
	TArray<FString> TrimKeywords;

	UPROPERTY(config, EditAnywhere, Category = "Classification")
	TArray<FString> RailKeywords;

	UPROPERTY(config, EditAnywhere, Category = "Classification")
	TArray<FString> VentKeywords;

	/** Physical surface types that map straight to a Grungify surface. Checked before name keywords. */
	UPROPERTY(config, EditAnywhere, Category = "Classification")
	TMap<TEnumAsByte<EPhysicalSurface>, EGrungifySurfaceType> PhysicalSurfaceMap;

	/** Used when nothing else identifies a surface. */
	UPROPERTY(config, EditAnywhere, Category = "Classification")
	EGrungifySurfaceType FallbackSurfaceType = EGrungifySurfaceType::Stone;

	/** A traced floor only counts if its mesh is at least this wide on its shortest horizontal side (keeps tables and shelves from acting as floors). */
	UPROPERTY(config, EditAnywhere, Category = "Floors", meta = (ClampMin = "0", Units = "cm"))
	float MinFloorSizeCm = 150.f;

	/** First custom primitive data index Grungify writes (it uses four: surface type, feature scale, seed offset, debug channel). Rebuild the plugin content after changing this. */
	UPROPERTY(config, EditAnywhere, Category = "Material", meta = (ClampMin = "0", ClampMax = "32"))
	int32 CustomPrimitiveDataIndex = 0;

	/** Material swapped onto meshes when previewing masks. */
	UPROPERTY(config, EditAnywhere, Category = "Material")
	TSoftObjectPtr<UMaterialInterface> MaskDebugMaterial;

	/** Create BP_WeatheringZone and the Grungify materials on editor startup if they are missing. */
	UPROPERTY(config, EditAnywhere, Category = "Content")
	bool bAutoBuildContent = true;
};
