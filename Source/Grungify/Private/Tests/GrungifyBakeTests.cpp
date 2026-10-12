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
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "UObject/Package.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyBakeRefusesUnrelatedTest, "Grungify.Bake.RefusesUnrelatedSameNameAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyBakeRefusesUnrelatedTest::RunTest(const FString& Parameters)
{
	if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("HostProject"))
	{
		AddError(TEXT("The bake test creates packages; run it in the isolated BuildPlugin HostProject, not a production project."));
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
	Source->GetOwner()->SetActorLabel(TEXT("RefuseCube"));
	UMaterialInterface* Original = Source->GetMaterial(0);
	AGrungifyWeatheringZone* Zone = World->SpawnActor<AGrungifyWeatheringZone>();
	Zone->bLivePreview = false;
	Zone->SetActorLabel(TEXT("RefuseZone"));
	Zone->bWriteReportFiles = false;
	Zone->bAutoDetectVents = false;
	Zone->bBackgroundGeneration = false;
	Zone->bWeatherLandscape = false;
	Zone->bExportBakedPNGs = false;
	Zone->FloorMode = EGrungifyFloorMode::ZoneBottom;
	Zone->GetZoneComponent()->SetBoxExtent(FVector(500.));
	Zone->IncludeOnlyActors.Add(Source->GetOwner());

	// Learn the names a bake of this component uses, in a probe folder, then put everything back.
	Zone->BakeFolder.Path = TEXT("/Game/GrungifyAutomation/RefuseProbe");
	Zone->BakeToAssets();
	TArray<FString> Names;
	for (const FString& PackageName : Zone->GetLastReport().BakedAssets)
	{
		Names.Add(FPackageName::GetShortName(PackageName));
	}
	Zone->Clear();
	const FString* TextureName = Names.FindByPredicate([](const FString& Name) { return Name.StartsWith(TEXT("T_Grungify_")) && !Name.EndsWith(TEXT("_Ext")); });
	const FString* InstanceName = Names.FindByPredicate([](const FString& Name) { return Name.StartsWith(TEXT("MI_Grungify_")); });
	if (!TestNotNull(TEXT("Probe bake produced a texture"), TextureName) || !TestNotNull(TEXT("Probe bake produced an instance"), InstanceName))
	{
		AddError(FString::Printf(TEXT("Probe bake status: %s"), *Zone->Status));
		return false;
	}

	auto MakeUnrelatedTexture = [](const FString& Folder, const FString& Name)
	{
		UTexture2D* Texture = NewObject<UTexture2D>(CreatePackage(*(Folder / Name)), *Name, RF_Public | RF_Standalone);
		TArray<FColor> Pixels;
		for (int32 Index = 0; Index < 16; ++Index)
		{
			Pixels.Add(FColor(uint8(Index * 13), uint8(255 - Index), 7, 200));
		}
		Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		FAssetRegistryModule::AssetCreated(Texture);
		return Texture;
	};
	auto MakeUnrelatedInstance = [](const FString& Folder, const FString& Name)
	{
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(CreatePackage(*(Folder / Name)), *Name, RF_Public | RF_Standalone);
		Instance->SetParentEditorOnly(UMaterial::GetDefaultMaterial(MD_Surface));
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Artist Value")), 3.f);
		FAssetRegistryModule::AssetCreated(Instance);
		return Instance;
	};
	auto Bytes = [](UTexture2D* Texture)
	{
		TArray64<uint8> Data;
		Texture->Source.GetMipData(Data, 0);
		return Data;
	};
	auto Refused = [&](const TCHAR* What)
	{
		const TArray<FGrungifyComponentReport>& Components = Zone->GetLastReport().Components;
		const bool bRefused = Components.Num() == 1 && Components[0].Result == TEXT("failed") && Components[0].Notes.Contains(TEXT("bake refused"));
		TestTrue(FString::Printf(TEXT("%s: the component is reported failed with the refusal"), What), bRefused);
		TestEqual(FString::Printf(TEXT("%s: counted as failed, not skipped"), What), Zone->GetLastReport().Failed, 1);
		TestTrue(FString::Printf(TEXT("%s: the component keeps its material"), What), Source->GetMaterial(0) == Original);
	};

	// 1. An artist's texture and instance already sit at both names.
	{
		const FString Folder = TEXT("/Game/GrungifyAutomation/RefuseBoth");
		UTexture2D* Texture = MakeUnrelatedTexture(Folder, *TextureName);
		UMaterialInstanceConstant* Instance = MakeUnrelatedInstance(Folder, *InstanceName);
		const TArray64<uint8> Before = Bytes(Texture);
		Zone->BakeFolder.Path = Folder;
		Zone->BakeToAssets();
		Refused(TEXT("Both taken"));
		TestTrue(TEXT("Both taken: the texture's bytes are unchanged"), Bytes(Texture) == Before);
		TestEqual(TEXT("Both taken: the texture is still 4x4"), Texture->Source.GetSizeX(), 4);
		TestNull(TEXT("Both taken: no bake record was added to the texture"), Texture->GetAssetUserDataOfClass(UGrungifyBakeRecord::StaticClass()));
		float Value = 0.f;
		TestTrue(TEXT("Both taken: the instance keeps its parent and value"), Instance->Parent == UMaterial::GetDefaultMaterial(MD_Surface)
			&& Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Artist Value")), Value, true) && Value == 3.f);
		TestNull(TEXT("Both taken: no bake record was added to the instance"), Instance->GetAssetUserDataOfClass(UGrungifyBakeRecord::StaticClass()));
	}

	// 2. Only the instance name is taken: the refusal comes before the (free) texture is created.
	{
		const FString Folder = TEXT("/Game/GrungifyAutomation/RefuseInstance");
		MakeUnrelatedInstance(Folder, *InstanceName);
		Zone->BakeFolder.Path = Folder;
		Zone->BakeToAssets();
		Refused(TEXT("Instance taken"));
		TestNull(TEXT("Instance taken: the free texture name was never created"), FindPackage(nullptr, *(Folder / *TextureName)));
	}

	// 3. Another class at the texture's name.
	{
		const FString Folder = TEXT("/Game/GrungifyAutomation/RefuseClass");
		MakeUnrelatedInstance(Folder, *TextureName);
		Zone->BakeFolder.Path = Folder;
		Zone->BakeToAssets();
		Refused(TEXT("Wrong class"));
	}

	// 4. Grungify's own earlier bake is still reused in place.
	Zone->BakeFolder.Path = TEXT("/Game/GrungifyAutomation/RefuseProbe");
	Zone->BakeToAssets();
	TestEqual(TEXT("Own bake: re-baking its own assets still works"), Zone->GetLastReport().Failed, 0);
	TestTrue(TEXT("Own bake: the texture now carries a bake record"), [&]()
	{
		const UTexture2D* Texture = FindObject<UTexture2D>(nullptr, *FString::Printf(TEXT("/Game/GrungifyAutomation/RefuseProbe/%s.%s"), **TextureName, **TextureName));
		return Texture && const_cast<UTexture2D*>(Texture)->GetAssetUserDataOfClass(UGrungifyBakeRecord::StaticClass()) != nullptr;
	}());
	Zone->Clear();
	return !HasAnyErrors();
}

#endif
