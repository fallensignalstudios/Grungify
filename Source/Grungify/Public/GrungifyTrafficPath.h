// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GrungifyTrafficPath.generated.h"

class USplineComponent;

/**
 * A route people (or carts) take. Zones with Scuff Intensity above zero polish the floor along it and chip
 * the bottom of walls next to it. Draw it along the floor with the spline tools.
 */
UCLASS(meta = (DisplayName = "Grungify Traffic Path"), hidecategories = (Collision, Physics, Replication, Networking, Input, Cooking, HLOD, LOD, Rendering))
class GRUNGIFY_API AGrungifyTrafficPath : public AActor
{
	GENERATED_BODY()

public:
	AGrungifyTrafficPath();

	/** How wide the worn strip is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify Traffic", meta = (ClampMin = "10", UIMax = "500", Units = "cm"))
	float WidthCm = 90.f;

	/** How heavily used the route is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grungify Traffic", meta = (ClampMin = "0", UIMax = "2"))
	float Traffic = 1.f;

	USplineComponent* GetSpline() const { return Spline; }

	/** World-space points along the spline, spaced at most StepCm apart. */
	void SamplePoints(float StepCm, TArray<FVector>& OutPoints) const;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grungify Traffic")
	TObjectPtr<USplineComponent> Spline;
};
