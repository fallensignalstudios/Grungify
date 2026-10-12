// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GrungifyCommandlet.generated.h"

/**
 * Runs weathering zones headless, for build machines and CI:
 *
 *   UnrealEditor-Cmd Project.uproject -run=Grungify -Map=/Game/Maps/A[,/Game/Maps/B] -Operation=Apply|Clear|DryRun|Bake
 *       [-Zones=LabelA,LabelB] [-Save] [-Report=Dir] [-unattended -nullrhi]
 *
 * Nothing is saved without -Save; Bake requires it and otherwise fails before opening a map.
 * Exit code: 0 ok, 1 warnings / conflicts / failed meshes, 2 usage error or missing map.
 */
UCLASS()
class UGrungifyCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGrungifyCommandlet();

	virtual int32 Main(const FString& Params) override;
};
