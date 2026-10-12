// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GrungifyEditorLibrary.generated.h"

class AActor;
class AGrungifyWeatheringZone;

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
};
