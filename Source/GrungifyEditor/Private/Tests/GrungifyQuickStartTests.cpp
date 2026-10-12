// Copyright Fallen Signal Studios. All Rights Reserved.
// Weather Selected Actors: zone fit, selection-only filtering, preview, and one-step undo. Isolated editor worlds only.
#include "GrungifyQuickStart.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GrungifySettings.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

namespace
{
	AActor* SpawnCube(UWorld* World, const FVector& Location, const TCHAR* Label)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!World || !Mesh)
		{
			return nullptr;
		}
		AActor* Actor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
		Actor->AddInstanceComponent(Component);
		Actor->SetRootComponent(Component);
		Component->SetMobility(EComponentMobility::Static);
		Component->SetStaticMesh(Mesh);
		Component->RegisterComponent();
		Actor->SetActorLocation(Location);
		Actor->SetActorLabel(Label);
		return Actor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyWeatherSelectedTest, "Grungify.QuickStart.WeatherSelectedActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyWeatherSelectedTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
	ON_SCOPE_EXIT { if (World) { World->DestroyWorld(false); } };
	TGuardValue<int32> BaseGuard(GetMutableDefault<UGrungifySettings>()->CustomPrimitiveDataIndex, 0);

	AActor* A = SpawnCube(World, FVector(0., 0., 50.), TEXT("CrateA"));
	AActor* B = SpawnCube(World, FVector(300., 120., 50.), TEXT("CrateB"));
	AActor* Neighbour = SpawnCube(World, FVector(150., 60., 50.), TEXT("Neighbour"));
	if (!TestNotNull(TEXT("Fixtures"), A) || !B || !Neighbour)
	{
		return false;
	}

	// Nothing weatherable: refused with a reason, no zone.
	FString Message;
	AActor* Empty = World->SpawnActor<AActor>();
	TestNull(TEXT("A mesh-less selection is refused"), FGrungifyQuickStart::WeatherActors(World, { Empty }, true, Message));
	TestFalse(TEXT("Refusal explains itself"), Message.IsEmpty());

	// The zone box pads every side and reaches below the selection for floor tracing.
	const FBox Bounds = A->GetComponentsBoundingBox(true) + B->GetComponentsBoundingBox(true);
	const FBox Expected = FGrungifyQuickStart::ZoneBoxFor(Bounds);
	TestTrue(TEXT("Zone box contains the selection"), Expected.IsInside(Bounds.ExpandBy(-1.)));
	TestTrue(TEXT("Zone box reaches below the selection"), Expected.Min.Z < Bounds.Min.Z - 50.);

	AGrungifyWeatheringZone* Zone = FGrungifyQuickStart::WeatherActors(World, { A, B, Empty }, /*bPreview*/ true, Message);
	if (!TestNotNull(TEXT("Zone spawned"), Zone))
	{
		AddError(Message);
		return false;
	}
	const UGrungifyZoneComponent* Box = Zone->GetZoneComponent();
	TestTrue(TEXT("Zone is centred on the fitted box"), Zone->GetActorLocation().Equals(Expected.GetCenter(), 0.1));
	TestTrue(TEXT("Zone extent is the fitted box"), Box->GetScaledBoxExtent().Equals(Expected.GetExtent(), 0.1));
	TestEqual(TEXT("Only the two mesh actors are included"), Zone->IncludeOnlyActors.Num(), 2);
	TestEqual(TEXT("Label names the selection"), Zone->GetActorLabel(), FString(TEXT("Grungify_CrateA_and_1_more")));

	// Preview touched exactly the selection, not the neighbour inside the padded box.
	TArray<const UMeshComponent*> Previewed;
	for (const FGrungifyComponentRecord& Record : Zone->GetPreviewRecords())
	{
		Previewed.Add(Record.Component.Get());
	}
	TestTrue(TEXT("CrateA previewed"), Previewed.Contains(A->FindComponentByClass<UStaticMeshComponent>()));
	TestTrue(TEXT("CrateB previewed"), Previewed.Contains(B->FindComponentByClass<UStaticMeshComponent>()));
	TestFalse(TEXT("Neighbour inside the box left alone"), Previewed.Contains(Neighbour->FindComponentByClass<UStaticMeshComponent>()));
	Zone->CancelPreview();

	// Without preview: the zone exists, nothing is touched.
	AGrungifyWeatheringZone* Quiet = FGrungifyQuickStart::WeatherActors(World, { Neighbour }, /*bPreview*/ false, Message);
	TestTrue(TEXT("No-preview zone touches nothing"), Quiet && Quiet->GetPreviewCount() == 0 && Quiet->FindAppliedRecord(Neighbour->FindComponentByClass<UStaticMeshComponent>()) == nullptr);
	return true;
}

#endif
