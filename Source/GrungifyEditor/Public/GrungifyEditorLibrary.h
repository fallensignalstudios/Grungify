// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GrungifyTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GrungifyEditorLibrary.generated.h"

class AActor;
class AGrungifyWeatheringZone;
class UGrungifyPreset;
class UMeshComponent;
class UTexture2D;

/** A local trial on chosen material slots of one component, without touching shared assets. */
USTRUCT(BlueprintType)
struct GRUNGIFYEDITOR_API FGrungifyLocalWeatherOptions
{
	GENERATED_BODY()

	/** Material slot indices to weather. Empty = every slot. All other slots are left untouched. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	TArray<int32> Slots;

	/**
	 * Clone each chosen slot's master material (and its material-instance chain) into Private Folder, hook Grungify into
	 * the clone, and assign the clone to only these slots. The shared master and every other user of it stay unchanged.
	 * Off: the slots keep their materials, which must already use Grungify for the masks to show.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	bool bPrivateMaster = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify", meta = (EditCondition = "bPrivateMaster"))
	FString PrivateFolder = TEXT("/Game/Grungify/Private");

	/** Artist permission gate: wear is allowed only where this texture's channel is white (painted on the mesh's UVs). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	TObjectPtr<UTexture2D> PermissionGate;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	EGrungifyChannel GateChannel = EGrungifyChannel::R;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	int32 GateUVChannel = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	bool bInvertGate = false;

	/** Look to load into the zone (layer settings, palette); null = the zone's defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	TObjectPtr<UGrungifyPreset> Preset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify")
	bool bPreview = true;
};

/** What a local trial actually did, counted from outcomes rather than intentions. */
USTRUCT(BlueprintType)
struct GRUNGIFYEDITOR_API FGrungifyLocalWeatherResult
{
	GENERATED_BODY()

	/** False when nothing was set up; Notes say why. */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	bool bSucceeded = false;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TObjectPtr<AGrungifyWeatheringZone> Zone;

	/** Asset paths created (private material clones). Not removed by Undo: delete them if the trial is discarded. */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Created;

	/** Slots now using a private clone. */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<int32> PrivateSlots;

	/** Background generations started instead of finishing synchronously (0 for explicit Preview). */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	int32 Queued = 0;

	/** From the zone's report: components written, left alone, and attempted but not written. */
	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	int32 Weathered = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	int32 Skipped = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	int32 Failed = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Grungify")
	TArray<FString> Notes;
};

/** Editor-only entry points for the same explicit authoring actions offered by the Grungify menu. */
UCLASS()
class GRUNGIFYEDITOR_API UGrungifyEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Builds and saves the fixed reference map in /Game/Grungify/Reference. Replaces the current editor world. */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Editor")
	static bool BuildReferenceScene(FString& OutMessage);

	/** Builds the tuned material and preset library in /Game/Grungify/Library. */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Editor")
	static bool BuildMaterialLibrary(bool bOverwrite, FString& OutMessage);

	/**
	 * Weather Selected Actors without the dialogs: spawns a zone fitted to Actors (and limited to them) in the editor
	 * world and previews it when bPreview is set. Doesn't hook up materials (use UGrungifyMaterialHookup for that).
	 * Null, with the reason, when none of the actors has a static or skeletal mesh.
	 */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Editor")
	static AGrungifyWeatheringZone* WeatherActors(const TArray<AActor*>& Actors, bool bPreview, FString& OutMessage);

	/**
	 * A local trial: weather only Options.Slots of Component, by default on private clones of their masters (shared
	 * materials untouched), behind an optional permission-gate texture, with a zone scoped to exactly those slots.
	 * One undo step for the level changes (slot assignments, zone, masks); created clones are listed for cleanup.
	 */
	UFUNCTION(BlueprintCallable, Category = "Grungify|Editor")
	static FGrungifyLocalWeatherResult WeatherComponentSlots(UMeshComponent* Component, const FGrungifyLocalWeatherOptions& Options);
};
