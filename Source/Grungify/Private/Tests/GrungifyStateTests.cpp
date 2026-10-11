// Copyright Fallen Signal Studios. All Rights Reserved.
// Regression coverage uses isolated transient editor worlds, never the editor's current map.
#include "GrungifyGenerator.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GrungifySettings.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"

namespace
{
	struct FGrungifyTestWorld
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
		UStaticMeshComponent* Component = nullptr;
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

		FGrungifyTestWorld()
		{
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

		~FGrungifyTestWorld()
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
			Result->FloorMode = EGrungifyFloorMode::ZoneBottom;
			Result->MaskOutput = EGrungifyMaskOutput::VertexColor;
			Result->GetZoneComponent()->SetBoxExtent(FVector(500.));
			Result->IncludeOnlyActors.Add(Component->GetOwner());
			return Result;
		}
	};

	TArray<TArray<FColor>> ColorsFor(UStaticMesh* Mesh, FColor Value)
	{
		TArray<TArray<FColor>> Result;
		if (const FStaticMeshRenderData* Data = Mesh->GetRenderData())
		{
			Result.SetNum(Data->LODResources.Num());
			for (int32 I = 0; I < Result.Num(); ++I)
			{
				Result[I].Init(Value, Data->LODResources[I].GetNumVertices());
			}
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyExactCPDTest, "Grungify.State.ExactCPDRestoration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyExactCPDTest::RunTest(const FString& Parameters)
{
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>();
	UGrungifySettings* Settings = GetMutableDefault<UGrungifySettings>();
	TGuardValue<int32> BaseGuard(Settings->CustomPrimitiveDataIndex, 0);
	const float Values[Grungify::CPD_Count] = { Grungify::MaskModeVertex, 0.f, 25.f, 12.f, 0.f };
	FGrungifyComponentRecord Empty;
	Grungify::CaptureRecord(Component, Empty);
	TestTrue(TEXT("Write empty arrays"), Grungify::WriteCustomData(Component, Values));
	Settings->CustomPrimitiveDataIndex = 8;
	TestTrue(TEXT("Restore after changing base"), Grungify::RestoreRecord(Empty, false));
	TestTrue(TEXT("Both arrays return to exact empty shape"), Component->GetDefaultCustomPrimitiveData().Data.IsEmpty()
		&& Component->GetCustomPrimitiveData().Data.IsEmpty());
	TestEqual(TEXT("Capture remembers original base"), Empty.CustomDataBase, 0);

	Component->SetDefaultCustomPrimitiveDataFloat(12, .2f);
	Component->SetCustomPrimitiveDataFloat(14, .37f);
	FGrungifyComponentRecord Original;
	Grungify::CaptureRecord(Component, Original);
	Settings->CustomPrimitiveDataIndex = 0;
	TestTrue(TEXT("Write mixed saved/live arrays"), Grungify::WriteCustomData(Component, Values));
	TestEqual(TEXT("Unrelated live-only value survives write"), Component->GetCustomPrimitiveData().Data[14], .37f);
	Settings->CustomPrimitiveDataIndex = 8;
	TestTrue(TEXT("Restore mixed arrays after changing base"), Grungify::RestoreRecord(Original, false));
	TestTrue(TEXT("Defaults restored exactly"), Component->GetDefaultCustomPrimitiveData().Data == Original.CustomData);
	TestTrue(TEXT("Live array restored exactly"), Component->GetCustomPrimitiveData().Data == Original.LiveCustomData);

	Component->SetCustomPrimitiveDataFloat(2, .5f);
	TestFalse(TEXT("Live-only reservation is occupied"), Grungify::CustomDataSlotsFree(
		Component->GetDefaultCustomPrimitiveData().Data, Component->GetCustomPrimitiveData().Data, 0));
	TestFalse(TEXT("Out of range reservation rejected"), Grungify::CustomDataSlotsFree({}, {}, FCustomPrimitiveData::NumCustomPrimitiveDataFloats - 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyPaintAndMaterialsTest, "Grungify.State.PaintCacheAndMaterialArrays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyPaintAndMaterialsTest::RunTest(const FString& Parameters)
{
	FGrungifyTestWorld Fixture;
	if (!TestNotNull(TEXT("Cube fixture"), Fixture.Component) || !TestNotNull(TEXT("Cube render data"), Fixture.Mesh->GetRenderData()))
	{
		return false;
	}
	const TArray<TArray<FColor>> Red = ColorsFor(Fixture.Mesh, FColor::Red);
	const TArray<TArray<FColor>> Blue = ColorsFor(Fixture.Mesh, FColor::Blue);
	TestTrue(TEXT("Original painted colors"), Grungify::WriteVertexColors(Fixture.Component, Red));
	FGrungifyComponentRecord Original;
	Grungify::CaptureRecord(Fixture.Component, Original);
	TestTrue(TEXT("Preexisting paint cache populated"), Original.LODs[0].PaintedVertices.Num() > 1);
	TestTrue(TEXT("New painted colors"), Grungify::WriteVertexColors(Fixture.Component, Blue));
	TestTrue(TEXT("New cache uses new colors"), Fixture.Component->LODData[0].PaintedVertices.ContainsByPredicate(
		[](const FPaintedVertex& V) { return V.Color == FColor::Blue; }));
	TestFalse(TEXT("Old colors absent from new remap cache"), Fixture.Component->LODData[0].PaintedVertices.ContainsByPredicate(
		[](const FPaintedVertex& V) { return V.Color == FColor::Red; }));
	TestTrue(TEXT("Restore original paint"), Grungify::RestoreRecord(Original, false));
	FGrungifyComponentRecord Restored;
	Grungify::CaptureRecord(Fixture.Component, Restored);
	TestTrue(TEXT("Override buffer restored"), Restored.LODs[0].Colors == Original.LODs[0].Colors);
	TestEqual(TEXT("LOD count restored"), Restored.LODDataCount, Original.LODDataCount);
	TestEqual(TEXT("DDC key restored"), Restored.StaticMeshDerivedDataKey, Original.StaticMeshDerivedDataKey);
	TestEqual(TEXT("Paint cache count restored"), Restored.LODs[0].PaintedVertices.Num(), Original.LODs[0].PaintedVertices.Num());
	for (int32 I = 0; I < Original.LODs[0].PaintedVertices.Num(); ++I)
	{
		TestTrue(TEXT("Painted vertex restored"), Restored.LODs[0].PaintedVertices[I].Position == Original.LODs[0].PaintedVertices[I].Position
			&& Restored.LODs[0].PaintedVertices[I].Normal == Original.LODs[0].PaintedVertices[I].Normal
			&& Restored.LODs[0].PaintedVertices[I].Color == Original.LODs[0].PaintedVertices[I].Color);
	}

	for (bool bShortArray : { false, true })
	{
		Fixture.Component->OverrideMaterials.Reset();
		if (bShortArray)
		{
			Fixture.Component->OverrideMaterials.Add(nullptr);
		}
		FGrungifyComponentRecord Before;
		Grungify::CaptureRecord(Fixture.Component, Before);
		Fixture.Component->SetMaterial(2, UMaterial::GetDefaultMaterial(MD_Surface));
		TestTrue(TEXT("Restore override material array"), Grungify::RestoreRecord(Before, false));
		TestTrue(TEXT("Empty/short/null raw array shape restored"), Fixture.Component->OverrideMaterials == Before.OverrideMaterials);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyOverlapTest, "Grungify.State.OverlapOwnershipAndOccupiedLiveData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyOverlapTest::RunTest(const FString& Parameters)
{
	FGrungifyTestWorld Fixture;
	if (!TestNotNull(TEXT("Cube fixture"), Fixture.Component))
	{
		return false;
	}
	TGuardValue<int32> BaseGuard(GetMutableDefault<UGrungifySettings>()->CustomPrimitiveDataIndex, 0);
	FGrungifyComponentRecord Before;
	Grungify::CaptureRecord(Fixture.Component, Before);
	AGrungifyWeatheringZone* A = Fixture.Zone();
	AGrungifyWeatheringZone* B = Fixture.Zone();
	B->OverlapPolicy = EGrungifyOverlapPolicy::TakeOver;
	A->Apply();
	TestNotNull(TEXT("A owns applied component"), A->FindAppliedRecord(Fixture.Component));
	FGrungifyComponentRecord Applied;
	Grungify::CaptureRecord(Fixture.Component, Applied);
	B->Preview();
	TestFalse(TEXT("B cannot preview another applied owner"), B->IsPreviewing(Fixture.Component));
	A->Clear();
	B->CancelPreview();
	TestTrue(TEXT("Cancel cannot resurrect cleared owner"), Fixture.Component->GetDefaultCustomPrimitiveData().Data == Before.CustomData
		&& Fixture.Component->GetCustomPrimitiveData().Data == Before.LiveCustomData
		&& Fixture.Component->OverrideMaterials == Before.OverrideMaterials);
	A->Apply();
	B->Preview();
	A->Apply();
	B->CancelPreview();
	TestNotNull(TEXT("A keeps ownership during blocked B preview"), A->FindAppliedRecord(Fixture.Component));
	B->Apply();
	TestNull(TEXT("Apply transfers ownership from A"), A->FindAppliedRecord(Fixture.Component));
	TestNotNull(TEXT("B receives original backup"), B->FindAppliedRecord(Fixture.Component));
	B->Clear();
	TestTrue(TEXT("Takeover Clear restores original arrays"), Fixture.Component->GetDefaultCustomPrimitiveData().Data == Before.CustomData
		&& Fixture.Component->GetCustomPrimitiveData().Data == Before.LiveCustomData
		&& Fixture.Component->OverrideMaterials == Before.OverrideMaterials);

	Fixture.Component->SetCustomPrimitiveDataFloat(0, .5f);
	FGrungifyComponentRecord Occupied;
	Grungify::CaptureRecord(Fixture.Component, Occupied);
	A->Apply();
	TestNull(TEXT("Live-only occupied slot gives no ownership"), A->FindAppliedRecord(Fixture.Component));
	TestTrue(TEXT("Conflict leaves full state untouched"), Fixture.Component->GetCustomPrimitiveData().Data == Occupied.LiveCustomData
		&& Fixture.Component->GetDefaultCustomPrimitiveData().Data == Occupied.CustomData
		&& Fixture.Component->OverrideMaterials == Occupied.OverrideMaterials);
	TestTrue(TEXT("Conflict is reported"), A->GetLastReport().Components.ContainsByPredicate(
		[](const FGrungifyComponentReport& Entry) { return Entry.bCustomDataConflict; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyMaterialOwnershipTest, "Grungify.State.AdmittedMaterialOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyMaterialOwnershipTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>();
	UMaterialInterface* Original = UMaterial::GetDefaultMaterial(MD_Surface);
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Original));
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Original));
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>();
	Component->SetStaticMesh(Mesh);
	FGrungifyComponentRecord Before;
	Grungify::CaptureRecord(Component, Before);
	const TArray<EGrungifySurfaceType> Surfaces = { EGrungifySurfaceType::Stone, EGrungifySurfaceType::Auto };
	TestTrue(TEXT("Own admitted vertex slot"), Grungify::BindOwnedMaterials(Component, nullptr, Surfaces, nullptr));
	UMaterialInstanceConstant* Owned = Cast<UMaterialInstanceConstant>(Component->GetMaterial(0));
	TestNotNull(TEXT("Admitted slot has owned MIC"), Owned);
	TestTrue(TEXT("MIC belongs to component"), Owned && Owned->GetOuter() == Component);
	TestTrue(TEXT("Excluded effective material unchanged"), Component->GetMaterial(1) == Original);
	TestEqual(TEXT("Excluded raw slot remains absent"), Component->OverrideMaterials.Num(), 1);
	if (Owned)
	{
		float Active = 0.f;
		float Surface = -1.f;
		TestTrue(TEXT("Owned activation parameter"), Owned->GetScalarParameterValue(FMaterialParameterInfo(Grungify::ActiveParameterName()), Active));
		TestEqual(TEXT("Owned activation enabled"), Active, 1.f);
		TestTrue(TEXT("Per-slot surface parameter"), Owned->GetScalarParameterValue(FMaterialParameterInfo(Grungify::SurfaceOverrideParameterName()), Surface));
		TestEqual(TEXT("Stone slot uses stone family"), Surface, 0.f);
	}
	TestTrue(TEXT("Clear raw materials"), Grungify::RestoreRecord(Before, false));
	TestTrue(TEXT("Original raw array shape"), Component->OverrideMaterials == Before.OverrideMaterials);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyLevelRemovalTest, "Grungify.State.EditorLevelRemovalRestoresPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyLevelRemovalTest::RunTest(const FString& Parameters)
{
	FGrungifyTestWorld Fixture;
	if (!TestNotNull(TEXT("Cube fixture"), Fixture.Component))
	{
		return false;
	}
	TGuardValue<int32> BaseGuard(GetMutableDefault<UGrungifySettings>()->CustomPrimitiveDataIndex, 0);
	FGrungifyComponentRecord Before;
	Grungify::CaptureRecord(Fixture.Component, Before);
	AGrungifyWeatheringZone* Zone = Fixture.Zone();
	Zone->Preview();
	TestTrue(TEXT("Preview active before level event"), Zone->IsPreviewing(Fixture.Component));
	Fixture.World->PersistentLevel->OnLoadedActorRemovedFromLevelEvent.Broadcast(*Zone);
	TestFalse(TEXT("Real editor module level event retires preview"), Zone->bPreviewActive);
	TestTrue(TEXT("Level removal restores both arrays/materials"), Fixture.Component->GetDefaultCustomPrimitiveData().Data == Before.CustomData
		&& Fixture.Component->GetCustomPrimitiveData().Data == Before.LiveCustomData
		&& Fixture.Component->OverrideMaterials == Before.OverrideMaterials);
	return true;
}

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
