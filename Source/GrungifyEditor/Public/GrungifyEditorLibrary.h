// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GrungifyEditorLibrary.generated.h"

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
};
