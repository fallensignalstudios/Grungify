// Copyright Fallen Signal Studios. All Rights Reserved.
// Weather Selected Actors: zone fit, selection-only filtering, preview, and one-step undo. Isolated editor worlds only.
#include "GrungifyQuickStart.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "GrungifyEditorLibrary.h"
#include "GrungifyMaterialHookup.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/Package.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GrungifySettings.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"

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

	// Include every nearby fixture: false means neither writes nor a queued background job.
	struct FQuietState
	{
		UStaticMeshComponent* Component;
		TArray<TObjectPtr<UMaterialInterface>> Materials;
		TArray<float> Defaults;
		TArray<float> Live;
	};
	TArray<FQuietState> BeforeQuiet;
	for (AActor* Actor : { A, B, Neighbour })
	{
		UStaticMeshComponent* Component = Actor->FindComponentByClass<UStaticMeshComponent>();
		BeforeQuiet.Add({ Component, Component->OverrideMaterials, Component->GetDefaultCustomPrimitiveData().Data, Component->GetCustomPrimitiveData().Data });
	}

	// Without preview: the zone exists, nothing is touched.
	AGrungifyWeatheringZone* Quiet = FGrungifyQuickStart::WeatherActors(World, { Neighbour }, /*bPreview*/ false, Message);
	TestTrue(TEXT("No-preview zone touches nothing"), Quiet && Quiet->GetPreviewCount() == 0 && Quiet->FindAppliedRecord(Neighbour->FindComponentByClass<UStaticMeshComponent>()) == nullptr);
	if (Quiet)
	{
		TestFalse(TEXT("No-preview zone has no queued background generation"), Quiet->IsGeneratingInBackground());
		TestEqual(TEXT("Live-preview flag is restored to its creation default"), Quiet->bLivePreview, Quiet->GetClass()->GetDefaultObject<AGrungifyWeatheringZone>()->bLivePreview);
	}
	for (const FQuietState& State : BeforeQuiet)
	{
		TestTrue(TEXT("No-preview keeps every nearby raw material override array exact"), State.Component->OverrideMaterials == State.Materials);
		TestTrue(TEXT("No-preview keeps every nearby saved CPD array exact"), State.Component->GetDefaultCustomPrimitiveData().Data == State.Defaults);
		TestTrue(TEXT("No-preview keeps every nearby live CPD array exact"), State.Component->GetCustomPrimitiveData().Data == State.Live);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyLocalSlotTrialTest, "Grungify.QuickStart.LocalSlotTrial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyLocalSlotTrialTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
	ON_SCOPE_EXIT { if (World) { World->DestroyWorld(false); } };
	TGuardValue<int32> BaseGuard(GetMutableDefault<UGrungifySettings>()->CustomPrimitiveDataIndex, 0);

	// A shared master and instance in /Temp (never saved), used by the hero and by a neighbour.
	UMaterial* SharedMaster = NewObject<UMaterial>(CreatePackage(TEXT("/Temp/GrungifyAutomation/M_SharedMaster")), TEXT("M_SharedMaster"), RF_Public | RF_Standalone | RF_Transactional);
	UMaterialInstanceConstant* SharedInstance = NewObject<UMaterialInstanceConstant>(CreatePackage(TEXT("/Temp/GrungifyAutomation/MI_Shared")), TEXT("MI_Shared"), RF_Public | RF_Standalone | RF_Transactional);
	SharedInstance->SetParentEditorOnly(SharedMaster);
	SharedMaster->PostEditChange();
	SharedInstance->PostEditChange();

	AActor* Hero = SpawnCube(World, FVector(0., 0., 50.), TEXT("HeroCoffer"));
	AActor* Neighbour = SpawnCube(World, FVector(400., 0., 50.), TEXT("Neighbour"));
	if (!TestNotNull(TEXT("Fixtures"), Hero) || !Neighbour)
	{
		return false;
	}
	UStaticMeshComponent* HeroMesh = Hero->FindComponentByClass<UStaticMeshComponent>();
	UStaticMeshComponent* NeighbourMesh = Neighbour->FindComponentByClass<UStaticMeshComponent>();
	HeroMesh->SetMaterial(0, SharedInstance);
	NeighbourMesh->SetMaterial(0, SharedInstance);

	FGrungifyLocalWeatherOptions Options;
	Options.Slots = { 0 };
	Options.PrivateFolder = TEXT("/Temp/GrungifyAutomation/Private");
	const FGrungifyLocalWeatherResult Result = UGrungifyEditorLibrary::WeatherComponentSlots(HeroMesh, Options);
	if (!TestTrue(TEXT("Local trial set up"), Result.bSucceeded && Result.Zone != nullptr))
	{
		AddError(FString::Join(Result.Notes, TEXT("\n")));
		return false;
	}

	// Private: a cloned master and instance, hooked up; the shared ones and their other users untouched.
	TestEqual(TEXT("Created the private master and instance"), Result.Created.Num(), 2);
	TestTrue(TEXT("Slot 0 uses the private clone"), Result.PrivateSlots == TArray<int32>{ 0 });
	// During preview the zone binds its own instance on top; walk up to the first asset in the chain.
	const UMaterialInterface* Walk = HeroMesh->GetMaterial(0);
	while (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Walk))
	{
		if (Instance->IsAsset())
		{
			break;
		}
		Walk = Instance->Parent;
	}
	TestTrue(TEXT("The hero's slot resolves to a private instance asset"), Walk && Walk != SharedInstance && Walk->GetPathName().StartsWith(Options.PrivateFolder));
	TestTrue(TEXT("The private master uses Grungify"), Walk && UGrungifyMaterialHookup::UsesGrungify(Walk));
	TestFalse(TEXT("The shared master was not hooked up"), UGrungifyMaterialHookup::UsesGrungify(SharedMaster));
	TestEqual(TEXT("The neighbour still uses the shared instance"), NeighbourMesh->GetMaterial(0), static_cast<UMaterialInterface*>(SharedInstance));
	TestEqual(TEXT("The neighbour has no Grungify custom data"), NeighbourMesh->GetDefaultCustomPrimitiveData().Data.Num(), 0);

	// Counts are the zone report's final outcomes.
	TestEqual(TEXT("One component weathered"), Result.Weathered, 1);
	TestEqual(TEXT("Nothing failed"), Result.Failed, 0);
	TestEqual(TEXT("Explicit preview is synchronous"), Result.Queued, 0);
	TestEqual(TEXT("Counts match the report"), Result.Weathered + Result.Skipped + Result.Failed, Result.Zone->GetLastReport().Components.Num());

	// Selected-slot scope: a zone whose only rule picks slot 1 leaves slot 0 alone.
	AGrungifyWeatheringZone* Scoped = Result.Zone;
	Scoped->CancelPreview();
	Scoped->SlotRules[0].SlotIndex = 1;
	Scoped->Preview();
	TestEqual(TEXT("Slot scope: nothing outside the chosen slots is weathered"), Scoped->GetLastReport().Weathered, 0);
	const TArray<FGrungifyComponentReport>& Components = Scoped->GetLastReport().Components;
	TestTrue(TEXT("Slot scope: the reason says why"), Components.Num() == 1 && Components[0].SlotDetails.Num() > 0
		&& Components[0].SlotDetails[0].Reason.Contains(TEXT("Only Slots Matched By Rules")));
	Scoped->CancelPreview();
	Scoped->SlotRules[0].SlotIndex = 0;

	// One Undo restores the level: slot assignment, custom data, and the zone.
	if (!GEditor || !GEditor->Trans)
	{
		AddWarning(TEXT("No editor transaction buffer; Undo not verified."));
		return true;
	}
	// The first trial's preview is cancelled; the hero now uses its first private clone. Run a second trial and undo it.
	const auto OverridesBefore = HeroMesh->OverrideMaterials;
	const auto DefaultsBefore = HeroMesh->GetDefaultCustomPrimitiveData().Data;
	const auto LiveBefore = HeroMesh->GetCustomPrimitiveData().Data;
	const FGrungifyLocalWeatherResult Again = UGrungifyEditorLibrary::WeatherComponentSlots(HeroMesh, Options);
	TWeakObjectPtr<AGrungifyWeatheringZone> UndoneZone = Again.Zone;
	if (!TestTrue(TEXT("Second trial set up"), Again.bSucceeded && Again.Weathered == 1))
	{
		return false;
	}
	TestTrue(TEXT("The second trial changed the hero"), HeroMesh->OverrideMaterials != OverridesBefore
		|| HeroMesh->GetDefaultCustomPrimitiveData().Data != DefaultsBefore);
	GEditor->UndoTransaction();
	TestTrue(TEXT("Undo: the hero's raw material overrides are back exactly"), HeroMesh->OverrideMaterials == OverridesBefore);
	TestTrue(TEXT("Undo: the hero's saved custom data is back"), HeroMesh->GetDefaultCustomPrimitiveData().Data == DefaultsBefore);
	TestTrue(TEXT("Undo: the hero's live custom data is back"), HeroMesh->GetCustomPrimitiveData().Data == LiveBefore);
	TestTrue(TEXT("Undo: the second zone is gone"), !UndoneZone.IsValid() || !IsValid(UndoneZone.Get()) || UndoneZone->IsPendingKillPending());
	TestEqual(TEXT("Undo: the neighbour is untouched"), NeighbourMesh->GetMaterial(0), static_cast<UMaterialInterface*>(SharedInstance));
	return true;
}

#endif
