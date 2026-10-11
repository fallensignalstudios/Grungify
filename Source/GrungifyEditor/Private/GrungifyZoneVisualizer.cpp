#include "GrungifyZoneVisualizer.h"

#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "SceneManagement.h"

namespace
{
	const FLinearColor DustColor(0.85f, 0.7f, 0.45f);
	const FLinearColor HandColor(0.3f, 0.9f, 0.4f);
	const FLinearColor RunoffColor(0.3f, 0.55f, 1.f);

	/** Horizontal rectangle across the zone at a height above its bottom face. */
	void DrawLevel(FPrimitiveDrawInterface* PDI, const FTransform& Zone, const FVector& Extent, float HeightAboveBottom, const FLinearColor& Color)
	{
		const double Z = FMath::Min<double>(-Extent.Z + HeightAboveBottom, Extent.Z);
		const FVector Corners[4] = {
			Zone.TransformPosition(FVector(-Extent.X, -Extent.Y, Z)),
			Zone.TransformPosition(FVector(Extent.X, -Extent.Y, Z)),
			Zone.TransformPosition(FVector(Extent.X, Extent.Y, Z)),
			Zone.TransformPosition(FVector(-Extent.X, Extent.Y, Z)) };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			PDI->DrawLine(Corners[Index], Corners[(Index + 1) % 4], Color, SDPG_World, 1.5f);
		}
	}
}

void FGrungifyZoneVisualizer::DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	const UGrungifyZoneComponent* ZoneComponent = Cast<UGrungifyZoneComponent>(Component);
	const AGrungifyWeatheringZone* Zone = ZoneComponent ? Cast<AGrungifyWeatheringZone>(ZoneComponent->GetOwner()) : nullptr;
	if (!Zone)
	{
		return;
	}

	FTransform ZoneTransform = ZoneComponent->GetComponentTransform();
	ZoneTransform.SetScale3D(FVector::OneVector);
	const FVector Extent = ZoneComponent->GetScaledBoxExtent();

	// Measured from the zone bottom here; the real generation measures from traced floors.
	DrawLevel(PDI, ZoneTransform, Extent, Zone->DustHeightCm, DustColor);
	DrawLevel(PDI, ZoneTransform, Extent, Zone->HandHeightMinCm, HandColor);
	DrawLevel(PDI, ZoneTransform, Extent, Zone->HandHeightMaxCm, HandColor);

	const float Length = FMath::Max(Zone->RunoffLengthCm, 10.f);
	for (const FGrungifyRunoffSource& Source : Zone->GetCachedRunoffSources())
	{
		const FVector Bottom = Source.Origin - FVector(0.0, 0.0, Length);
		const double BottomRadius = Source.Radius * (1.0 + Zone->RunoffSpread);

		DrawCircle(PDI, Source.Origin, FVector::ForwardVector, FVector::RightVector, RunoffColor, Source.Radius, 16, SDPG_World, 1.5f);
		DrawCircle(PDI, Bottom, FVector::ForwardVector, FVector::RightVector, RunoffColor * 0.6f, BottomRadius, 16, SDPG_World, 1.f);
		for (const FVector& Side : { FVector::ForwardVector, -FVector::ForwardVector, FVector::RightVector, -FVector::RightVector })
		{
			PDI->DrawLine(Source.Origin + Side * Source.Radius, Bottom + Side * BottomRadius, RunoffColor, SDPG_World, 1.f);
		}
	}
}
