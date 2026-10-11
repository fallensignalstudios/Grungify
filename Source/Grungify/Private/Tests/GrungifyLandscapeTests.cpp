// Copyright Fallen Signal Studios. All Rights Reserved.
#include "GrungifyWeatheringZone.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GrungifyGenerator.h"
#include "GrungifySettings.h"
#include "GrungifyZoneComponent.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "Misc/AutomationTest.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyLandscapeScopeTest, "Grungify.State.LandscapeActorFiltering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyLandscapeScopeTest::RunTest(const FString& Parameters)
{
	// A transient proxy with genuine component bounds but no terrain asset/collision data.
	// This tests collection/write admission and fresh resolution, not rendered terrain masks.
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
	if (!TestNotNull(TEXT("Isolated editor world"), World)) { return false; }
	ALandscape* Landscape = World->SpawnActor<ALandscape>();
	AGrungifyWeatheringZone* Zone = World->SpawnActor<AGrungifyWeatheringZone>();
	if (!Landscape || !Zone)
	{
		AddError(TEXT("Could not create transient landscape/zone fixture"));
		World->DestroyWorld(false);
		return false;
	}
	Zone->bLivePreview = false;
	Zone->bWriteReportFiles = false;
	Zone->LandscapeContactRadiusCm = 0.f;
	Zone->GetZoneComponent()->SetBoxExtent(FVector(500.));
	UBoxComponent* Bounds = NewObject<UBoxComponent>(Landscape);
	Landscape->AddInstanceComponent(Bounds);
	Bounds->SetupAttachment(Landscape->GetRootComponent());
	Bounds->SetBoxExtent(FVector(200.));
	Bounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Bounds->RegisterComponent();
	FGrungifyGenerationJob Job;
	AActor* Other = World->SpawnActor<AActor>();
	Zone->IncludeOnlyActors.Add(Other);
	TestTrue(TEXT("Generate filtered terrain"), Zone->GenerateLandscapes(Job, TFunction<bool()>()));
	TestEqual(TEXT("Include-only mesh cannot collect an unrelated terrain proxy"), Zone->PendingLandscapes.Num(), 0);

	Zone->IncludeOnlyActors.Reset();
	Zone->IncludeOnlyActors.Add(Landscape);
	TestTrue(TEXT("Generate admitted bounded proxy"), Zone->GenerateLandscapes(Job, TFunction<bool()>()));
	if (TestEqual(TEXT("Positive admission reaches the landscape collector"), Zone->PendingLandscapes.Num(), 1))
	{
		const TSharedPtr<FGrungifyLandscapeResult> Previous = Zone->PendingLandscapes[0];
		Zone->LandscapeTexelSizeCm = 100.f;
		TestTrue(TEXT("Regenerate after texel-size edit with mesh cache enabled"), Zone->GenerateLandscapes(Job, TFunction<bool()>()));
		TestTrue(TEXT("Terrain result is recomputed at the new dimensions"), Zone->PendingLandscapes.Num() == 1
			&& Zone->PendingLandscapes[0] != Previous
			&& Zone->PendingLandscapes[0]->SizeX < Previous->SizeX);
	}
	Zone->ExcludedActors.Add(Landscape);
	TestTrue(TEXT("Generate excluded terrain"), Zone->GenerateLandscapes(Job, TFunction<bool()>()));
	TestEqual(TEXT("Explicit exclusion overrides inclusion"), Zone->PendingLandscapes.Num(), 0);
	Zone->ExcludedActors.Reset();
	Zone->IncludeDataLayers.Add(NewObject<UDataLayerAsset>());
	TestTrue(TEXT("Generate data-layer-filtered terrain"), Zone->GenerateLandscapes(Job, TFunction<bool()>()));
	TestEqual(TEXT("Proxy outside requested data layer is not collected"), Zone->PendingLandscapes.Num(), 0);
	Zone->IncludeDataLayers.Reset();
	Landscape->Tags.Add(UGrungifySettings::Get()->IgnoreTag);
	TestTrue(TEXT("Generate ignored terrain"), Zone->GenerateLandscapes(Job, TFunction<bool()>()));
	TestEqual(TEXT("Actor ignore tag prevents collection"), Zone->PendingLandscapes.Num(), 0);
	Landscape->Tags.Reset();

	// Write must recheck admission and component tags before creating textures or overrides.
	ULandscapeComponent* Component = NewObject<ULandscapeComponent>(Landscape);
	Component->Bounds = Bounds->Bounds;
	Landscape->LandscapeComponents.Add(Component);
	Component->ComponentTags.Add(UGrungifySettings::Get()->IgnoreTag);
	TSharedPtr<FGrungifyLandscapeResult> Pending = MakeShared<FGrungifyLandscapeResult>();
	Pending->Proxy = Landscape;
	Pending->SizeX = Pending->SizeY = 1;
	Pending->Pixels.Add(FColor::White);
	Zone->PendingLandscapes.Add(Pending);
	TArray<FGrungifyLandscapeRecord> Records;
	Zone->WriteLandscapes(Records);
	TestEqual(TEXT("Component ignore tag prevents override ownership"), Records.Num(), 0);
	TestNull(TEXT("Ignored component raw override remains absent"), Component->OverrideMaterial.Get());
	Zone->IncludeOnlyActors.Reset();
	Zone->IncludeOnlyActors.Add(Other);
	Component->ComponentTags.Reset();
	Zone->WriteLandscapes(Records);
	TestEqual(TEXT("Changed actor admission is rechecked before write"), Records.Num(), 0);
	TestNull(TEXT("Rejected actor raw override remains absent"), Component->OverrideMaterial.Get());
	World->DestroyWorld(false);
	return !HasAnyErrors();
}

#endif
