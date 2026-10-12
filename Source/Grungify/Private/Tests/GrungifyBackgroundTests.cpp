// Copyright Fallen Signal Studios. All Rights Reserved.
// Background generation reads immutable snapshots: edits during a run make it restart, deletion never crashes or
// writes, and cancellation is prompt. Isolated transient editor worlds only.
#include "GrungifyWeatheringZone.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GrungifySettings.h"
#include "GrungifyZoneComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

namespace
{
	TArray<UStaticMeshComponent*> SpawnCubes(UWorld* World, int32 Count)
	{
		TArray<UStaticMeshComponent*> Out;
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		for (int32 Index = 0; Index < Count && Mesh; ++Index)
		{
			AActor* Actor = World->SpawnActor<AActor>(FVector(150. * (Index % 6), 150. * (Index / 6), 50.), FRotator::ZeroRotator);
			UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
			Actor->AddInstanceComponent(Component);
			Actor->SetRootComponent(Component);
			Component->SetMobility(EComponentMobility::Static);
			Component->SetStaticMesh(Mesh);
			Component->RegisterComponent();
			Out.Add(Component);
		}
		return Out;
	}

	AGrungifyWeatheringZone* SpawnZone(UWorld* World)
	{
		AGrungifyWeatheringZone* Zone = World->SpawnActor<AGrungifyWeatheringZone>(FVector(400., 400., 0.), FRotator::ZeroRotator);
		Zone->bLivePreview = true;
		Zone->bBackgroundGeneration = true;
		Zone->bUseGenerationCache = false;
		Zone->bWriteReportFiles = false;
		Zone->bAutoDetectVents = false;
		Zone->bWeatherLandscape = false;
		Zone->FloorMode = EGrungifyFloorMode::ZoneBottom;
		Zone->MaskOutput = EGrungifyMaskOutput::Texture;
		Zone->MaskTexelSizeCm = 0.5f; // dense masks, so a run takes long enough to interrupt
		Zone->GetZoneComponent()->SetBoxExtent(FVector(1200.));
		return Zone;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyBackgroundSnapshotTest, "Grungify.Background.SnapshotsAndCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyBackgroundSnapshotTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
	ON_SCOPE_EXIT { if (World) { World->DestroyWorld(false); } };
	TGuardValue<int32> BaseGuard(GetMutableDefault<UGrungifySettings>()->CustomPrimitiveDataIndex, 0);
	TArray<UStaticMeshComponent*> Cubes = SpawnCubes(World, 24);
	if (!TestEqual(TEXT("Fixture cubes"), Cubes.Num(), 24))
	{
		return false;
	}
	AGrungifyWeatheringZone* Zone = SpawnZone(World);

	// 1. An undisturbed run writes its preview.
	TestTrue(TEXT("Background run starts"), Zone->BeginBackgroundPreviewForTests());
	TestEqual(TEXT("Undisturbed run writes"), Zone->FinishBackgroundPreviewForTests(), FString(TEXT("written")));
	TestTrue(TEXT("Preview written"), Zone->GetPreviewCount() > 0);
	Zone->CancelPreview();

	// 2. A mesh moved while the worker runs: the stale result is never written; the run starts again.
	TestTrue(TEXT("Second run starts"), Zone->BeginBackgroundPreviewForTests());
	Cubes[0]->SetWorldLocation(Cubes[0]->GetComponentLocation() + FVector(0., 0., 30.));
	TestEqual(TEXT("Edited input restarts the run"), Zone->FinishBackgroundPreviewForTests(), FString(TEXT("restarted")));
	TestEqual(TEXT("Nothing stale was written"), Zone->GetPreviewCount(), 0);
	TestEqual(TEXT("The restarted run writes"), Zone->FinishBackgroundPreviewForTests(), FString(TEXT("written")));
	Zone->CancelPreview();

	// 3. A mesh deleted while the worker runs: no crash, and nothing written to it.
	TestTrue(TEXT("Third run starts"), Zone->BeginBackgroundPreviewForTests());
	UStaticMeshComponent* Deleted = Cubes.Pop();
	Deleted->GetOwner()->Destroy();
	const FString Outcome = Zone->FinishBackgroundPreviewForTests();
	TestTrue(TEXT("Run after deletion completes"), Outcome == TEXT("written") || Outcome == TEXT("restarted"));
	if (Outcome == TEXT("restarted"))
	{
		Zone->FinishBackgroundPreviewForTests();
	}
	TestFalse(TEXT("Deleted mesh never previewed"), Zone->IsPreviewing(Deleted));
	Zone->CancelPreview();

	// 4. Cancellation is prompt. The bound is generous for slow CI machines; LastCancelLatencyMs has the measurement.
	TestTrue(TEXT("Fourth run starts"), Zone->BeginBackgroundPreviewForTests());
	Zone->CancelPreview();
	TestFalse(TEXT("Cancelled run is gone"), Zone->IsGeneratingInBackground());
	TestEqual(TEXT("Cancelled run wrote nothing"), Zone->GetPreviewCount(), 0);
	AddInfo(FString::Printf(TEXT("Cancel latency %.1f ms (worst %.1f ms)"), Zone->LastCancelLatencyMs, Zone->MaxCancelLatencyMs));
	TestTrue(TEXT("Cancellation stops within 1 s"), Zone->MaxCancelLatencyMs < 1000.f);
	return !HasAnyErrors();
}

#endif
