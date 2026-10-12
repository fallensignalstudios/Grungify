// Copyright Fallen Signal Studios. All Rights Reserved.
// Portable bakes: Bake, reuse on a fresh compatible component without Grungify's custom data, refusal on another
// mesh, Clear and re-bake. Writes baked packages under /Game/GrungifyAutomation, so run it in the isolated HostProject.
#include "GrungifyInspection.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GrungifyBakeRecord.h"
#include "GrungifySettings.h"
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
	UStaticMeshComponent* SpawnMesh(UWorld* World, const TCHAR* MeshPath, const FVector& Location)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, MeshPath);
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
		return Component;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyPortableBakeTest, "Grungify.Bake.PortableAndCompatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyPortableBakeTest::RunTest(const FString& Parameters)
{
	if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("HostProject"))
	{
		AddError(TEXT("The bake test saves packages; run it in the isolated BuildPlugin HostProject, not a production project."));
		return false;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
	ON_SCOPE_EXIT { if (World) { World->DestroyWorld(false); } };
	TGuardValue<int32> BaseGuard(GetMutableDefault<UGrungifySettings>()->CustomPrimitiveDataIndex, 0);

	UStaticMeshComponent* Source = SpawnMesh(World, TEXT("/Engine/BasicShapes/Cube.Cube"), FVector::ZeroVector);
	if (!TestNotNull(TEXT("Source cube"), Source))
	{
		return false;
	}
	UMaterialInterface* Original = Source->GetMaterial(0);

	AGrungifyWeatheringZone* Zone = World->SpawnActor<AGrungifyWeatheringZone>();
	Zone->bLivePreview = false;
	Zone->bWriteReportFiles = false;
	Zone->bAutoDetectVents = false;
	Zone->bBackgroundGeneration = false;
	Zone->bWeatherLandscape = false;
	Zone->bExportBakedPNGs = false;
	Zone->FloorMode = EGrungifyFloorMode::ZoneBottom;
	Zone->BakeFolder.Path = TEXT("/Game/GrungifyAutomation/Baked");
	Zone->GetZoneComponent()->SetBoxExtent(FVector(500.));
	Zone->IncludeOnlyActors.Add(Source->GetOwner());

	Zone->BakeToAssets();
	UMaterialInstanceConstant* Baked = Cast<UMaterialInstanceConstant>(Source->GetMaterial(0));
	if (!Baked || !Baked->IsAsset())
	{
		// The engine cube needs a usable mask UV channel for texture output; say so instead of failing obscurely.
		AddError(FString::Printf(TEXT("Bake produced no standalone instance (status: %s)"), *Zone->Status));
		return false;
	}
	const UGrungifyBakeRecord* Record = Cast<UGrungifyBakeRecord>(Baked->GetAssetUserDataOfClass(UGrungifyBakeRecord::StaticClass()));
	if (!TestNotNull(TEXT("Baked instance carries a bake record"), Record))
	{
		return false;
	}
	TestEqual(TEXT("Record names the source mesh"), Record->SourceMesh, FSoftObjectPath(Source->GetStaticMesh()));
	float BakedMode = 0.f;
	TestTrue(TEXT("Portable fallback mode is set"), Baked->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Grungify Baked Mode")), BakedMode)
		&& BakedMode == Grungify::MaskModeTexture);

	FString Reason;
	TestTrue(TEXT("Compatible with its own component"), UGrungifyInspectionLibrary::CheckBakeCompatibility(Baked, Source, Reason));

	// A fresh component with the same mesh and empty custom data: compatible, and the instance supplies the mode.
	UStaticMeshComponent* Fresh = SpawnMesh(World, TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(2000., 0., 0.));
	if (TestNotNull(TEXT("Fresh cube"), Fresh))
	{
		TestEqual(TEXT("Fresh component has no custom data"), Fresh->GetCustomPrimitiveData().Data.Num(), 0);
		Fresh->SetMaterial(0, Baked);
		TestTrue(TEXT("Compatible with a fresh same-mesh component"), UGrungifyInspectionLibrary::CheckBakeCompatibility(Baked, Fresh, Reason));
		const FGrungifyComponentInspection Inspected = UGrungifyInspectionLibrary::InspectComponent(Fresh);
		TestTrue(TEXT("Inspection reports the baked, compatible slot"), Inspected.Slots.Num() > 0 && Inspected.Slots[0].bBakedInstance
			&& Inspected.Slots[0].BakeCompatibility == TEXT("compatible"));
		TestEqual(TEXT("Fresh component still has no mask mode (the instance's fallback applies)"), Inspected.MaskModeName, FString(TEXT("none")));
	}

	// Another mesh: refused with a reason.
	UStaticMeshComponent* Other = SpawnMesh(World, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), FVector(4000., 0., 0.));
	if (TestNotNull(TEXT("Cylinder"), Other))
	{
		TestFalse(TEXT("Incompatible with another mesh"), UGrungifyInspectionLibrary::CheckBakeCompatibility(Baked, Other, Reason));
		TestTrue(TEXT("Reason explains the UV layout"), Reason.StartsWith(TEXT("incompatible")));
	}

	// Clear restores the source; the baked asset stays. Re-baking updates the same asset in place.
	Zone->Clear();
	TestTrue(TEXT("Clear restores the original material"), Source->GetMaterial(0) == Original);
	const FString BakedPath = Baked->GetPathName();
	TestNotNull(TEXT("Baked asset survives Clear"), FindObject<UMaterialInstanceConstant>(nullptr, *BakedPath));
	Zone->BakeToAssets();
	TestTrue(TEXT("Re-bake reuses the same asset"), Source->GetMaterial(0) == Baked);
	Zone->Clear();
	return !HasAnyErrors();
}

#endif
