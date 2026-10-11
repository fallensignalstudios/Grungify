#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GrungifyMaterialHookup.generated.h"

class UMaterial;
class UMaterialInterface;

/** What happened to one material. */
USTRUCT(BlueprintType)
struct FGrungifyHookupResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Material;

	/** "added", "removed", "already", "skipped" or "failed". */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Outcome;

	/** "legacy", "attributes" or "substrate" when the material was changed. */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Path;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Notes;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	FString Backup;
};

/**
 * Wires Grungify into existing master materials, so nobody has to edit material graphs by hand:
 *  - legacy materials: Base Color, Roughness, Metallic and Normal are routed through MF_GrungifyWeathering;
 *  - "Use Material Attributes" materials: Get/Set Material Attributes nodes route the same four attributes;
 *  - Substrate materials (Front Material connected): MF_GrungifySubstrate mixes its grime slab over the material.
 * Inserted nodes are tagged for removal. Legacy dormant constant wiring/unconnected pin metadata and world-space
 * attribute normals are refused before changing the graph. Add/Remove are editor undo transactions. A backup copy
 * must be saved to /Game/Grungify/Backups before a first backed-up change; an unsaved duplicate is insufficient.
 * Material instances resolve to their parent material, which changes every instance of it, as intended.
 */
UCLASS()
class UGrungifyMaterialHookup : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Grungify|Materials", meta = (DevelopmentOnly))
	static FGrungifyHookupResult AddGrungifyToMaterial(UMaterialInterface* Material, bool bBackup = true);

	UFUNCTION(BlueprintCallable, Category = "Grungify|Materials", meta = (DevelopmentOnly))
	static FGrungifyHookupResult RemoveGrungifyFromMaterial(UMaterialInterface* Material);

	/** True if the material (or its parent) already reads Grungify's masks, by hand or through this tool. */
	UFUNCTION(BlueprintPure, Category = "Grungify|Materials", meta = (DevelopmentOnly))
	static bool UsesGrungify(const UMaterialInterface* Material);

	/** The base materials used by static meshes in the editor level (inside zones, if there are any) that don't use Grungify yet. */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Materials", meta = (DevelopmentOnly))
	static TArray<UMaterial*> FindUnhookedMaterialsInLevel();

	// Menu actions: run on the Content Browser selection or the level, with a progress bar, notification and report file.
	static void AddToSelectedAssets();
	static void RemoveFromSelectedAssets();
	static void HookUpLevel();
};
