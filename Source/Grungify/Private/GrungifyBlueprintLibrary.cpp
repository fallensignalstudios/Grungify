// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyBlueprintLibrary.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GrungifyPreset.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif

#define LOCTEXT_NAMESPACE "GrungifyBlueprintLibrary"

namespace
{
	UWorld* WorldFrom(const UObject* WorldContextObject)
	{
		return GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	}

	void CollectZones(UWorld* World, TArray<AGrungifyWeatheringZone*>& OutZones)
	{
		if (!World)
		{
			return;
		}
		for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				OutZones.Add(*It);
			}
		}
	}
}

TArray<AGrungifyWeatheringZone*> UGrungifyBlueprintLibrary::GetZones(const UObject* WorldContextObject)
{
	TArray<AGrungifyWeatheringZone*> Zones;
	CollectZones(WorldFrom(WorldContextObject), Zones);
	return Zones;
}

AGrungifyWeatheringZone* UGrungifyBlueprintLibrary::FindZoneByLabel(const UObject* WorldContextObject, const FString& Label)
{
	TArray<AGrungifyWeatheringZone*> Zones;
	CollectZones(WorldFrom(WorldContextObject), Zones);
	for (AGrungifyWeatheringZone* Zone : Zones)
	{
		// GetActorNameOrLabel is the label in the editor and the object name in game builds (labels are editor-only).
		if (Zone->GetActorNameOrLabel().Equals(Label, ESearchCase::IgnoreCase) || Zone->GetName().Equals(Label, ESearchCase::IgnoreCase))
		{
			return Zone;
		}
	}
	return nullptr;
}

FGrungifyRunReport UGrungifyBlueprintLibrary::RunZone(AGrungifyWeatheringZone* Zone, EGrungifyOperation Operation)
{
	if (!IsValid(Zone))
	{
		return FGrungifyRunReport();
	}

	switch (Operation)
	{
	case EGrungifyOperation::DryRun: Zone->DryRun(); break;
	case EGrungifyOperation::Preview: Zone->Preview(); break;
	case EGrungifyOperation::Apply: Zone->Apply(); break;
	case EGrungifyOperation::Clear: Zone->Clear(); break;
	case EGrungifyOperation::CancelPreview: Zone->CancelPreview(); break;
	case EGrungifyOperation::Bake: Zone->BakeToAssets(); break;
	}
	return Zone->GetLastReport();
}

TArray<FGrungifyRunReport> UGrungifyBlueprintLibrary::RunAllZones(const UObject* WorldContextObject, EGrungifyOperation Operation)
{
	TArray<AGrungifyWeatheringZone*> Zones;
	CollectZones(WorldFrom(WorldContextObject), Zones);

	TArray<FGrungifyRunReport> Reports;
	Reports.Reserve(Zones.Num());

#if WITH_EDITOR
	// Groups the zones' own transactions so one undo reverts the whole batch, like the toolbar's All Zones actions.
	const FScopedTransaction Transaction(LOCTEXT("AllZones", "Grungify: All Zones"), Zones.Num() > 0);
#endif
	for (AGrungifyWeatheringZone* Zone : Zones)
	{
		Reports.Add(RunZone(Zone, Operation));
	}
	return Reports;
}

AGrungifyWeatheringZone* UGrungifyBlueprintLibrary::SpawnZone(const UObject* WorldContextObject, const FTransform& Transform, FVector HalfExtent, UGrungifyPreset* Preset)
{
	UWorld* World = WorldFrom(WorldContextObject);
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AGrungifyWeatheringZone* Zone = World->SpawnActor<AGrungifyWeatheringZone>(AGrungifyWeatheringZone::StaticClass(), Transform, Params);
	if (!Zone)
	{
		return nullptr;
	}

	if (UGrungifyZoneComponent* Box = Zone->GetZoneComponent())
	{
		Box->SetBoxExtent(HalfExtent.ComponentMax(FVector(1.0)));
	}

	if (Preset)
	{
		Zone->Preset = Preset;
#if WITH_EDITOR
		// Loading a preset refreshes a live preview; a script spawning zones should decide itself when to generate.
		const bool bWasLive = Zone->bLivePreview;
		Zone->bLivePreview = false;
		Zone->LoadPreset();
		Zone->bLivePreview = bWasLive;
#endif
	}
	return Zone;
}

FString UGrungifyBlueprintLibrary::ReportToJson(const FGrungifyRunReport& Report)
{
	FString Json;
	if (!FJsonObjectConverter::UStructToJsonObjectString(Report, Json))
	{
		return FString();
	}
	return Json;
}

bool UGrungifyBlueprintLibrary::SaveReport(const FGrungifyRunReport& Report, const FString& FilePath)
{
	if (FilePath.IsEmpty())
	{
		return false;
	}

	const FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), FilePath);
	const FString Json = ReportToJson(Report);
	if (Json.IsEmpty() || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullPath), true))
	{
		UE_LOG(LogGrungify, Warning, TEXT("Could not write Grungify report to %s"), *FullPath);
		return false;
	}
	if (!FFileHelper::SaveStringToFile(Json, *FullPath))
	{
		UE_LOG(LogGrungify, Warning, TEXT("Could not write Grungify report to %s"), *FullPath);
		return false;
	}
	return true;
}

TArray<AGrungifyWeatheringZone*> UGrungifyBlueprintLibrary::GetZonesAffecting(const UPrimitiveComponent* Component)
{
	TArray<AGrungifyWeatheringZone*> Result;
	if (!IsValid(Component))
	{
		return Result;
	}

	const FBox ComponentBox = Component->Bounds.GetBox();
	TArray<AGrungifyWeatheringZone*> Zones;
	CollectZones(Component->GetWorld(), Zones);
	for (AGrungifyWeatheringZone* Zone : Zones)
	{
		// Same box test the zone uses to pick candidates: axis-aligned bounds, so a rotated zone can over-report slightly.
		const UGrungifyZoneComponent* Box = Zone->GetZoneComponent();
		if (Box && Box->Bounds.GetBox().Intersect(ComponentBox))
		{
			Result.Add(Zone);
		}
	}
	return Result;
}

void UGrungifyBlueprintLibrary::SetZoneSeed(AGrungifyWeatheringZone* Zone, int32 Seed, bool bPreview)
{
	if (!IsValid(Zone))
	{
		return;
	}

	Zone->Modify();
	Zone->Seed = Seed;
	if (bPreview)
	{
		Zone->Preview();
	}
}

#undef LOCTEXT_NAMESPACE
