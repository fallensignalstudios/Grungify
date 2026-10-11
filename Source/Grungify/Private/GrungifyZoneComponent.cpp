// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyZoneComponent.h"

#include "Engine/CollisionProfile.h"
#include "GrungifyWeatheringZone.h"

UGrungifyZoneComponent::UGrungifyZoneComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InitBoxExtent(FVector(400.f, 400.f, 200.f));
	ShapeColor = FColor(255, 140, 40);
	LineThickness = 2.f;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	bHiddenInGame = true;
}

#if WITH_EDITOR
void UGrungifyZoneComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (AGrungifyWeatheringZone* Zone = Cast<AGrungifyWeatheringZone>(GetOwner()))
	{
		Zone->NotifyZoneShapeChanged(PropertyChangedEvent);
	}
}
#endif
