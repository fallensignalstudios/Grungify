#pragma once

#include "CoreMinimal.h"
#include "Components/BoxComponent.h"
#include "GrungifyZoneComponent.generated.h"

/** The box that bounds a weathering zone. Editor-only visual, no collision. */
UCLASS(ClassGroup = Grungify, meta = (BlueprintSpawnableComponent))
class GRUNGIFY_API UGrungifyZoneComponent : public UBoxComponent
{
	GENERATED_BODY()

public:
	UGrungifyZoneComponent(const FObjectInitializer& ObjectInitializer);

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
