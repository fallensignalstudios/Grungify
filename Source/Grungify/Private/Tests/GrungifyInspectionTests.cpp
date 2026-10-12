// Copyright Fallen Signal Studios. All Rights Reserved.
// Inspection must report Grungify's state exactly and never change it. Isolated transient editor worlds only.
#include "GrungifyInspection.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GrungifyGenerator.h"
#include "GrungifySettings.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "Misc/AutomationTest.h"

namespace
{
	struct FInspectionFixture
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
		UStaticMeshComponent* Component = nullptr;

		FInspectionFixture()
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (World && Mesh)
			{
				AActor* Actor = World->SpawnActor<AActor>();
				Component = NewObject<UStaticMeshComponent>(Actor);
				Actor->AddInstanceComponent(Component);
				Actor->SetRootComponent(Component);
				Component->SetMobility(EComponentMobility::Static);
				Component->SetStaticMesh(Mesh);
				Component->RegisterComponent();
			}
		}

		~FInspectionFixture()
		{
			if (World)
			{
				World->DestroyWorld(false);
			}
		}

		AGrungifyWeatheringZone* Zone()
		{
			AGrungifyWeatheringZone* Result = World->SpawnActor<AGrungifyWeatheringZone>();
			Result->bLivePreview = false;
			Result->bWriteReportFiles = false;
			Result->bAutoDetectVents = false;
			Result->bBackgroundGeneration = false;
			Result->bWeatherLandscape = false;
			Result->FloorMode = EGrungifyFloorMode::ZoneBottom;
			Result->MaskOutput = EGrungifyMaskOutput::VertexColor;
			Result->GetZoneComponent()->SetBoxExtent(FVector(500.));
			Result->IncludeOnlyActors.Add(Component->GetOwner());
			return Result;
		}
	};

	/** Everything inspection could conceivably disturb. */
	bool SameState(const UStaticMeshComponent* Component, const FGrungifyComponentRecord& Before)
	{
		return Component->GetDefaultCustomPrimitiveData().Data == Before.CustomData
			&& Component->GetCustomPrimitiveData().Data == Before.LiveCustomData
			&& Component->OverrideMaterials == Before.OverrideMaterials
			&& Component->LODData.Num() == Before.LODDataCount;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyInspectionTest, "Grungify.Inspection.ReadOnlyComponentState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyInspectionTest::RunTest(const FString& Parameters)
{
	FInspectionFixture Fixture;
	if (!TestNotNull(TEXT("Cube fixture"), Fixture.Component))
	{
		return false;
	}
	TGuardValue<int32> BaseGuard(GetMutableDefault<UGrungifySettings>()->CustomPrimitiveDataIndex, 0);

	// Untouched.
	FGrungifyComponentInspection Clean = UGrungifyInspectionLibrary::InspectComponent(Fixture.Component);
	TestTrue(TEXT("Valid"), Clean.bValid);
	TestEqual(TEXT("Untouched state"), Clean.State, FString(TEXT("none")));
	TestEqual(TEXT("Untouched mode"), Clean.MaskModeName, FString(TEXT("none")));
	TestTrue(TEXT("Transient fixture is reported as transient"), Clean.bTransient);
	TestEqual(TEXT("One slot inspected"), Clean.Slots.Num(), Fixture.Component->GetNumMaterials());

	// Applied.
	AGrungifyWeatheringZone* Zone = Fixture.Zone();
	Zone->Apply();
	if (!TestNotNull(TEXT("Zone applied"), Zone->FindAppliedRecord(Fixture.Component)))
	{
		return false;
	}
	FGrungifyComponentRecord Before;
	Grungify::CaptureRecord(Fixture.Component, Before);
	const FGrungifyComponentInspection Applied = UGrungifyInspectionLibrary::InspectComponent(Fixture.Component);
	TestTrue(TEXT("Inspection changed nothing"), SameState(Fixture.Component, Before));
	TestEqual(TEXT("Applied state"), Applied.State, FString(TEXT("applied")));
	TestEqual(TEXT("Applied by this zone"), Applied.AppliedBy, FSoftObjectPath(Zone).ToString());
	TestEqual(TEXT("Vertex mode"), Applied.MaskModeName, FString(TEXT("vertex")));
	TestTrue(TEXT("Live CPD reported separately"), Applied.LiveCustomData == Fixture.Component->GetCustomPrimitiveData().Data
		&& Applied.DefaultCustomData == Fixture.Component->GetDefaultCustomPrimitiveData().Data);
	TestTrue(TEXT("LOD0 has override colours"), Applied.OverrideVertexColorCounts.Num() > 0 && Applied.OverrideVertexColorCounts[0] > 0);
	if (TestTrue(TEXT("Slot inspected"), Applied.Slots.Num() > 0))
	{
		const FGrungifySlotInspection& Slot = Applied.Slots[0];
		TestTrue(TEXT("Owned instance"), Slot.bOwnedInstance);
		TestTrue(TEXT("Owned instance resolves by path"), UGrungifyInspectionLibrary::ResolveGrungifyObject(Slot.Material) == Fixture.Component->GetMaterial(0));
		TestTrue(TEXT("Active is on"), Slot.bHasActiveParameter && Slot.Active == 1.f);
		TestEqual(TEXT("Vertex output binds no mask texture"), Slot.MaskTexture, FString());
	}

	// The zone view lists the same component.
	const TArray<FGrungifyComponentInspection> ZoneView = UGrungifyInspectionLibrary::InspectZone(Zone);
	TestTrue(TEXT("Zone inspection lists the applied component"), ZoneView.Num() == 1 && ZoneView[0].Component == Applied.Component);

	// Previewed by a second zone is refused (another zone owns it), so the state stays applied.
	AGrungifyWeatheringZone* Other = Fixture.Zone();
	Other->Preview();
	TestEqual(TEXT("Another zone cannot preview an applied mesh"),
		UGrungifyInspectionLibrary::InspectComponent(Fixture.Component).State, FString(TEXT("applied")));
	Other->CancelPreview();

	// Cleared, then previewed.
	Zone->Clear();
	TestEqual(TEXT("Cleared state"), UGrungifyInspectionLibrary::InspectComponent(Fixture.Component).State, FString(TEXT("none")));
	Zone->Preview();
	const FGrungifyComponentInspection Previewed = UGrungifyInspectionLibrary::InspectComponent(Fixture.Component);
	TestEqual(TEXT("Previewed state"), Previewed.State, FString(TEXT("previewed")));
	TestEqual(TEXT("Previewed by this zone"), Previewed.PreviewedBy, FSoftObjectPath(Zone).ToString());
	Zone->CancelPreview();

	// Unresolvable paths never load anything.
	TestNull(TEXT("Unknown path resolves to null"), UGrungifyInspectionLibrary::ResolveGrungifyObject(TEXT("/Game/DoesNotExist.DoesNotExist:Nothing")));
	TestEqual(TEXT("Null component is reported, not crashed on"), UGrungifyInspectionLibrary::InspectComponent(nullptr).bValid, false);
	return true;
}

#endif
