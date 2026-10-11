// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyTrafficPath.h"

#include "Components/SplineComponent.h"

AGrungifyTrafficPath::AGrungifyTrafficPath()
{
	PrimaryActorTick.bCanEverTick = false;
	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Path"));
	Spline->SetClosedLoop(false);
	Spline->bDrawDebug = true;
	RootComponent = Spline;
}

void AGrungifyTrafficPath::SamplePoints(float StepCm, TArray<FVector>& OutPoints) const
{
	OutPoints.Reset();
	if (!Spline)
	{
		return;
	}
	const float Length = Spline->GetSplineLength();
	const int32 Steps = FMath::Max(1, FMath::CeilToInt(Length / FMath::Max(StepCm, 1.f)));
	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		OutPoints.Add(Spline->GetLocationAtDistanceAlongSpline(Length * Step / Steps, ESplineCoordinateSpace::World));
	}
}
