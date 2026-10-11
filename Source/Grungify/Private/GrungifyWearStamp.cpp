#include "GrungifyWearStamp.h"

#include "Components/SphereComponent.h"
#include "GrungifyWeatheringZone.h"

#if WITH_EDITOR
#include "EngineUtils.h"
#include "GrungifyGenerator.h"
#endif

AGrungifyWearStamp::AGrungifyWearStamp()
{
	PrimaryActorTick.bCanEverTick = false;

	Shape = CreateDefaultSubobject<USphereComponent>(TEXT("Shape"));
	Shape->SetCollisionProfileName(TEXT("NoCollision"));
	Shape->SetGenerateOverlapEvents(false);
	Shape->SetCanEverAffectNavigation(false);
	Shape->bHiddenInGame = true;
	Shape->ShapeColor = FColor(255, 200, 40);
	RootComponent = Shape;
	SyncShape();
}

void AGrungifyWearStamp::SyncShape()
{
	if (Shape)
	{
		Shape->SetSphereRadius(RadiusCm);
	}
}

FBox AGrungifyWearStamp::GetStampBounds() const
{
	const FVector Extent = GetActorScale3D().GetAbs() * RadiusCm;
	return FBox(GetActorLocation() - Extent, GetActorLocation() + Extent);
}

float AGrungifyWearStamp::WeightAt(const FVector& WorldPoint, float FeatureScaleCm) const
{
#if WITH_EDITOR
	FGrungifyStampData Data;
	Data.Transform = GetActorTransform();
	Data.Radius = RadiusCm;
	Data.Falloff = Falloff;
	Data.EdgeNoise = EdgeNoise;
	return Grungify::StampWeight(Data, WorldPoint, FeatureScaleCm);
#else
	return 0.f;
#endif
}

#if WITH_EDITOR
void AGrungifyWearStamp::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	SyncShape();
	if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive && !GIsTransacting)
	{
		NotifyZones();
	}
}

void AGrungifyWearStamp::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (bFinished && !GIsTransacting)
	{
		NotifyZones();
	}
}

void AGrungifyWearStamp::NotifyZones()
{
	// Live-previewing zones that overlap the stamp refresh, just as if their own settings changed.
	UWorld* World = GetWorld();
	if (!World || World->WorldType != EWorldType::Editor)
	{
		return;
	}
	const FBox Bounds = GetStampBounds();
	for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
	{
		if (It->bLivePreview && It->GetZoneComponent() && It->GetZoneComponent()->Bounds.GetBox().Intersect(Bounds))
		{
			It->RequestLivePreview();
		}
	}
}
#endif
#if !WITH_EDITOR
void AGrungifyWearStamp::NotifyZones() {}
#endif
