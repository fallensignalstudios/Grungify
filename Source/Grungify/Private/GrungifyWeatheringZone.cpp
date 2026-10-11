#include "GrungifyWeatheringZone.h"

#include "Components/ArrowComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GrungifyZoneComponent.h"

#if WITH_EDITOR
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/DecalComponent.h"
#include "Components/LocalLightComponent.h"
#include "Async/Async.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/DecalActor.h"
#include "Engine/StaticMeshActor.h"
#include "GrungifyTrafficPath.h"
#include "GrungifyWearStamp.h"
#include "ImageUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/GarbageCollection.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GrungifyGenerator.h"
#include "GrungifyPreset.h"
#include "GrungifySettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "ScopedTransaction.h"
#include "StaticMeshResources.h"
#endif

#define LOCTEXT_NAMESPACE "GrungifyWeatheringZone"

#if WITH_EDITOR
namespace
{
	/** Records how long a phase took, and how many items it handled, in the run report. */
	struct FGrungifyPhaseScope
	{
		FGrungifyRunReport& Report;
		FString Name;
		double Start;
		int32 Items = 0;
		bool bFinished = false;

		FGrungifyPhaseScope(FGrungifyRunReport& InReport, const TCHAR* InName)
			: Report(InReport), Name(InName), Start(FPlatformTime::Seconds())
		{
		}

		/** Records now (before the report is written); otherwise the destructor records. */
		void Finish()
		{
			if (!bFinished)
			{
				bFinished = true;
				FGrungifyPhaseTiming& Timing = Report.Phases.AddDefaulted_GetRef();
				Timing.Phase = Name;
				Timing.Seconds = float(FPlatformTime::Seconds() - Start);
				Timing.Items = Items;
			}
		}

		~FGrungifyPhaseScope()
		{
			Finish();
		}
	};
}
#endif

AGrungifyWeatheringZone::AGrungifyWeatheringZone()
{
	PrimaryActorTick.bCanEverTick = false;

	ZoneComponent = CreateDefaultSubobject<UGrungifyZoneComponent>(TEXT("Zone"));
	RootComponent = ZoneComponent;
	PresetFolder.Path = TEXT("/Game/Grungify/Presets");
	BakeFolder.Path = TEXT("/Game/Grungify/Baked");

#if WITH_EDITORONLY_DATA
	WearArrow = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("WearDirectionArrow"));
	if (WearArrow)
	{
		WearArrow->SetupAttachment(ZoneComponent);
		WearArrow->ArrowColor = FColor(80, 200, 255);
		WearArrow->ArrowSize = 2.f;
		WearArrow->bIsScreenSizeScaled = true;
		WearArrow->SetRelativeRotation(WearDirection);
	}
#endif
}

void AGrungifyWeatheringZone::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateWearArrow();
}

void AGrungifyWeatheringZone::UpdateWearArrow()
{
#if WITH_EDITORONLY_DATA
	if (WearArrow)
	{
		WearArrow->SetRelativeRotation(WearDirection);
	}
#endif
}

const FGrungifyComponentRecord* AGrungifyWeatheringZone::FindAppliedRecord(const UStaticMeshComponent* Component) const
{
	return AppliedRecords.FindByPredicate([Component](const FGrungifyComponentRecord& Record) { return Record.Component.Get() == Component; });
}

bool AGrungifyWeatheringZone::IsPreviewing(const UStaticMeshComponent* Component) const
{
	return PreviewRecords.ContainsByPredicate([Component](const FGrungifyComponentRecord& Record) { return Record.Component.Get() == Component; });
}

// ------------------------------------------------------------------ Actions

void AGrungifyWeatheringZone::Preview()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	CancelBackgroundJob();
	const double Start = FPlatformTime::Seconds();
	Modify();
	if (!RestorePreview())
	{
		return;
	}

	TArray<FGrungifyComponentResult> Results;
	TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*> TakeOvers;
	if (!GenerateResults(TEXT("Preview"), Results, TakeOvers))
	{
		FinishReport(Start);
		return;
	}
	WritePreview(Results, Start);
#endif
}

#if WITH_EDITOR
void AGrungifyWeatheringZone::WritePreview(TArray<FGrungifyComponentResult>& Results, double StartSeconds)
{
	// Writing is quick and must not stop halfway, so this dialog has no Cancel button.
	FGrungifyPhaseScope WritePhase(LastReport, TEXT("Write"));
	FScopedSlowTask WriteTask(float(FMath::Max(Results.Num(), 1)), LOCTEXT("WritingPreview", "Grungify: writing preview..."));
	WriteTask.MakeDialogDelayed(0.3f, /*bShowCancelButton*/ false);
	for (const FGrungifyComponentResult& Result : Results)
	{
		WriteTask.EnterProgressFrame(1.f);
		if (!IsValid(Result.Component))
		{
			continue;
		}
		++WritePhase.Items;
		FGrungifyComponentRecord Record;
		Grungify::CaptureRecord(Result.Component, Record);
		const FGrungifyComponentRecord* Original = FindOriginalRecord(Result.Component);
		if (WriteResult(Result, PreviewDisplay, Original ? *Original : Record))
		{
			PreviewRecords.Add(MoveTemp(Record));
		}
		else if (!LastReport.Components[Result.ReportIndex].bCustomDataConflict && !Grungify::RestoreRecord(Record))
		{
			// Keep the rollback snapshot if the failure could not be restored.
			PreviewRecords.Add(MoveTemp(Record));
		}
	}

	bPreviewActive = PreviewRecords.Num() > 0;
	WritePhase.Finish();
	FinishReport(StartSeconds);
}
#endif

void AGrungifyWeatheringZone::Apply()
{
#if WITH_EDITOR
	ApplyInternal(TEXT("Apply"));
#endif
}

void AGrungifyWeatheringZone::BakeToAssets()
{
#if WITH_EDITOR
	BakedPackages.Reset();
	bBakingToAssets = true;
	ApplyInternal(TEXT("Bake"));
	bBakingToAssets = false;
	if (BakedPackages.Num() > 0)
	{
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(BakedPackages, /*bOnlyDirty*/ false);
		if (!bSaved)
		{
			const FString Warning = TEXT("Bake To Assets: one or more generated packages could not be saved; dirty assets remain in memory.");
			LastReport.Warnings.AddUnique(Warning);
			UE_LOG(LogGrungify, Warning, TEXT("%s: %s"), *GetActorNameOrLabel(), *Warning);
			// ApplyInternal already emitted its report before the package-save result was known.
			if (bWriteReportFiles && !LastReportFile.IsEmpty())
			{
				FString Json;
				if (!FJsonObjectConverter::UStructToJsonObjectString(LastReport, Json) || !FFileHelper::SaveStringToFile(Json, *LastReportFile))
				{
					UE_LOG(LogGrungify, Warning, TEXT("%s: could not update the bake report with its save failure."), *GetActorNameOrLabel());
				}
			}
		}
		Status += FString::Printf(TEXT(" Baked %d asset(s) into %s%s."), BakedPackages.Num(), *BakeFolder.Path,
			bSaved ? TEXT("") : TEXT(" (some could not be saved; save them from the Content Browser)"));
		UE_LOG(LogGrungify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
	}
	BakedPackages.Reset();
#endif
}

#if WITH_EDITOR
void AGrungifyWeatheringZone::ApplyInternal(const TCHAR* Operation)
{
	if (!CanRunInEditorWorld())
	{
		return;
	}

	CancelBackgroundJob();
	const double Start = FPlatformTime::Seconds();
	const FScopedTransaction Transaction(bBakingToAssets
		? LOCTEXT("BakeWeathering", "Grungify: Bake Weathering To Assets")
		: LOCTEXT("ApplyWeathering", "Grungify: Apply Weathering"));
	Modify();
	if (!RestorePreview() || !CancelDependentPreviews())
	{
		return;
	}

	TArray<FGrungifyComponentResult> Results;
	TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*> TakeOvers;
	if (!GenerateResults(Operation, Results, TakeOvers))
	{
		FinishReport(Start);
		return;
	}

	// Writing is quick and must not stop halfway, so this dialog has no Cancel button.
	FGrungifyPhaseScope WritePhase(LastReport, TEXT("Write"));
	FScopedSlowTask WriteTask(float(FMath::Max(Results.Num(), 1)), LOCTEXT("WritingApply", "Grungify: applying weathering..."));
	WriteTask.MakeDialogDelayed(0.3f, /*bShowCancelButton*/ false);

	// Meshes this zone applied to before but no longer covers go back to their originals.
	for (int32 Index = AppliedRecords.Num() - 1; Index >= 0; --Index)
	{
		const UStaticMeshComponent* Previous = AppliedRecords[Index].Component.Get();
		if (!Previous)
		{
			// Not loaded (e.g. an unloaded World Partition cell): keep its backup for a later Clear.
			continue;
		}
		const bool bStillCovered = Results.ContainsByPredicate([Previous](const FGrungifyComponentResult& Result) { return Result.Component == Previous; });
		if (!bStillCovered)
		{
			if (Grungify::RestoreRecord(AppliedRecords[Index]))
			{
				AppliedRecords.RemoveAt(Index);
			}
		}
	}

	TArray<FGrungifyComponentResult> CommittedResults;
	for (FGrungifyComponentResult& Result : Results)
	{
		WriteTask.EnterProgressFrame(1.f);
		++WritePhase.Items;
		// Check before restoring an existing owner or transferring its backup. A conflict must touch nothing.
		const FGrungifyComponentRecord* Original = FindOriginalRecord(Result.Component);
		const TArray<float>& Defaults = Original ? Original->CustomData : Result.Component->GetDefaultCustomPrimitiveData().Data;
		const TArray<float>& Live = Original ? (Original->bHasLiveCustomData ? Original->LiveCustomData : Original->CustomData)
			: Result.Component->GetCustomPrimitiveData().Data;
		if (!Grungify::CustomDataSlotsFree(Defaults, Live, UGrungifySettings::Get()->CustomPrimitiveDataIndex))
		{
			if (LastReport.Components.IsValidIndex(Result.ReportIndex))
			{
				LastReport.Components[Result.ReportIndex].bCustomDataConflict = true;
				LastReport.Components[Result.ReportIndex].Result = TEXT("skipped");
			}
			continue;
		}
		int32 RecordIndex = AppliedRecords.IndexOfByPredicate([&Result](const FGrungifyComponentRecord& Record) { return Record.Component.Get() == Result.Component; });
		bool bNewRecord = false;

		if (RecordIndex != INDEX_NONE)
		{
			// Start again from the original so nothing from the previous Apply lingers.
			if (!Grungify::RestoreRecord(AppliedRecords[RecordIndex]))
			{
				LastReport.Components[Result.ReportIndex].Result = TEXT("failed");
				LastReport.Components[Result.ReportIndex].Notes += TEXT(" original state could not be restored; backup retained");
				continue;
			}
		}
		else
		{
			FGrungifyComponentRecord Record;
			AGrungifyWeatheringZone* const* PreviousOwner = TakeOvers.Find(Result.Component);
			if (PreviousOwner && *PreviousOwner)
			{
				if (!(*PreviousOwner)->ReleaseAppliedRecord(Result.Component, Record))
				{
					LastReport.Components[Result.ReportIndex].Result = TEXT("failed");
					LastReport.Components[Result.ReportIndex].Notes += TEXT(" previous owner's backup could not be restored; ownership retained");
					continue;
				}
			}
			else
			{
				Grungify::CaptureRecord(Result.Component, Record);
			}
			RecordIndex = AppliedRecords.Add(MoveTemp(Record));
			bNewRecord = true;
		}

		if (!WriteResult(Result, EGrungifyPreviewDisplay::FinalMaterial, AppliedRecords[RecordIndex]))
		{
			const bool bRolledBack = LastReport.Components[Result.ReportIndex].bCustomDataConflict
				|| Grungify::RestoreRecord(AppliedRecords[RecordIndex]);
			if (bNewRecord && bRolledBack)
			{
				AppliedRecords.RemoveAt(RecordIndex);
			}
		}
		else
		{
			CommittedResults.Add(MoveTemp(Result));
		}
	}

	WritePhase.Finish();

	// Never discard previous scatter after a partial write or conflict.
	const bool bWriteFailure = LastReport.Components.ContainsByPredicate([](const FGrungifyComponentReport& Report)
	{
		return Report.Result == TEXT("failed") || Report.bCustomDataConflict;
	});
	if (CommittedResults.Num() == Results.Num() && !bWriteFailure)
	{
		ClearScatteredActors();
		ScatterFromResults(CommittedResults);
	}
	else
	{
		LastReport.Warnings.AddUnique(TEXT("Scatter retained from previous Apply: one or more component writes failed or conflicted; no replacement scatter was generated."));
	}
	FinishReport(Start);
}
#endif

void AGrungifyWeatheringZone::Clear()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	CancelBackgroundJob();
	const FScopedTransaction Transaction(LOCTEXT("ClearWeathering", "Grungify: Clear Weathering"));
	Modify();
	if (!RestorePreview() || !CancelDependentPreviews())
	{
		return;
	}
	ClearScatteredActors();

	int32 NumCleared = 0;
	for (int32 Index = AppliedRecords.Num() - 1; Index >= 0; --Index)
	{
		// Meshes that aren't loaded keep their record so a later Clear can still restore them.
		if (AppliedRecords[Index].Component.Get())
		{
			if (Grungify::RestoreRecord(AppliedRecords[Index]))
			{
				AppliedRecords.RemoveAt(Index);
				++NumCleared;
			}
		}
	}

	Status = AppliedRecords.Num() > 0
		? FString::Printf(TEXT("Cleared %d mesh(es). %d backups retained (unloaded or restoration failed); see Output Log."), NumCleared, AppliedRecords.Num())
		: FString::Printf(TEXT("Cleared %d mesh(es)."), NumCleared);
	UE_LOG(LogGrungify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
#endif
}

void AGrungifyWeatheringZone::CancelPreview()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	CancelBackgroundJob();
	Modify();
	if (!RestorePreview())
	{
		return;
	}
	Status = AppliedRecords.Num() > 0
		? FString::Printf(TEXT("Preview cancelled. %d mesh(es) applied."), AppliedRecords.Num())
		: FString(TEXT("Preview cancelled."));
#endif
}

void AGrungifyWeatheringZone::NewSeed()
{
#if WITH_EDITOR
	Modify();
	Seed = FMath::RandRange(1, 999999);
	RefreshPreviewIfLive();
#endif
}

void AGrungifyWeatheringZone::MakeSeedVariants()
{
#if WITH_EDITOR
	Modify();
	SeedVariants = { Seed, FMath::RandRange(1, 999999), FMath::RandRange(1, 999999) };
	VariantIndex = 0;
	Status = FString::Printf(TEXT("Seed variants: %d, %d, %d. Use Preview Next / Previous Variant to compare; each result stays cached."),
		SeedVariants[0], SeedVariants[1], SeedVariants[2]);
#endif
}

void AGrungifyWeatheringZone::PreviewNextVariant()
{
#if WITH_EDITOR
	if (SeedVariants.Num() == 0)
	{
		MakeSeedVariants();
	}
	Modify();
	VariantIndex = (VariantIndex + 1) % SeedVariants.Num();
	Seed = SeedVariants[VariantIndex];
	// Variants are compared side by side in time, so always generate now, cached when possible.
	Preview();
	Status = FString::Printf(TEXT("Variant %d / %d (seed %d). %s"), VariantIndex + 1, SeedVariants.Num(), Seed, *Status);
#endif
}

void AGrungifyWeatheringZone::PreviewPreviousVariant()
{
#if WITH_EDITOR
	if (SeedVariants.Num() == 0)
	{
		MakeSeedVariants();
	}
	Modify();
	VariantIndex = (VariantIndex + SeedVariants.Num() - 1) % SeedVariants.Num();
	Seed = SeedVariants[VariantIndex];
	Preview();
	Status = FString::Printf(TEXT("Variant %d / %d (seed %d). %s"), VariantIndex + 1, SeedVariants.Num(), Seed, *Status);
#endif
}

void AGrungifyWeatheringZone::ClearGenerationCache()
{
#if WITH_EDITOR
	CancelBackgroundJob();
	const int32 Count = GenerationCache.Num();
	GenerationCache.Reset();
	Status = FString::Printf(TEXT("Forgot %d cached mesh result(s)."), Count);
#endif
}

bool AGrungifyWeatheringZone::IsGeneratingInBackground() const
{
#if WITH_EDITOR
	return BackgroundJob.IsValid() && !BackgroundJob->bFinished;
#else
	return false;
#endif
}

void AGrungifyWeatheringZone::BeginDestroy()
{
#if WITH_EDITOR
	CancelBackgroundJob();
	GenerationCache.Reset();
#endif
	Super::BeginDestroy();
}

void AGrungifyWeatheringZone::DryRun()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}
	CancelBackgroundJob();

	const double Start = FPlatformTime::Seconds();
	TArray<FGrungifyComponentResult> Results;
	TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*> TakeOvers;
	if (!GenerateResults(TEXT("DryRun"), Results, TakeOvers))
	{
		FinishReport(Start);
		return;
	}

	// Flag the custom data conflicts a real run would hit.
	for (const FGrungifyComponentResult& Result : Results)
	{
		const FGrungifyComponentRecord* Original = FindOriginalRecord(Result.Component);
		const TArray<float>& Defaults = Original ? Original->CustomData : Result.Component->GetDefaultCustomPrimitiveData().Data;
		const TArray<float>& Live = Original ? (Original->bHasLiveCustomData ? Original->LiveCustomData : Original->CustomData)
			: Result.Component->GetCustomPrimitiveData().Data;
		if (!Grungify::CustomDataSlotsFree(Defaults, Live, UGrungifySettings::Get()->CustomPrimitiveDataIndex) && LastReport.Components.IsValidIndex(Result.ReportIndex))
		{
			LastReport.Components[Result.ReportIndex].bCustomDataConflict = true;
			LastReport.Components[Result.ReportIndex].Result = TEXT("skipped");
		}
	}

	FinishReport(Start);
#endif
}


void AGrungifyWeatheringZone::LoadPreset()
{
#if WITH_EDITOR
	if (!Preset)
	{
		Status = TEXT("No preset assigned. Pick one in Preset, or use Save To Preset to create one from this zone.");
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("LoadPreset", "Grungify: Load Preset"));
	Modify();
	CopyFromPreset(*Preset);
	Status = FString::Printf(TEXT("Loaded preset %s%s."), *Preset->GetName(), Preset->bIncludeSeed ? TEXT(" (including seed)") : TEXT(""));
	UE_LOG(LogGrungify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
	RefreshPreviewIfLive();
#endif
}

void AGrungifyWeatheringZone::SaveToPreset()
{
#if WITH_EDITOR
	UGrungifyPreset* Target = Preset;
	bool bCreated = false;

	if (!Target)
	{
		// New asset in the project's content, never in the plugin's generated content.
		FString Folder = PresetFolder.Path.IsEmpty() ? FString(TEXT("/Game/Grungify/Presets")) : PresetFolder.Path;
		Folder.RemoveFromEnd(TEXT("/"));
		FString BaseName;
		for (const TCHAR Ch : GetActorNameOrLabel())
		{
			BaseName.AppendChar(FChar::IsAlnum(Ch) ? Ch : TEXT('_'));
		}
		BaseName = TEXT("GP_") + BaseName;

		FString AssetName = BaseName;
		FString PackageName = Folder / AssetName;
		for (int32 Suffix = 2; FPackageName::DoesPackageExist(PackageName) || FindPackage(nullptr, *PackageName); ++Suffix)
		{
			AssetName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
			PackageName = Folder / AssetName;
		}

		FText Error;
		if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ false, &Error))
		{
			Status = FString::Printf(TEXT("Preset not saved: '%s' is not a valid content path (%s)."), *PackageName, *Error.ToString());
			UE_LOG(LogGrungify, Warning, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
			return;
		}

		UPackage* Package = CreatePackage(*PackageName);
		Target = NewObject<UGrungifyPreset>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Target);
		bCreated = true;
	}

	Target->Modify();
#define GRUNGIFY_COPY_TO_PRESET(Field) Target->Field = Field;
	GRUNGIFY_PRESET_FIELDS(GRUNGIFY_COPY_TO_PRESET)
#undef GRUNGIFY_COPY_TO_PRESET
	Target->Seed = Seed;
	Target->MarkPackageDirty();

	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Target->GetPackage() }, /*bOnlyDirty*/ false);

	if (bCreated)
	{
		const FScopedTransaction Transaction(LOCTEXT("AssignPreset", "Grungify: Assign New Preset"));
		Modify();
		Preset = Target;
	}

	Status = bSaved
		? FString::Printf(TEXT("%s preset %s."), bCreated ? TEXT("Created") : TEXT("Saved"), *Target->GetPathName())
		: FString::Printf(TEXT("Preset %s was updated in memory but could not be saved to disk; save it from the Content Browser."), *Target->GetPathName());
	UE_LOG(LogGrungify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
#endif
}

#if WITH_EDITOR

void AGrungifyWeatheringZone::CopyFromPreset(const UGrungifyPreset& Source)
{
#define GRUNGIFY_COPY_FROM_PRESET(Field) Field = Source.Field;
	GRUNGIFY_PRESET_FIELDS(GRUNGIFY_COPY_FROM_PRESET)
#undef GRUNGIFY_COPY_FROM_PRESET
	if (Source.bIncludeSeed)
	{
		Seed = Source.Seed;
	}
}

// ------------------------------------------------------------ Editor hooks

void AGrungifyWeatheringZone::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateWearArrow();

	// Undo/redo arrives with no property; never regenerate then.
	if (!PropertyChangedEvent.Property || GIsTransacting)
	{
		return;
	}

	const FName MemberName = PropertyChangedEvent.GetMemberPropertyName();

	// Assigning a preset copies it in (one undo step), then regenerates like any other change.
	if (MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, Preset))
	{
		if (Preset)
		{
			LoadPreset();
		}
		return;
	}
	if (MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, PresetFolder))
	{
		return;
	}
	if (MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, Status)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, bPreviewActive)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, LastReport)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, bWriteReportFiles)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, LastReportFile)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, Scatter)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, ScatterDataLayers)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, BakeFolder)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, bExportBakedPNGs)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, CacheBudgetMB)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, bUseGenerationCache)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, bBackgroundGeneration)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, SeedVariants)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, VariantIndex))
	{
		if (MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, bUseGenerationCache) && !bUseGenerationCache)
		{
			GenerationCache.Reset();
		}
		else if (MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, CacheBudgetMB))
		{
			TrimCache();
		}
		return;
	}

	// Dragging a slider or a leak widget fires Interactive changes every frame; wait for the release.
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	RefreshPreviewIfLive();
}

void AGrungifyWeatheringZone::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	if (bFinished && !GIsTransacting)
	{
		RefreshPreviewIfLive();
	}
}

void AGrungifyWeatheringZone::PostEditImport()
{
	Super::PostEditImport();
	ForgetRecords();
}

void AGrungifyWeatheringZone::PostDuplicate(EDuplicateMode::Type DuplicateMode)
{
	Super::PostDuplicate(DuplicateMode);
	if (DuplicateMode == EDuplicateMode::Normal)
	{
		ForgetRecords();
	}
}

void AGrungifyWeatheringZone::ForgetRecords()
{
	// A pasted or duplicated zone starts fresh: the meshes belong to the zone that weathered them.
	PreviewRecords.Reset();
	AppliedRecords.Reset();
	bPreviewActive = false;
	Status.Reset();
}

void AGrungifyWeatheringZone::NotifyZoneShapeChanged(const FPropertyChangedEvent& PropertyChangedEvent)
{
	if (PropertyChangedEvent.Property && !GIsTransacting && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		RefreshPreviewIfLive();
	}
}

void AGrungifyWeatheringZone::RefreshPreviewIfLive()
{
	if (!CanRunInEditorWorld())
	{
		return;
	}
	if (bLivePreview)
	{
		RequestLivePreview();
	}
	else
	{
		// Keep the viewport's runoff gizmos current even when not previewing.
		RefreshRunoffSources();
	}
}

bool AGrungifyWeatheringZone::CanRunInEditorWorld() const
{
	const UWorld* World = GetWorld();
	return World && World->WorldType == EWorldType::Editor && !IsTemplate() && ZoneComponent;
}

#endif // WITH_EDITOR

// --------------------------------------------------------------- Lifecycle

void AGrungifyWeatheringZone::Destroyed()
{
#if WITH_EDITOR
	CancelBackgroundJob();
	// Deleting the zone drops its preview inside the delete transaction, so undo brings both back.
	if (bPreviewActive && !GIsTransacting && CanRunInEditorWorld())
	{
		Modify();
		RestorePreview();
	}
#endif
	Super::Destroyed();
}

void AGrungifyWeatheringZone::PostUnregisterAllComponents()
{
	Super::PostUnregisterAllComponents();

#if WITH_EDITOR
	// Level unload or streaming out: put previewed meshes back so no un-owned preview is left behind.
	const ULevel* Level = GetLevel();
	const bool bLeaving = IsActorBeingDestroyed() || (Level && Level->bIsBeingRemoved);
	if (bPreviewActive && bLeaving && !GIsTransacting && !IsEngineExitRequested() && CanRunInEditorWorld())
	{
		DiscardPreview(TEXT("zone unloaded"));
	}
#endif
}

#if WITH_EDITOR

void AGrungifyWeatheringZone::DiscardPreview(const TCHAR* Reason)
{
	if (!bPreviewActive && PreviewRecords.Num() == 0)
	{
		return;
	}

	for (int32 Index = PreviewRecords.Num() - 1; Index >= 0; --Index)
	{
		if (Grungify::RestoreRecord(PreviewRecords[Index], /*bTransactional*/ false))
		{
			PreviewRecords.RemoveAt(Index);
		}
	}
	bPreviewActive = PreviewRecords.Num() > 0;

	Status = bPreviewActive
		? FString(TEXT("Preview restoration failed; backups retained. See Output Log."))
		: FString::Printf(TEXT("Preview discarded (%s). Press Preview to see it again."), Reason);
	UE_LOG(LogGrungify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
}

bool AGrungifyWeatheringZone::ReleaseAppliedRecord(const UStaticMeshComponent* Component, FGrungifyComponentRecord& OutRecord)
{
	const int32 Index = AppliedRecords.IndexOfByPredicate([Component](const FGrungifyComponentRecord& Record) { return Record.Component.Get() == Component; });
	if (Index == INDEX_NONE)
	{
		return false;
	}
	Modify();
	if (!Grungify::RestoreRecord(AppliedRecords[Index]))
	{
		return false;
	}
	OutRecord = MoveTemp(AppliedRecords[Index]);
	AppliedRecords.RemoveAt(Index);
	Status = FString::Printf(TEXT("%s was taken over by another zone."), *Component->GetName());
	return true;
}

// ---------------------------------------------------------------- Internals

bool AGrungifyWeatheringZone::RestorePreview()
{
	for (int32 Index = PreviewRecords.Num() - 1; Index >= 0; --Index)
	{
		if (Grungify::RestoreRecord(PreviewRecords[Index]))
		{
			PreviewRecords.RemoveAt(Index);
		}
	}
	bPreviewActive = PreviewRecords.Num() > 0;
	if (bPreviewActive)
	{
		Status = TEXT("Some preview state could not be restored; backups retained. See Output Log.");
	}
	return !bPreviewActive;
}

bool AGrungifyWeatheringZone::CancelDependentPreviews()
{
	// Older saved records can contain previews of this zone's applied layer. Retire those snapshots
	// before changing the owner, so a later Cancel cannot resurrect an owner that has already cleared.
	for (TActorIterator<AGrungifyWeatheringZone> It(GetWorld()); It; ++It)
	{
		AGrungifyWeatheringZone* Other = *It;
		if (Other == this)
		{
			continue;
		}
		const bool bDependsOnUs = AppliedRecords.ContainsByPredicate([Other](const FGrungifyComponentRecord& Record)
			{ return Record.Component.Get() && Other->IsPreviewing(Record.Component.Get()); });
		if (bDependsOnUs)
		{
			Other->CancelPreview();
			if (AppliedRecords.ContainsByPredicate([Other](const FGrungifyComponentRecord& Record)
				{ return Record.Component.Get() && Other->IsPreviewing(Record.Component.Get()); }))
			{
				Status = TEXT("Another zone's dependent preview could not be restored; owner state retained.");
				return false;
			}
		}
	}
	return true;
}

void AGrungifyWeatheringZone::RefreshRunoffSources()
{
	CachedRunoffSources.Reset();

	UWorld* World = GetWorld();
	if (!World || !ZoneComponent)
	{
		return;
	}

	auto AddSource = [this](const FBox& Box)
	{
		if (!Box.IsValid)
		{
			return;
		}
		const FVector Center = Box.GetCenter();
		const FVector Extent = Box.GetExtent();
		FGrungifyRunoffSource& Source = CachedRunoffSources.AddDefaulted_GetRef();
		Source.Origin = FVector(Center.X, Center.Y, Box.Min.Z);
		Source.Radius = FMath::Clamp(float(FMath::Max(Extent.X, Extent.Y)), 5.f, 300.f);
	};

	for (const FGrungifyLeak& Leak : Leaks)
	{
		FGrungifyRunoffSource& Source = CachedRunoffSources.AddDefaulted_GetRef();
		Source.Origin = GetActorTransform().TransformPosition(Leak.Location);
		Source.Radius = FMath::Max(Leak.RadiusCm, 1.f);
		Source.Length = Leak.LengthCm;
		Source.Strength = Leak.Strength;
		Source.bManual = true;
	}

	for (const TSoftObjectPtr<AActor>& Explicit : RunoffSources)
	{
		if (const AActor* Actor = Explicit.Get())
		{
			AddSource(Actor->GetComponentsBoundingBox(/*bNonColliding*/ true));
		}
	}

	if (!bAutoDetectVents)
	{
		return;
	}

	// Vents above the zone can still drip into it.
	FBox SearchBox = ZoneComponent->Bounds.GetBox();
	SearchBox.Max.Z += RunoffLengthCm;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!Actor || Actor == this || Actor->IsA<AGrungifyWeatheringZone>() || Grungify::HasIgnoreTag(Actor, nullptr))
		{
			continue;
		}

		const FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
		if (!ActorBox.IsValid || !ActorBox.Intersect(SearchBox))
		{
			continue;
		}

		if (Grungify::IsVent(Actor, nullptr))
		{
			AddSource(ActorBox);
			continue;
		}

		// Vents can also be individual parts of a larger actor.
		TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
		for (const UPrimitiveComponent* Primitive : Primitives)
		{
			if (Primitive && Primitive->IsRegistered() && !Grungify::HasIgnoreTag(nullptr, Primitive) && Grungify::IsVent(nullptr, Primitive))
			{
				const FBox Box = Primitive->Bounds.GetBox();
				if (Box.Intersect(SearchBox))
				{
					AddSource(Box);
				}
			}
		}
	}
}

void AGrungifyWeatheringZone::BuildParams(FGrungifyGenerationParams& P) const
{
	P.Seed = uint32(Seed);
	P.Intensity = Intensity;
	P.FeatureScale = FeatureScaleCm;

	P.ZoneTransform = ZoneComponent->GetComponentTransform();
	P.ZoneTransform.SetScale3D(FVector::OneVector);
	P.ZoneExtent = ZoneComponent->GetScaledBoxExtent();
	P.ZoneFalloff = ZoneFalloffCm;

	P.WearDir = GetActorTransform().TransformVectorNoScale(WearDirection.Vector()).GetSafeNormal();
	P.WearInfluence = WearDirectionInfluence;

	P.DustIntensity = DustIntensity;
	P.DustHeight = DustHeightCm;
	P.LedgeDust = LedgeDust;
	P.CreviceDust = CreviceDust;
	P.FloorDust = FloorDust;

	P.RunoffIntensity = RunoffIntensity;
	P.RunoffLength = FMath::Max(RunoffLengthCm, 10.f);
	P.RunoffSpread = RunoffSpread;
	P.AmbientStreaks = AmbientStreaks;

	P.HandlingIntensity = HandlingIntensity;
	P.HandMin = FMath::Min(HandHeightMinCm, HandHeightMaxCm);
	P.HandMax = FMath::Max(HandHeightMinCm, HandHeightMaxCm);
	P.GeneralHandling = GeneralHandling;

	P.ChipIntensity = ChipIntensity;
	P.EdgeAngleDeg = EdgeAngleDegrees;
	P.EdgeWearWidth = EdgeWearWidthCm;
	P.FloorKickHeight = FloorKickHeightCm;

	P.MaskOutput = MaskOutput;
	P.MaskTexelSize = MaskTexelSizeCm;
	P.MaxMaskResolution = MaxMaskResolution;
	P.MaskUVChannel = UGrungifySettings::Get()->MaskUVChannel;
}

void AGrungifyWeatheringZone::BuildContext(UStaticMeshComponent* Component, TMap<UTexture2D*, TUniquePtr<FGrungifyGateSampler>>& Samplers,
	FGrungifyComponentContext& OutContext, FGrungifyComponentReport& Report)
{
	FString ComponentRailSource;
	const bool bComponentRail = Grungify::IsRail(Component, &ComponentRailSource);
	const UEnum* SurfaceEnum = StaticEnum<EGrungifySurfaceType>();
	const UStaticMesh* Mesh = Component->GetStaticMesh();
	const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
	const int32 NumUVChannels = (RenderData && RenderData->LODResources.Num() > 0)
		? int32(RenderData->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()) : 0;
	const int32 NumSlots = FMath::Max(Component->GetNumMaterials(), 1);
	OutContext.Slots.SetNum(NumSlots);

	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		FGrungifySlotInfo& Info = OutContext.Slots[Slot];
		FGrungifySlotReport& Detail = Report.SlotDetails.AddDefaulted_GetRef();
		Detail.Slot = Slot;
		if (Mesh && Mesh->GetStaticMaterials().IsValidIndex(Slot))
		{
			Detail.SlotName = Mesh->GetStaticMaterials()[Slot].MaterialSlotName.ToString();
		}
		const UMaterialInterface* Material = Component->GetMaterial(Slot);
		Detail.Material = Material ? Material->GetName() : TEXT("(none)");

		FString RuleWhy;
		const FGrungifySlotRule* Rule = nullptr;
		for (const FGrungifySlotRule& Candidate : SlotRules)
		{
			if (Grungify::RuleMatchesSlot(Candidate, Component, Slot, &RuleWhy))
			{
				Rule = &Candidate;
				break;
			}
		}
		const FString RuleName = Rule ? FString::Printf(TEXT("'%s'"), Rule->Label.IsEmpty() ? TEXT("(unnamed)") : *Rule->Label) : FString();
		if (Rule)
		{
			Detail.Rule = FString::Printf(TEXT("%s, matched by %s"), *RuleName, *RuleWhy);
		}

		// Surface family.
		if (Rule && Rule->Surface != EGrungifySurfaceType::Auto)
		{
			Info.Surface = Rule->Surface;
			Detail.SurfaceSource = FString::Printf(TEXT("rule %s"), *RuleName);
		}
		else
		{
			Info.Surface = Grungify::ClassifySlot(Component, Slot, SurfaceType, &Detail.SurfaceSource);
		}
		Detail.Surface = SurfaceEnum->GetNameStringByValue(int64(Info.Surface));

		// Profile: rule's own, then a named material profile, then the surface's.
		switch (Info.Surface)
		{
		case EGrungifySurfaceType::Metal: Info.Profile = MetalProfile; Detail.Profile = TEXT("Metal Profile"); break;
		case EGrungifySurfaceType::Trim:  Info.Profile = TrimProfile; Detail.Profile = TEXT("Trim Profile"); break;
		default:                          Info.Profile = StoneProfile; Detail.Profile = TEXT("Stone Profile"); break;
		}

		Info.bRail = bComponentRail;
		Detail.RailSource = bComponentRail ? ComponentRailSource : FString();

		bool bGateUnreadable = false;
		if (Rule)
		{
			Info.bExcluded = Rule->bExclude;
			if (Rule->bOverrideProfile)
			{
				Info.Profile = Rule->Profile;
				Detail.Profile = FString::Printf(TEXT("rule %s profile"), *RuleName);
			}
			else if (!Rule->ProfileName.IsNone())
			{
				if (const FGrungifySurfaceProfile* Named = MaterialProfiles.Find(Rule->ProfileName))
				{
					Info.Profile = *Named;
					Detail.Profile = FString::Printf(TEXT("material profile '%s'"), *Rule->ProfileName.ToString());
				}
				else
				{
					Detail.Warnings.Add(FString::Printf(TEXT("rule %s names profile '%s', which isn't in Material Profiles; used %s instead"),
						*RuleName, *Rule->ProfileName.ToString(), *Detail.Profile));
				}
			}
			if (Rule->Rail != EGrungifyRailMode::Inherit)
			{
				Info.bRail = Rule->Rail == EGrungifyRailMode::Rail;
				Detail.RailSource = FString::Printf(TEXT("rule %s (%s)"), *RuleName, Info.bRail ? TEXT("forced on") : TEXT("forced off"));
			}

			const FGrungifyGate& Gate = Rule->Gate;
			Info.Gate = &Gate;
			TArray<FString> Gates;
			if (Gate.bUseFacingGate)
			{
				Gates.Add(FString::Printf(TEXT("facing %.2f..%.2f"), Gate.MinUpFacing, Gate.MaxUpFacing));
			}
			if (Gate.bUseHeightGate)
			{
				Gates.Add(FString::Printf(TEXT("height %.0f..%.0f cm"), Gate.MinHeightCm, Gate.MaxHeightCm));
			}
			if (UTexture2D* GateTexture = Gate.GateTexture)
			{
				Gates.Add(FString::Printf(TEXT("texture %s %s on UV%d%s"), *GateTexture->GetName(),
					*StaticEnum<EGrungifyChannel>()->GetNameStringByValue(int64(Gate.GateChannel)), Gate.GateUVChannel, Gate.bInvertGate ? TEXT(" (inverted)") : TEXT("")));
				if (NumUVChannels > 0 && Gate.GateUVChannel >= NumUVChannels)
				{
					Detail.Warnings.Add(FString::Printf(TEXT("gate UV channel %d doesn't exist on this mesh (%d channel(s)); UV%d is used instead"),
						Gate.GateUVChannel, NumUVChannels, NumUVChannels - 1));
				}

				TUniquePtr<FGrungifyGateSampler>& Sampler = Samplers.FindOrAdd(GateTexture);
				FString Error;
				if (!Sampler.IsValid())
				{
					Sampler = MakeUnique<FGrungifyGateSampler>();
					if (!Sampler->Init(GateTexture, Error))
					{
						LastReport.Warnings.AddUnique(FString::Printf(TEXT("Rule %s: %s. Slots using it are left untouched."), *RuleName, *Error));
						Sampler.Reset();
					}
				}
				if (Sampler.IsValid())
				{
					Info.GateSampler = Sampler.Get();
				}
				else
				{
					// Fail safe: an unreadable gate must not let wear through everywhere.
					Info.bExcluded = true;
					bGateUnreadable = true;
					Detail.Warnings.Add(FString::Printf(TEXT("gate texture %s could not be read%s"), *GateTexture->GetName(), Error.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(": %s"), *Error)));
				}
			}
			Detail.Gates = FString::Join(Gates, TEXT(", "));
		}

		Detail.bRail = Info.bRail;
		Detail.bIncluded = !Info.bExcluded;
		if (Rule && Rule->bExclude)
		{
			Detail.Reason = FString::Printf(TEXT("excluded by rule %s (Exclude is on; matched by %s)"), *RuleName, *RuleWhy);
		}
		else if (bGateUnreadable)
		{
			Detail.Reason = FString::Printf(TEXT("excluded: rule %s has a gate texture that couldn't be read, so no wear is allowed (fail-safe)"), *RuleName);
		}
		else if (Rule)
		{
			Detail.Reason = FString::Printf(TEXT("included; rule %s applies (matched by %s)"), *RuleName, *RuleWhy);
		}
		else
		{
			Detail.Reason = SlotRules.Num() > 0 ? TEXT("included; no slot rule matched") : TEXT("included; zone has no slot rules");
		}
		if (!Info.bExcluded && !Material)
		{
			Detail.Warnings.Add(TEXT("slot has no material, so there is nothing to bind the weathering to"));
		}
		float ActiveDefault = 0.f;
		if (!Info.bExcluded && Material && !Material->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(Grungify::ActiveParameterName()), ActiveDefault))
		{
			// Masks are still written (the debug display shows them), but the final material can't show them.
			Detail.Warnings.Add(FString::Printf(TEXT("material %s doesn't use Grungify, so the weathering won't show in Final Material; ")
				TEXT("use Tools > Grungify > Hook Up Materials In Level, or right-click the material > Add Grungify"), *Material->GetName()));
		}

		Info.Description = FString::Printf(TEXT("%d %s -> %s%s%s%s"),
			Slot,
			*Detail.Material,
			*Detail.Surface,
			Rule ? *FString::Printf(TEXT(" [rule %s]"), *RuleName) : TEXT(""),
			Info.bRail ? TEXT(" [rail]") : TEXT(""),
			Info.bExcluded ? TEXT(" [excluded]") : TEXT(""));
		Report.Slots.Add(Info.Description);
	}
}

bool AGrungifyWeatheringZone::ShouldConsiderActor(const AActor* Actor, const FBox& ZoneBox, FString* OutReason) const
{
	if (!Actor || Actor == this || Actor->IsA<AGrungifyWeatheringZone>() || Actor->IsEditorOnly() || Actor->IsHiddenEd())
	{
		return false;
	}
	const FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
	if (!ActorBox.IsValid || !ActorBox.Intersect(ZoneBox))
	{
		return false;
	}

	// Inside the zone but filtered by a setting: say which one.
	auto Filtered = [OutReason](const FString& Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
		return false;
	};
	if (Grungify::HasIgnoreTag(Actor, nullptr))
	{
		return Filtered(FString::Printf(TEXT("actor has the '%s' tag"), *UGrungifySettings::Get()->IgnoreTag.ToString()));
	}
	if (ExcludedActors.ContainsByPredicate([Actor](const TSoftObjectPtr<AActor>& Excluded) { return Excluded.Get() == Actor; }))
	{
		return Filtered(TEXT("actor is in the zone's Excluded Actors"));
	}
	if (IncludeOnlyActors.Num() > 0 && !IncludeOnlyActors.ContainsByPredicate([Actor](const TSoftObjectPtr<AActor>& Included) { return Included.Get() == Actor; }))
	{
		return Filtered(TEXT("actor is not in the zone's Include Only Actors"));
	}
	if (IncludeDataLayers.Num() > 0)
	{
		const TArray<const UDataLayerAsset*> ActorLayers = Actor->GetDataLayerAssets();
		if (!IncludeDataLayers.ContainsByPredicate([&ActorLayers](const TObjectPtr<const UDataLayerAsset>& Layer) { return Layer && ActorLayers.Contains(Layer.Get()); }))
		{
			return Filtered(TEXT("actor is in none of the zone's Include Data Layers"));
		}
	}
	return true;
}

const FGrungifyComponentRecord* AGrungifyWeatheringZone::FindOriginalRecord(const UStaticMeshComponent* Component) const
{
	// Whether the slots are free is decided by the pre-Grungify values, not by values this or another zone wrote.
	if (const FGrungifyComponentRecord* Own = FindAppliedRecord(Component))
	{
		return Own;
	}
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
		{
			if (*It != this)
			{
				if (const FGrungifyComponentRecord* Other = It->FindAppliedRecord(Component))
				{
					return Other;
				}
			}
		}
	}
	return nullptr;
}

namespace
{
	/** Zone-owned instances wrap the original material; hash and classify against the original. */
	const UMaterialInterface* UnwrapOwnedMaterial(const UMaterialInterface* Material, const UObject* Owner)
	{
		while (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
		{
			if (Instance->GetOuter() != Owner)
			{
				break;
			}
			Material = Instance->Parent;
		}
		return Material;
	}

	struct FHasher
	{
		uint32 Hash = 0;
		void Add(uint32 Value) { Hash = HashCombine(Hash, Value); }
		void Add(float Value) { Add(GetTypeHash(Value)); }
		void Add(const FVector& Value) { Add(GetTypeHash(Value)); }
		void Add(const FString& Value) { Add(GetTypeHash(Value)); }
	};

	uint32 HashParams(const FGrungifyGenerationParams& P)
	{
		FHasher H;
		H.Add(P.Seed);
		for (const float Value : { P.Intensity, P.FeatureScale, P.ZoneFalloff, P.WearInfluence, P.DustIntensity, P.DustHeight, P.LedgeDust,
			P.CreviceDust, P.FloorDust, P.RunoffIntensity, P.RunoffLength, P.RunoffSpread, P.AmbientStreaks, P.HandlingIntensity, P.HandMin,
			P.HandMax, P.GeneralHandling, P.ChipIntensity, P.EdgeAngleDeg, P.EdgeWearWidth, P.FloorKickHeight, P.MaskTexelSize, P.SkyInfluence,
			P.ScatterDensity })
		{
			H.Add(Value);
		}
		H.Add(P.ZoneTransform.GetLocation());
		H.Add(P.ZoneTransform.GetRotation().Euler());
		H.Add(P.ZoneTransform.GetScale3D());
		H.Add(P.ZoneExtent);
		H.Add(P.WearDir);
		H.Add(uint32(P.MaskOutput));
		H.Add(uint32(P.MaxMaskResolution));
		H.Add(uint32(P.MaskUVChannel));
		H.Add(uint32(P.bExtended));
		H.Add(uint32(P.Extended.bSootFromLights));
		const FGrungifyExtendedLayers& E = P.Extended;
		for (const float Value : { E.MoistureIntensity, E.PoolDampRadiusCm, E.WaterLineHeightCm, E.MossIntensity, E.NorthYawDegrees, E.MossNorthBias,
			E.SootIntensity, E.SootRadiusCm, E.SootPlumeHeightCm, E.ScorchIntensity, E.ScorchRadiusCm, E.ScuffIntensity, E.ScuffWallHeightCm,
			E.RustBloomIntensity, E.RustBloomRadiusCm })
		{
			H.Add(Value);
		}
		for (const FGrungifyRunoffSource& Source : P.Sources)
		{
			H.Add(Source.Origin);
			H.Add(Source.Radius);
			H.Add(Source.Length);
			H.Add(Source.Strength);
			H.Add(uint32(Source.Path.Num()));
		}
		for (const FGrungifyHeatSource& Heat : P.HeatSources)
		{
			H.Add(Heat.Location);
			H.Add(uint32(Heat.bFire));
		}
		for (const FVector& Fastener : P.Fasteners)
		{
			H.Add(Fastener);
		}
		for (const FGrungifyPool& Pool : P.Pools)
		{
			H.Add(Pool.Center);
			H.Add(Pool.Radius);
		}
		for (const FGrungifyStampData& Stamp : P.Stamps)
		{
			H.Add(Stamp.Transform.GetLocation());
			H.Add(Stamp.Transform.GetRotation().Euler());
			H.Add(Stamp.Transform.GetScale3D());
			const FGrungifyStampLayers& L = Stamp.Layers;
			for (const float Value : { Stamp.Radius, Stamp.Falloff, Stamp.EdgeNoise, L.EdgeChips, L.Dust, L.Runoff, L.Handling, L.Moisture, L.Soot, L.Scorch, L.Moss })
			{
				H.Add(Value);
			}
		}
		return H.Hash;
	}

	/** Everything about one component that changes its masks, beyond the run-wide inputs. */
	uint32 HashComponent(UStaticMeshComponent* Component, const FGrungifyComponentContext& Context)
	{
		FHasher H;
		H.Add(Component->GetPathName());
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		H.Add(Mesh ? Mesh->GetPathName() : FString());
		if (const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr)
		{
			H.Add(RenderData->DerivedDataKey);
		}
		const FTransform& Transform = Component->GetComponentTransform();
		H.Add(Transform.GetLocation());
		H.Add(Transform.GetRotation().Euler());
		H.Add(Transform.GetScale3D());
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			const UMaterialInterface* Material = UnwrapOwnedMaterial(Component->GetMaterial(Slot), Component);
			H.Add(Material ? Material->GetPathName() : FString());
		}
		H.Add(Grungify::HashSlotContext(Context));
		if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
		{
			H.Add(uint32(Instanced->GetInstanceCount()));
			for (int32 Index = 0; Index < Instanced->GetInstanceCount(); ++Index)
			{
				FTransform Instance;
				if (Instanced->GetInstanceTransform(Index, Instance, /*bWorldSpace*/ true))
				{
					H.Add(Instance.GetLocation());
					H.Add(Instance.GetRotation().Euler());
					H.Add(Instance.GetScale3D());
				}
			}
		}
		return H.Hash;
	}

	int64 ResultBytes(const FGrungifyComponentResult& Result)
	{
		int64 Bytes = int64(Result.TexturePixels.Num() + Result.ExtendedPixels.Num()) * sizeof(FColor);
		for (const TArray<FColor>& LOD : Result.LODColors)
		{
			Bytes += int64(LOD.Num()) * sizeof(FColor);
		}
		Bytes += int64(Result.InstanceMasks.Num()) * sizeof(FVector4f);
		Bytes += int64(Result.ScatterSamples.Num()) * sizeof(FGrungifyComponentResult::FScatterSample);
		return Bytes + 1024;
	}

	FString SanitizeAssetName(const FString& Name)
	{
		FString Out;
		for (const TCHAR Ch : Name)
		{
			Out.AppendChar(FChar::IsAlnum(Ch) ? Ch : TEXT('_'));
		}
		return Out;
	}

	bool IsFireActor(const AActor* Actor)
	{
		const UGrungifySettings* Settings = UGrungifySettings::Get();
		if (Actor->ActorHasTag(Settings->FireTag) || Grungify::NameMatches(Actor->GetActorLabel(), Settings->FireKeywords))
		{
			return true;
		}
		TInlineComponentArray<UStaticMeshComponent*> Meshes(Actor);
		for (const UStaticMeshComponent* Mesh : Meshes)
		{
			if (Mesh->ComponentHasTag(Settings->FireTag) || (Mesh->GetStaticMesh() && Grungify::NameMatches(Mesh->GetStaticMesh()->GetName(), Settings->FireKeywords)))
			{
				return true;
			}
		}
		return false;
	}

	bool IsFastener(const UStaticMeshComponent* Component)
	{
		const UGrungifySettings* Settings = UGrungifySettings::Get();
		const AActor* Owner = Component->GetOwner();
		return Component->ComponentHasTag(Settings->FastenerTag) || (Owner && Owner->ActorHasTag(Settings->FastenerTag))
			|| (Component->GetStaticMesh() && Grungify::NameMatches(Component->GetStaticMesh()->GetName(), Settings->FastenerKeywords));
	}
}

void FGrungifyGenerationJob::GenerateItem(FItem& Item)
{
	if (Item.bGenerated)
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	bool bOk = false;
	if (UStaticMeshComponent* Component = Item.Component.Get())
	{
		if (Item.bInstanced)
		{
			bOk = Grungify::GenerateInstanced(Cast<UInstancedStaticMeshComponent>(Component), Item.Context, Params, Item.Result);
		}
		else
		{
			bOk = Grungify::GenerateComponent(Component, Item.Context, Params, Item.Result);
		}
	}
	Item.Seconds = float(FPlatformTime::Seconds() - Start);
	Item.bGenerated = !Item.Result.bCancelled;
	Item.bFailed = !bOk && !Item.Result.bCancelled;
}

void AGrungifyWeatheringZone::GatherScene(FGrungifyGenerationJob& Job, const FBox& ZoneBox)
{
	UWorld* World = GetWorld();
	FGrungifyGenerationParams& P = Job.Params;
	const FGrungifyExtendedLayers& E = Extended;

	// Lamps and fires: soot rises from them, so look below the zone as far as a plume reaches.
	if (E.SootIntensity > 0.f || E.ScorchIntensity > 0.f)
	{
		FBox Search = ZoneBox.ExpandBy(FVector(E.SootRadiusCm, E.SootRadiusCm, 0.0));
		Search.Min.Z -= E.SootPlumeHeightCm;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const AActor* Actor = *It;
			if (!Actor || Actor == this || Grungify::HasIgnoreTag(Actor, nullptr))
			{
				continue;
			}
			const FBox Box = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
			if (!Box.IsValid || !Box.Intersect(Search))
			{
				continue;
			}
			if (IsFireActor(Actor))
			{
				P.HeatSources.Add({ Box.GetCenter(), true });
				continue;
			}
			if (E.bSootFromLights && E.SootIntensity > 0.f)
			{
				TInlineComponentArray<ULocalLightComponent*> Lights(Actor);
				for (const ULocalLightComponent* Light : Lights)
				{
					if (Light && Light->IsVisible())
					{
						P.HeatSources.Add({ Light->GetComponentLocation(), false });
					}
				}
			}
		}
	}

	// Fasteners (bolts, rivets...), including instanced ones.
	if (E.RustBloomIntensity > 0.f)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const AActor* Actor = *It;
			if (!Actor || Actor == this || !Actor->GetComponentsBoundingBox(true).Intersect(ZoneBox))
			{
				continue;
			}
			TInlineComponentArray<UStaticMeshComponent*> Meshes(Actor);
			for (const UStaticMeshComponent* Mesh : Meshes)
			{
				if (!Mesh || !IsFastener(Mesh))
				{
					continue;
				}
				if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Mesh))
				{
					for (int32 Index = 0; Index < Instanced->GetInstanceCount() && P.Fasteners.Num() < 4096; ++Index)
					{
						FTransform Instance;
						if (Instanced->GetInstanceTransform(Index, Instance, /*bWorldSpace*/ true))
						{
							P.Fasteners.Add(Instance.GetLocation());
						}
					}
				}
				else if (Mesh->Bounds.BoxExtent.GetMax() <= 25.0)
				{
					P.Fasteners.Add(Mesh->Bounds.Origin);
				}
			}
		}
	}

	// Hand-placed stamps.
	if (bApplyWearStamps)
	{
		for (TActorIterator<AGrungifyWearStamp> It(World); It; ++It)
		{
			if (It->GetStampBounds().Intersect(ZoneBox))
			{
				FGrungifyStampData& Stamp = P.Stamps.AddDefaulted_GetRef();
				Stamp.Transform = It->GetActorTransform();
				Stamp.Radius = It->RadiusCm;
				Stamp.Falloff = It->Falloff;
				Stamp.EdgeNoise = It->EdgeNoise;
				Stamp.Layers = It->WearLayers;
			}
		}
	}

	// Traffic paths. The segment radius adds a 40 cm margin so walls beside the path see it.
	Job.Traffic.Reset(60.0);
	if (E.ScuffIntensity > 0.f)
	{
		TArray<FVector> Points;
		for (TActorIterator<AGrungifyTrafficPath> It(World); It; ++It)
		{
			It->SamplePoints(25.f, Points);
			for (int32 Index = 1; Index < Points.Num(); ++Index)
			{
				FGrungifySegmentField::FSegment Segment;
				Segment.A = Points[Index - 1];
				Segment.B = Points[Index];
				if (!ZoneBox.ExpandBy(It->WidthCm).IsInside(Segment.A))
				{
					continue;
				}
				Segment.Radius = It->WidthCm * 0.5f + 40.f;
				Segment.Value = It->Traffic;
				Job.Traffic.Add(Segment);
			}
		}
	}
	P.Traffic = Job.Traffic.IsEmpty() ? nullptr : &Job.Traffic;

	// Water: follow each source down the actual geometry.
	Job.Flow.Reset(40.0);
	if (bTraceWaterFlow)
	{
		const int32 MaxSteps = FMath::Clamp(FMath::CeilToInt((ZoneBox.GetSize().Z + 500.0) / FMath::Max(FlowStepCm, 2.f)) * 2, 10, 2000);
		for (FGrungifyRunoffSource& Source : CachedRunoffSources)
		{
			Grungify::TraceWaterPath(World, Source.Origin, FlowStepCm, MaxSteps, this, Source.Path, P.Pools);
			const float Length = FMath::Max(Source.Length > 0.f ? Source.Length : RunoffLengthCm, 10.f);
			float Travelled = 0.f;
			for (int32 Index = 1; Index < Source.Path.Num(); ++Index)
			{
				FGrungifySegmentField::FSegment Segment;
				Segment.A = Source.Path[Index - 1];
				Segment.B = Source.Path[Index];
				const float Step = float(FVector::Dist(Segment.A, Segment.B));
				Segment.AlongA = FMath::Clamp(Travelled / Length, 0.f, 1.f);
				Travelled += Step;
				Segment.AlongB = FMath::Clamp(Travelled / Length, 0.f, 1.f);
				Segment.Radius = Source.Radius * (1.f + RunoffSpread * Segment.AlongB);
				Segment.Value = Source.Strength;
				if (Segment.AlongA < 1.f)
				{
					Job.Flow.Add(Segment);
				}
			}
		}
	}
	P.Flow = Job.Flow.IsEmpty() ? nullptr : &Job.Flow;
	P.Sources = CachedRunoffSources;
}

bool AGrungifyWeatheringZone::PrepareJob(FGrungifyGenerationJob& Job, const TCHAR* Operation, const TFunction<bool()>& ShouldCancel)
{
	UWorld* World = GetWorld();
	const FBox ZoneBox = ZoneComponent->Bounds.GetBox();
	Job.Operation = Operation;

	LastReport = FGrungifyRunReport();
	LastReport.Operation = Operation;
	LastReport.Zone = GetActorNameOrLabel();
	LastReport.Seed = Seed;
	LastReport.Time = FDateTime::Now().ToIso8601();
	LastReport.Preset = Preset ? Preset->GetPathName() : FString();

	auto Cancelled = [this, &Job](const TCHAR* Phase)
	{
		Job.bCancelled = true;
		Job.CancelledDuring = Phase;
		LastReport.bCancelled = true;
		LastReport.CancelledDuring = Phase;
		return false;
	};
	auto CancelRequested = [&ShouldCancel]() { return ShouldCancel && ShouldCancel(); };

	{
		FGrungifyPhaseScope Phase(LastReport, TEXT("Floor traces"));
		if (FloorMode == EGrungifyFloorMode::Trace)
		{
			if (!Job.Floor.BuildTraced(World, ZoneBox, FloorProbeSpacingCm, this, ShouldCancel))
			{
				return Cancelled(TEXT("Floor traces"));
			}
			Phase.Items = Job.Floor.NumCells();
		}
		else
		{
			Job.Floor.BuildFlat(ZoneBox.Min.Z);
		}
	}

	if (bUseSkyExposure)
	{
		FGrungifyPhaseScope Phase(LastReport, TEXT("Sky exposure"));
		if (!Job.Exposure.Build(World, ZoneBox, SkyProbeSpacingCm, SkyRays, SkyRayLengthCm, this, ShouldCancel))
		{
			return Cancelled(TEXT("Sky exposure"));
		}
		Phase.Items = Job.Exposure.NumNodes();
	}

	BuildParams(Job.Params);
	if (FCString::Strcmp(Operation, TEXT("Bake")) == 0)
	{
		// Baked assets are textures; meshes without the mask UV channel still fall back to vertex colours.
		Job.Params.MaskOutput = EGrungifyMaskOutput::Texture;
	}
	Job.Params.Floor = &Job.Floor;
	Job.Params.Exposure = bUseSkyExposure ? &Job.Exposure : nullptr;
	Job.Params.SkyInfluence = bUseSkyExposure ? SkyExposureInfluence : 0.f;
	Job.Params.Extended = Extended;
	Job.Params.bExtended = Extended.UsesExtendedTexture();

	// Scatter only places things on Apply and Bake, so other runs don't pay for its samples.
	const bool bPlaces = FCString::Strcmp(Operation, TEXT("Apply")) == 0 || FCString::Strcmp(Operation, TEXT("Bake")) == 0;
	float MaxDensity = 0.f;
	for (const FGrungifyScatterEntry& Entry : Scatter)
	{
		if (Entry.bEnabled && (Entry.DecalMaterial || Entry.Mesh))
		{
			MaxDensity = FMath::Max(MaxDensity, Entry.DensityPerM2);
		}
	}
	Job.Params.ScatterDensity = bPlaces ? MaxDensity : 0.f;

	{
		FGrungifyPhaseScope Phase(LastReport, TEXT("Scene sources"));
		RefreshRunoffSources();
		GatherScene(Job, ZoneBox);
		Phase.Items = CachedRunoffSources.Num() + Job.Params.HeatSources.Num() + Job.Params.Fasteners.Num() + Job.Params.Stamps.Num() + Job.Params.Pools.Num();
	}
	LastReport.RunoffSources = CachedRunoffSources.Num();
	if (CancelRequested())
	{
		return Cancelled(TEXT("Scene sources"));
	}

	Job.EnvironmentHash = HashCombine(HashCombine(HashParams(Job.Params), Job.Floor.Hash()),
		HashCombine(HashCombine(Job.Exposure.Hash(), Job.Flow.Hash()), Job.Traffic.Hash()));

	auto Skip = [this](FGrungifyComponentReport& Report, const FString& Reason)
	{
		Report.Result = TEXT("skipped");
		Report.Notes = Reason;
		LastReport.Components.Add(MoveTemp(Report));
	};

	FGrungifyPhaseScope Phase(LastReport, TEXT("Scan"));

	// Which meshes other zones already own (applied) or are previewing.
	TMap<const UStaticMeshComponent*, AGrungifyWeatheringZone*> AppliedBy;
	TMap<const UStaticMeshComponent*, AGrungifyWeatheringZone*> PreviewedBy;
	for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
	{
		AGrungifyWeatheringZone* Other = *It;
		if (Other == this)
		{
			continue;
		}
		for (const FGrungifyComponentRecord& Record : Other->AppliedRecords)
		{
			if (const UStaticMeshComponent* Owned = Record.Component.Get())
			{
				AppliedBy.Add(Owned, Other);
			}
		}
		for (const FGrungifyComponentRecord& Record : Other->PreviewRecords)
		{
			if (const UStaticMeshComponent* Owned = Record.Component.Get())
			{
				PreviewedBy.Add(Owned, Other);
			}
		}
	}

	const double Now = FPlatformTime::Seconds();
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		FString ActorReason;
		if (!ShouldConsiderActor(Actor, ZoneBox, &ActorReason))
		{
			if (!ActorReason.IsEmpty())
			{
				FGrungifyComponentReport Report;
				Report.Actor = Actor->GetActorNameOrLabel();
				Report.Component = TEXT("(all components)");
				Skip(Report, ActorReason);
			}
			continue;
		}

		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (UStaticMeshComponent* Component : Components)
		{
			if (!Component || !Component->IsRegistered() || Component->IsEditorOnly() || !Component->GetStaticMesh()
				|| !Component->Bounds.GetBox().Intersect(ZoneBox))
			{
				continue;
			}

			FGrungifyComponentReport Report;
			Report.Actor = Actor->GetActorNameOrLabel();
			Report.Component = Component->GetName();
			Report.Mesh = Component->GetStaticMesh()->GetName();

			if (Grungify::HasIgnoreTag(nullptr, Component))
			{
				Skip(Report, FString::Printf(TEXT("component has the '%s' tag"), *UGrungifySettings::Get()->IgnoreTag.ToString()));
				continue;
			}
			if (RequiredComponentTags.Num() > 0 && !RequiredComponentTags.ContainsByPredicate([Component](const FName& Tag) { return Component->ComponentHasTag(Tag); }))
			{
				TArray<FString> TagNames;
				for (const FName& Tag : RequiredComponentTags)
				{
					TagNames.Add(Tag.ToString());
				}
				Skip(Report, FString::Printf(TEXT("component has none of the Required Component Tags (%s)"), *FString::Join(TagNames, TEXT(", "))));
				continue;
			}
			const bool bInstanced = Component->IsA<UInstancedStaticMeshComponent>();
			if (bInstanced && !bWeatherInstancedMeshes)
			{
				Skip(Report, TEXT("instanced mesh (enable Weather Instanced Meshes)"));
				continue;
			}
			if (!bIncludeMovable && Component->Mobility == EComponentMobility::Movable)
			{
				Skip(Report, TEXT("movable (enable Include Movable)"));
				continue;
			}
			if (AGrungifyWeatheringZone* const* Previewer = PreviewedBy.Find(Component))
			{
				Skip(Report, FString::Printf(TEXT("being previewed by %s"), *(*Previewer)->GetActorNameOrLabel()));
				continue;
			}
			AGrungifyWeatheringZone* TakeOverFrom = nullptr;
			if (AGrungifyWeatheringZone* const* AppliedOwner = AppliedBy.Find(Component))
			{
				Report.PreviousOwner = (*AppliedOwner)->GetActorNameOrLabel();
				// TakeOver is an Apply operation, never a temporary snapshot of another owner's layer.
				const bool bWrites = FCString::Strcmp(Operation, TEXT("Apply")) == 0 || FCString::Strcmp(Operation, TEXT("Bake")) == 0;
				if (OverlapPolicy == EGrungifyOverlapPolicy::SkipOwned || !bWrites)
				{
					Skip(Report, OverlapPolicy == EGrungifyOverlapPolicy::TakeOver
						? FString::Printf(TEXT("owned by %s (%s never takes over; use Take Over and Apply)"), *Report.PreviousOwner, Operation)
						: FString::Printf(TEXT("owned by %s (set Overlap Policy to Take Over)"), *Report.PreviousOwner));
					continue;
				}
				TakeOverFrom = *AppliedOwner;
			}

			FGrungifyGenerationJob::FItem& Item = Job.Items.AddDefaulted_GetRef();
			Item.Component = Component;
			Item.Path = Component->GetPathName();
			Item.bInstanced = bInstanced;
			Item.TakeOverFrom = TakeOverFrom;
			Item.Report = MoveTemp(Report);
			BuildContext(Component, Job.Samplers, Item.Context, Item.Report);

			// Workers must never read the zone's rule array; give each slot its own gate copy.
			for (FGrungifySlotInfo& Slot : Item.Context.Slots)
			{
				if (Slot.Gate)
				{
					Slot.Gate = Job.GateCopies.Add_GetRef(MakeUnique<FGrungifyGate>(*Slot.Gate)).Get();
				}
			}

			if (!Item.Context.Slots.ContainsByPredicate([](const FGrungifySlotInfo& Slot) { return !Slot.bExcluded; }))
			{
				Skip(Item.Report, TEXT("every material slot is excluded (see Slot Details for each reason)"));
				Job.Items.Pop();
				continue;
			}

			if (bUseGenerationCache)
			{
				Item.Key = HashCombine(Job.EnvironmentHash, HashComponent(Component, Item.Context));
				if (const TSharedPtr<FGrungifyCacheEntry>* Entry = GenerationCache.Find(Item.Path))
				{
					if ((*Entry)->Key == Item.Key && (*Entry)->Result.IsValid())
					{
						Item.Result = *(*Entry)->Result;
						Item.Result.Component = Component;
						Item.bGenerated = true;
						Item.bFromCache = true;
						(*Entry)->LastUsed = Now;
					}
				}
			}
		}
	}
	Phase.Items = Job.Items.Num();
	return true;
}

void AGrungifyWeatheringZone::CollectJob(FGrungifyGenerationJob& Job, TArray<FGrungifyComponentResult>& OutResults, TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*>& OutTakeOvers)
{
	const double Now = FPlatformTime::Seconds();
	for (FGrungifyGenerationJob::FItem& Item : Job.Items)
	{
		UStaticMeshComponent* Component = Item.Component.Get();
		FGrungifyComponentReport& Report = Item.Report;
		if (!Component || !Item.bGenerated || Item.bFailed)
		{
			Report.Result = TEXT("skipped");
			Report.Notes = !Component ? TEXT("component no longer exists") : TEXT("no readable mesh data");
			LastReport.Components.Add(MoveTemp(Report));
			continue;
		}

		FGrungifyComponentResult& Result = Item.Result;
		Result.Component = Component;
		Result.MaterialSurfaces.Reset();
		for (const FGrungifySlotInfo& Slot : Item.Context.Slots)
		{
			Result.MaterialSurfaces.Add(Slot.bExcluded ? EGrungifySurfaceType::Auto : Slot.Surface);
		}

		if (bUseGenerationCache && !Item.bFromCache && Item.Key != 0)
		{
			TSharedPtr<FGrungifyCacheEntry> Entry = MakeShared<FGrungifyCacheEntry>();
			Entry->Key = Item.Key;
			Entry->Result = MakeShared<FGrungifyComponentResult>(Result);
			Entry->Bytes = ResultBytes(Result);
			Entry->LastUsed = Now;
			GenerationCache.Add(Item.Path, Entry);
		}

		Report.Result = Result.bInstanced ? TEXT("instanced") : Result.Output == EGrungifyMaskOutput::Texture ? TEXT("texture") : TEXT("vertex");
		Report.Notes = Result.Notes.TrimEnd();
		Report.Vertices = Result.Vertices;
		Report.TextureSize = Result.TextureSize;
		Report.ExtendedTextureSize = Result.ExtendedPixels.Num() > 0 ? Result.TextureSize : 0;
		Report.Instances = Result.InstanceMasks.Num();
		Report.MeanMasks = Result.MeanMasks;
		Report.MaxMasks = Result.MaxMasks;
		Report.bNanite = Result.bNanite;
		Report.MaskUV = Result.MaskUV;
		Report.Seconds = Item.Seconds;
		Report.bFromCache = Item.bFromCache;
		LastReport.CacheHits += Item.bFromCache ? 1 : 0;

		if (AGrungifyWeatheringZone* AppliedOwner = Item.TakeOverFrom.Get())
		{
			OutTakeOvers.Add(Component, AppliedOwner);
		}
		Result.ReportIndex = LastReport.Components.Add(MoveTemp(Report));
		OutResults.Add(MoveTemp(Result));
	}
	TrimCache();
}

bool AGrungifyWeatheringZone::GenerateResults(const TCHAR* Operation, TArray<FGrungifyComponentResult>& OutResults, TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*>& OutTakeOvers)
{
	// Progress dialog with a Cancel button, shown only if the run takes noticeable time.
	// Cancellation is honoured at every phase boundary and inside long phases; nothing is written before this returns.
	FScopedSlowTask Task(100.f, FText::Format(LOCTEXT("Generating", "Grungify {0}: {1}"), FText::FromString(Operation), FText::FromString(GetActorNameOrLabel())));
	Task.MakeDialogDelayed(0.3f, /*bShowCancelButton*/ true);
	const TFunction<bool()> ShouldCancel = [&Task]() { return Task.ShouldCancel(); };

	FGrungifyGenerationJob Job;
	Task.EnterProgressFrame(20.f, LOCTEXT("PhasePrepare", "Tracing floors, sky and water; scanning meshes..."));
	if (!PrepareJob(Job, Operation, ShouldCancel))
	{
		return false;
	}
	Job.Params.ShouldCancel = ShouldCancel;

	// A later mesh can cancel the whole run before the write phase. Keep the diagnostics of the masks already
	// generated, without reporting them as weathered in either the rows or the aggregate.
	auto CancelDuringGeneration = [this, &Job]()
	{
		LastReport.bCancelled = true;
		LastReport.CancelledDuring = TEXT("Mask generation");
		for (FGrungifyGenerationJob::FItem& Item : Job.Items)
		{
			if (!Item.bGenerated || Item.bFailed || !Item.Component.IsValid())
			{
				continue;
			}
			FGrungifyComponentReport& Report = Item.Report;
			const FString Kind = Item.Result.bInstanced ? TEXT("instanced") : Item.Result.Output == EGrungifyMaskOutput::Texture ? TEXT("texture") : TEXT("vertex");
			Report.Result = TEXT("cancelled_before_write");
			Report.Notes = Item.Result.Notes.TrimEnd() + FString::Printf(TEXT(" Generated %s masks were discarded before writing."), *Kind);
			Report.Vertices = Item.Result.Vertices;
			Report.TextureSize = Item.Result.TextureSize;
			Report.MeanMasks = Item.Result.MeanMasks;
			Report.MaxMasks = Item.Result.MaxMasks;
			Report.bNanite = Item.Result.bNanite;
			Report.MaskUV = Item.Result.MaskUV;
			Report.Seconds = Item.Seconds;
			Report.bFromCache = Item.bFromCache;
			LastReport.Components.Add(MoveTemp(Report));
		}
		return false;
	};

	Task.EnterProgressFrame(80.f);
	{
		FGrungifyPhaseScope Phase(LastReport, TEXT("Mask generation"));
		FScopedSlowTask MeshTask(float(FMath::Max(Job.Items.Num(), 1)), LOCTEXT("PhaseGenerate", "Generating masks..."));
		for (int32 Index = 0; Index < Job.Items.Num(); ++Index)
		{
			FGrungifyGenerationJob::FItem& Item = Job.Items[Index];
			MeshTask.EnterProgressFrame(1.f, FText::Format(LOCTEXT("PhaseGenerateMesh", "Generating masks: {0} / {1}  ({2} of {3}){4}"),
				FText::FromString(Item.Report.Actor), FText::FromString(Item.Report.Component), Index + 1, Job.Items.Num(),
				Item.bFromCache ? LOCTEXT("Cached", "  [cached]") : FText::GetEmpty()));
			if (ShouldCancel())
			{
				return CancelDuringGeneration();
			}
			Job.GenerateItem(Item);
			if (Item.Result.bCancelled)
			{
				return CancelDuringGeneration();
			}
			Phase.Items += Item.bFromCache ? 0 : 1;
		}
	}

	CollectJob(Job, OutResults, OutTakeOvers);
	return true;
}

// ------------------------------------------------------- Background preview

void AGrungifyWeatheringZone::RequestLivePreview()
{
	if (!bLivePreview || !CanRunInEditorWorld())
	{
		return;
	}
	if (bBackgroundGeneration && !IsRunningCommandlet() && !GIsAutomationTesting)
	{
		StartBackgroundPreview();
	}
	else
	{
		Preview();
	}
}

void AGrungifyWeatheringZone::StartBackgroundPreview()
{
	CancelBackgroundJob();

	// Slot rules and classification read the live materials, so the old preview goes first (the scan is quick;
	// only mask generation runs on the worker).
	Modify();
	if (!RestorePreview())
	{
		return;
	}

	TSharedPtr<FGrungifyGenerationJob> Job = MakeShared<FGrungifyGenerationJob>();
	Job->StartSeconds = FPlatformTime::Seconds();
	if (!PrepareJob(*Job, TEXT("Preview"), TFunction<bool()>()))
	{
		return;
	}
	LastReport.bBackground = true;

	FGrungifyGenerationJob* Raw = Job.Get();
	Raw->Params.ShouldCancel = [Raw]() { return Raw->bCancelRequested.load(); };
	BackgroundJob = Job;
	// The zone keeps the job alive until the worker finishes (CancelBackgroundJob waits), so a raw pointer is safe.
	Raw->Future = Async(EAsyncExecution::ThreadPool, [Raw]()
	{
		for (FGrungifyGenerationJob::FItem& Item : Raw->Items)
		{
			if (Raw->bCancelRequested.load())
			{
				break;
			}
			// No garbage collection while a mesh is being read.
			FGCScopeGuard Guard;
			Raw->GenerateItem(Item);
		}
		Raw->bFinished = true;
	});

	const int32 Pending = Raw->Items.FilterByPredicate([](const FGrungifyGenerationJob::FItem& Item) { return !Item.bFromCache; }).Num();
	Status = FString::Printf(TEXT("Generating %d mesh(es) in the background (%d cached)..."), Pending, Raw->Items.Num() - Pending);
	BackgroundTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &AGrungifyWeatheringZone::TickBackgroundJob), 0.05f);
}

void AGrungifyWeatheringZone::CancelBackgroundJob()
{
	if (BackgroundTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(BackgroundTicker);
		BackgroundTicker.Reset();
	}
	if (BackgroundJob.IsValid())
	{
		BackgroundJob->bCancelRequested = true;
		if (BackgroundJob->Future.IsValid())
		{
			BackgroundJob->Future.Wait();
		}
		BackgroundJob.Reset();
	}
}

bool AGrungifyWeatheringZone::TickBackgroundJob(float DeltaTime)
{
	if (!BackgroundJob.IsValid())
	{
		return false;
	}
	if (!BackgroundJob->bFinished)
	{
		return true;
	}

	TSharedPtr<FGrungifyGenerationJob> Job = MoveTemp(BackgroundJob);
	BackgroundTicker.Reset();
	if (Job->Future.IsValid())
	{
		Job->Future.Wait();
	}
	if (Job->bCancelRequested.load() || !CanRunInEditorWorld())
	{
		return false;
	}

	TArray<FGrungifyComponentResult> Results;
	TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*> TakeOvers;
	CollectJob(*Job, Results, TakeOvers);
	Modify();
	if (RestorePreview())
	{
		// Something else may have previewed or applied these meshes while the worker ran.
		Results.RemoveAll([this](const FGrungifyComponentResult& Result)
		{
			if (!IsValid(Result.Component))
			{
				return true;
			}
			for (TActorIterator<AGrungifyWeatheringZone> It(GetWorld()); It; ++It)
			{
				if (*It != this && (It->IsPreviewing(Result.Component) || It->FindAppliedRecord(Result.Component)))
				{
					LastReport.Components[Result.ReportIndex].Result = TEXT("skipped");
					LastReport.Components[Result.ReportIndex].Notes = TEXT("another zone took this mesh while the background preview ran");
					return true;
				}
			}
			return false;
		});
		WritePreview(Results, Job->StartSeconds);
	}
	return false;
}

// ----------------------------------------------------------- Cache, variants

void AGrungifyWeatheringZone::TrimCache()
{
	int64 Total = 0;
	for (const TPair<FString, TSharedPtr<FGrungifyCacheEntry>>& Pair : GenerationCache)
	{
		Total += Pair.Value->Bytes;
	}
	const int64 Budget = int64(FMath::Max(CacheBudgetMB, 16)) * 1024 * 1024;
	while (Total > Budget && GenerationCache.Num() > 0)
	{
		FString Oldest;
		double OldestTime = TNumericLimits<double>::Max();
		for (const TPair<FString, TSharedPtr<FGrungifyCacheEntry>>& Pair : GenerationCache)
		{
			if (Pair.Value->LastUsed < OldestTime)
			{
				OldestTime = Pair.Value->LastUsed;
				Oldest = Pair.Key;
			}
		}
		Total -= GenerationCache[Oldest]->Bytes;
		GenerationCache.Remove(Oldest);
	}
}

// ----------------------------------------------------------------- Scatter

void AGrungifyWeatheringZone::ClearScatteredActors()
{
	UWorld* World = GetWorld();
	if (ScatteredActors.Num() == 0 || !World)
	{
		return;
	}
	Modify();
	for (const TSoftObjectPtr<AActor>& Scattered : ScatteredActors)
	{
		if (AActor* Actor = Scattered.Get())
		{
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
		}
	}
	ScatteredActors.Reset();
}

void AGrungifyWeatheringZone::ScatterFromResults(const TArray<FGrungifyComponentResult>& Results)
{
	UWorld* World = GetWorld();
	float MaxDensity = 0.f;
	for (const FGrungifyScatterEntry& Entry : Scatter)
	{
		if (Entry.bEnabled && (Entry.DecalMaterial || Entry.Mesh))
		{
			MaxDensity = FMath::Max(MaxDensity, Entry.DensityPerM2);
		}
	}
	if (!World || MaxDensity <= 0.f)
	{
		return;
	}

	constexpr int32 MaxPlacements = 2000;
	const FName Folder(*FString::Printf(TEXT("Grungify/%s/Scatter"), *GetActorNameOrLabel()));
	Modify();
	int32 Placed = 0;
	for (const FGrungifyComponentResult& Result : Results)
	{
		for (const FGrungifyComponentResult::FScatterSample& Sample : Result.ScatterSamples)
		{
			for (int32 EntryIndex = 0; EntryIndex < Scatter.Num() && Placed < MaxPlacements; ++EntryIndex)
			{
				const FGrungifyScatterEntry& Entry = Scatter[EntryIndex];
				if (!Entry.bEnabled || (!Entry.DecalMaterial && !Entry.Mesh) || (Entry.Surfaces.Num() > 0 && !Entry.Surfaces.Contains(Sample.Surface)))
				{
					continue;
				}
				const int32 Layer = int32(Entry.Layer);
				const FLinearColor& Masks = Layer < 4 ? Sample.Primary : Sample.Extended;
				const float Value = Layer % 4 == 0 ? Masks.R : Layer % 4 == 1 ? Masks.G : Layer % 4 == 2 ? Masks.B : Masks.A;
				// Each entry thins the shared candidates independently, but deterministically.
				const float Pick = FMath::Frac(Sample.Random * 7.31f + EntryIndex * 0.618034f);
				if (Value < Entry.Threshold || Pick > Entry.DensityPerM2 / MaxDensity)
				{
					continue;
				}

				const float Size = FMath::Lerp(float(Entry.SizeRangeCm.X), float(Entry.SizeRangeCm.Y), Sample.Random2);
				const float Spin = FMath::Frac(Sample.Random2 * 13.7f + EntryIndex * 0.37f) * 360.f;
				FActorSpawnParameters SpawnParams;
				SpawnParams.OverrideLevel = GetLevel();
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

				AActor* Spawned = nullptr;
				if (Entry.DecalMaterial)
				{
					// Decals project along their X axis, so point X into the surface and spin around it.
					const FQuat Facing = FRotationMatrix::MakeFromX(-Sample.N).ToQuat();
					const FQuat Rotation = FQuat(-Sample.N, FMath::DegreesToRadians(Spin)) * Facing;
					ADecalActor* Decal = World->SpawnActor<ADecalActor>(Sample.P, Rotation.Rotator(), SpawnParams);
					if (Decal)
					{
						Decal->SetDecalMaterial(Entry.DecalMaterial);
						Decal->GetDecal()->DecalSize = FVector(Entry.DecalDepthCm, Size * 0.5f, Size * 0.5f);
						Spawned = Decal;
					}
				}
				else
				{
					const FQuat Facing = FRotationMatrix::MakeFromZ(Sample.N).ToQuat();
					const FQuat Rotation = FQuat(Sample.N, FMath::DegreesToRadians(Spin)) * Facing;
					AStaticMeshActor* MeshActor = World->SpawnActor<AStaticMeshActor>(Sample.P, Rotation.Rotator(), SpawnParams);
					if (MeshActor)
					{
						MeshActor->GetStaticMeshComponent()->SetStaticMesh(Entry.Mesh);
						const float MeshSize = float(FMath::Max(Entry.Mesh->GetBounds().BoxExtent.GetMax() * 2.0, 1.0));
						MeshActor->SetActorScale3D(FVector(Size / MeshSize));
						Spawned = MeshActor;
					}
				}
				if (!Spawned)
				{
					continue;
				}

				Spawned->SetFolderPath(Folder);
				Spawned->SetActorLabel(FString::Printf(TEXT("%s_Scatter_%d"), *GetActorNameOrLabel(), Placed));
				Spawned->Tags.Add(TEXT("Grungify.Scatter"));
				// Scattered pieces must never be weathered (or counted) by zones themselves.
				Spawned->Tags.Add(UGrungifySettings::Get()->IgnoreTag);
				for (const UDataLayerAsset* DataLayer : ScatterDataLayers)
				{
					if (DataLayer)
					{
						const UDataLayerManager* LayerManager = UDataLayerManager::GetDataLayerManager(World);
						const UDataLayerInstance* LayerInstance = LayerManager ? LayerManager->GetDataLayerInstanceFromAsset(DataLayer) : nullptr;
						if (!LayerInstance || !Spawned->AddDataLayer(LayerInstance))
						{
							LastReport.Warnings.AddUnique(FString::Printf(TEXT("Could not assign scatter to Data Layer %s; no assignable instance in this world."), *DataLayer->GetPathName()));
						}
					}
				}
				ScatteredActors.Add(Spawned);
				++Placed;
			}
		}
	}
	LastReport.Scattered = Placed;
	if (Placed >= MaxPlacements)
	{
		LastReport.Warnings.Add(FString::Printf(TEXT("Scatter stopped at %d placements; lower densities or raise thresholds."), MaxPlacements));
	}
}

bool AGrungifyWeatheringZone::WriteResult(const FGrungifyComponentResult& Result, EGrungifyPreviewDisplay Display, const FGrungifyComponentRecord& OriginalState)
{
	UStaticMeshComponent* Component = Result.Component;
	FGrungifyComponentReport& Entry = LastReport.Components[Result.ReportIndex];
	auto Fail = [&Entry](const TCHAR* Reason)
	{
		Entry.Result = TEXT("failed");
		Entry.Notes += FString(TEXT(" ")) + Reason;
		return false;
	};

	// Never overwrite custom data another system put on this mesh. Without our slots the material path stays
	// inactive, so don't write masks either.
	if (!Grungify::CustomDataSlotsFree(OriginalState.CustomData,
		OriginalState.bHasLiveCustomData ? OriginalState.LiveCustomData : OriginalState.CustomData,
		UGrungifySettings::Get()->CustomPrimitiveDataIndex))
	{
		Entry.bCustomDataConflict = true;
		Entry.Result = TEXT("skipped");
		return false;
	}
	UInstancedStaticMeshComponent* Instanced = Result.bInstanced ? Cast<UInstancedStaticMeshComponent>(Component) : nullptr;
	if (Result.bInstanced && (!Instanced || !Grungify::InstanceSlotsFree(OriginalState)))
	{
		// Same rule for per-instance custom data.
		const int32 Base = UGrungifySettings::Get()->InstanceCustomDataIndex;
		Entry.bCustomDataConflict = true;
		Entry.Result = TEXT("skipped");
		Entry.Notes += FString::Printf(TEXT(" per-instance custom data %d-%d is already in use (change Instance Custom Data Index)"), Base, Base + Grungify::InstanceDataCount - 1);
		return false;
	}

	Component->SetFlags(RF_Transactional);
	Component->Modify();

	UMaterialInterface* DebugMaterial = nullptr;
	if (Display != EGrungifyPreviewDisplay::FinalMaterial)
	{
		DebugMaterial = UGrungifySettings::Get()->MaskDebugMaterial.LoadSynchronous();
		if (!DebugMaterial)
		{
			UE_LOG(LogGrungify, Warning, TEXT("Mask debug material not found. Run Tools > Grungify > Rebuild Plugin Content."));
			return Fail(TEXT("requested debug material is unavailable"));
		}
	}

	const bool bTexture = !Result.bInstanced && Result.Output == EGrungifyMaskOutput::Texture;
	if (Instanced)
	{
		if (!Grungify::WriteInstanceMasks(Instanced, Result.InstanceMasks)
			|| !Grungify::BindOwnedMaterials(Component, nullptr, Result.MaterialSurfaces, DebugMaterial, &Palette))
		{
			return Fail(TEXT("per-instance masks or admitted material bindings could not be written"));
		}
	}
	else if (bTexture && bBakingToAssets)
	{
		if (!WriteBakedAssets(Result))
		{
			return Fail(TEXT("baked texture or material assets could not be created"));
		}
	}
	else if (bTexture)
	{
		UTexture2D* Texture = Grungify::CreateMaskTexture(Component, Result.TextureSize, Result.TexturePixels);
		UTexture2D* ExtendedTexture = Result.ExtendedPixels.Num() > 0 ? Grungify::CreateMaskTexture(Component, Result.TextureSize, Result.ExtendedPixels) : nullptr;
		if (!Texture || (Result.ExtendedPixels.Num() > 0 && !ExtendedTexture)
			|| !Grungify::BindOwnedMaterials(Component, Texture, Result.MaterialSurfaces, DebugMaterial, &Palette, ExtendedTexture, Result.TexelSizeCm))
		{
			return Fail(TEXT("texture or admitted material bindings could not be written"));
		}
	}
	else
	{
		if (!Grungify::WriteVertexColors(Component, Result.LODColors, &Result.MaterialSurfaces)
			|| !Grungify::BindOwnedMaterials(Component, nullptr, Result.MaterialSurfaces, DebugMaterial, &Palette))
		{
			return Fail(TEXT("vertex masks or admitted material bindings could not be written"));
		}
		if (bBakingToAssets)
		{
			Entry.Notes += TEXT(" baked as vertex colours (no mask UV channel), so no texture asset was created");
		}
	}

	float DebugChannel = 0.f;
	switch (Display)
	{
	case EGrungifyPreviewDisplay::EdgeChips:   DebugChannel = 1.f; break;
	case EGrungifyPreviewDisplay::Dust:        DebugChannel = 2.f; break;
	case EGrungifyPreviewDisplay::Runoff:      DebugChannel = 3.f; break;
	case EGrungifyPreviewDisplay::Handling:    DebugChannel = 4.f; break;
	case EGrungifyPreviewDisplay::Moisture:    DebugChannel = 5.f; break;
	case EGrungifyPreviewDisplay::Soot:        DebugChannel = 6.f; break;
	case EGrungifyPreviewDisplay::Scorch:      DebugChannel = 7.f; break;
	case EGrungifyPreviewDisplay::Moss:        DebugChannel = 8.f; break;
	case EGrungifyPreviewDisplay::AllExtended: DebugChannel = 9.f; break;
	default: break;
	}

	float CustomData[Grungify::CPD_Count];
	CustomData[Grungify::CPD_MaskMode] = Instanced ? Grungify::MaskModeInstanced : bTexture ? Grungify::MaskModeTexture : Grungify::MaskModeVertex;
	CustomData[Grungify::CPD_SurfaceType] = float(int32(Result.DominantSurface) - 1);
	CustomData[Grungify::CPD_FeatureScale] = FeatureScaleCm;
	// Scramble the seed into a stable 0..1000 offset so the shader detail changes with the seed too.
	CustomData[Grungify::CPD_SeedOffset] = float((uint32(Seed) * 2654435761u) % 100000u) * 0.01f;
	CustomData[Grungify::CPD_DebugChannel] = DebugChannel;
	if (!Grungify::WriteCustomData(Component, CustomData))
	{
		return Fail(TEXT("custom primitive data arrays could not be written"));
	}

	Component->MarkRenderStateDirty();
	return true;
}

bool AGrungifyWeatheringZone::WriteBakedAssets(const FGrungifyComponentResult& Result)
{
	UStaticMeshComponent* Component = Result.Component;
	FString Folder = BakeFolder.Path.IsEmpty() ? FString(TEXT("/Game/Grungify/Baked")) : BakeFolder.Path;
	Folder.RemoveFromEnd(TEXT("/"));
	const AActor* ComponentOwner = Component->GetOwner();
	const FString Base = SanitizeAssetName(FString::Printf(TEXT("%s_%s_%s"), *GetActorNameOrLabel(),
		ComponentOwner ? *ComponentOwner->GetActorNameOrLabel() : TEXT("NoOwner"), *Component->GetName()));
	const FString PngDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"), TEXT("Bakes"), SanitizeAssetName(GetActorNameOrLabel()));

	// Re-baking updates the same assets, so materials and levels that reference them stay valid.
	auto FindOrCreate = [this, &Folder](UClass* Class, const FString& Name) -> UObject*
	{
		const FString PackageName = Folder / Name;
		FText Error;
		if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ false, &Error))
		{
			UE_LOG(LogGrungify, Warning, TEXT("%s: '%s' is not a valid content path (%s)."), *GetActorNameOrLabel(), *PackageName, *Error.ToString());
			return nullptr;
		}
		UObject* Asset = StaticLoadObject(Class, nullptr, *(PackageName + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Asset)
		{
			UPackage* Package = CreatePackage(*PackageName);
			Asset = NewObject<UObject>(Package, Class, *Name, RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Asset);
		}
		Asset->Modify();
		BakedPackages.AddUnique(Asset->GetPackage());
		LastReport.BakedAssets.AddUnique(PackageName);
		return Asset;
	};

	auto BakeTexture = [&](const FString& Name, const TArray<FColor>& Pixels) -> UTexture2D*
	{
		UTexture2D* Texture = Cast<UTexture2D>(FindOrCreate(UTexture2D::StaticClass(), Name));
		if (!Texture || Pixels.Num() != Result.TextureSize * Result.TextureSize)
		{
			return nullptr;
		}
		Texture->Source.Init(Result.TextureSize, Result.TextureSize, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Default;
		Texture->LODGroup = TEXTUREGROUP_World;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;

		if (bExportBakedPNGs)
		{
			// Linear data: paint over it in any editor and Reimport the texture to pick the changes up.
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(Result.TextureSize, Result.TextureSize, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
			const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(PngDirectory, Name + TEXT(".png")));
			if (IFileManager::Get().MakeDirectory(*PngDirectory, true) && FFileHelper::SaveArrayToFile(Png, *File))
			{
				if (!Texture->AssetImportData)
				{
					Texture->AssetImportData = NewObject<UAssetImportData>(Texture, TEXT("AssetImportData"));
				}
				Texture->AssetImportData->Update(File);
			}
			else
			{
				LastReport.Warnings.Add(FString::Printf(TEXT("Could not export %s"), *File));
			}
		}
		Texture->PostEditChange();
		Texture->MarkPackageDirty();
		return Texture;
	};

	UTexture2D* Texture = BakeTexture(TEXT("T_Grungify_") + Base, Result.TexturePixels);
	UTexture2D* ExtendedTexture = Result.ExtendedPixels.Num() > 0 ? BakeTexture(TEXT("T_Grungify_") + Base + TEXT("_Ext"), Result.ExtendedPixels) : nullptr;
	if (!Texture || (Result.ExtendedPixels.Num() > 0 && !ExtendedTexture) || Result.MaterialSurfaces.Num() != Component->GetNumMaterials())
	{
		return false;
	}

	for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
	{
		UMaterialInterface* Parent = Component->GetMaterial(Slot);
		if (Result.MaterialSurfaces[Slot] == EGrungifySurfaceType::Auto || !Parent)
		{
			continue;
		}
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(FindOrCreate(UMaterialInstanceConstant::StaticClass(),
			FString::Printf(TEXT("MI_Grungify_%s_%d"), *Base, Slot)));
		if (!Instance || Instance == Parent)
		{
			return false;
		}
		Grungify::ConfigureOwnedInstance(Instance, Parent, Texture, ExtendedTexture, Result.MaterialSurfaces[Slot], &Palette, Result.TexelSizeCm);
		Instance->MarkPackageDirty();
		Component->SetMaterial(Slot, Instance);
	}
	return true;
}

void AGrungifyWeatheringZone::FinishReport(double StartSeconds)
{
	LastReport.Seconds = float(FPlatformTime::Seconds() - StartSeconds);

	int32 NumVertex = 0;
	int32 NumTexture = 0;
	int32 NumInstanced = 0;
	int32 NumConflicts = 0;
	for (const FGrungifyComponentReport& Entry : LastReport.Components)
	{
		NumVertex += Entry.Result == TEXT("vertex") ? 1 : 0;
		NumTexture += Entry.Result == TEXT("texture") ? 1 : 0;
		NumInstanced += Entry.Result == TEXT("instanced") ? 1 : 0;
		NumConflicts += Entry.bCustomDataConflict ? 1 : 0;
	}
	LastReport.Weathered = NumVertex + NumTexture + NumInstanced;
	LastReport.Skipped = LastReport.Components.Num() - LastReport.Weathered;
	if (NumConflicts > 0)
	{
		const int32 Base = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
		LastReport.Warnings.Add(FString::Printf(TEXT("%d mesh(es) already use custom primitive data %d-%d; they were left untouched. Change Project Settings > Plugins > Grungify > Custom Primitive Data Index."),
			NumConflicts, Base, Base + Grungify::CPD_Count - 1));
	}

	TArray<FString> PhaseText;
	for (const FGrungifyPhaseTiming& Timing : LastReport.Phases)
	{
		PhaseText.Add(FString::Printf(TEXT("%s %.2fs"), *Timing.Phase, Timing.Seconds));
	}
	const FString Phases = PhaseText.Num() > 0 ? FString::Printf(TEXT(" [%s]"), *FString::Join(PhaseText, TEXT(", "))) : FString();

	if (LastReport.bCancelled)
	{
		// Cancellation only ever happens before the write phase, so no mesh received new masks.
		const TCHAR* Consequence = LastReport.Operation == TEXT("Apply") || LastReport.Operation == TEXT("Bake")
			? TEXT("No meshes were weathered and applied meshes are unchanged; any active preview was removed.")
			: LastReport.Operation == TEXT("Preview")
				? TEXT("No meshes were written; the previous preview was removed.")
				: TEXT("Nothing was changed.");
		Status = FString::Printf(TEXT("%s cancelled during %s after %.2fs. %s%s"),
			*LastReport.Operation, *LastReport.CancelledDuring, LastReport.Seconds, Consequence, *Phases);
	}
	else
	{
		const TCHAR* Verb = LastReport.Operation == TEXT("Apply") ? TEXT("Applied") : LastReport.Operation == TEXT("Bake") ? TEXT("Baked")
			: LastReport.Operation == TEXT("Preview") ? TEXT("Previewing") : TEXT("Dry run");
		FString Extras;
		if (NumInstanced > 0)
		{
			Extras += FString::Printf(TEXT(", %d instanced"), NumInstanced);
		}
		if (LastReport.CacheHits > 0)
		{
			Extras += FString::Printf(TEXT(", %d from cache"), LastReport.CacheHits);
		}
		if (LastReport.Scattered > 0)
		{
			Extras += FString::Printf(TEXT(", %d scattered"), LastReport.Scattered);
		}
		Status = FString::Printf(TEXT("%s: %d mesh(es) (%d vertex, %d texture%s), %d skipped, %d runoff source(s), seed %d, %.2fs%s%s. See Last Report."),
			Verb, LastReport.Weathered, NumVertex, NumTexture, *Extras, LastReport.Skipped, LastReport.RunoffSources, Seed, LastReport.Seconds,
			LastReport.bBackground ? TEXT(" (background)") : TEXT(""), *Phases);
	}
	UE_LOG(LogGrungify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
	for (const FString& Warning : LastReport.Warnings)
	{
		UE_LOG(LogGrungify, Warning, TEXT("%s: %s"), *GetActorNameOrLabel(), *Warning);
	}

	if (bWriteReportFiles)
	{
		auto ReportFailure = [this](const FString& Message)
		{
			LastReport.Warnings.Add(Message);
			Status += TEXT(" Report file was not written; see Last Report.");
			UE_LOG(LogGrungify, Warning, TEXT("%s: %s"), *GetActorNameOrLabel(), *Message);
		};
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"), TEXT("Reports"));
		if (!IFileManager::Get().MakeDirectory(*Directory, true))
		{
			ReportFailure(FString::Printf(TEXT("Report directory could not be created: %s"), *Directory));
			return;
		}
		FString Json;
		if (!FJsonObjectConverter::UStructToJsonObjectString(LastReport, Json))
		{
			ReportFailure(TEXT("Report could not be serialized to JSON."));
			return;
		}
		const FString File = FPaths::Combine(Directory,
			FPaths::MakeValidFileName(FString::Printf(TEXT("%s_%s.json"), *GetActorNameOrLabel(), *LastReport.Operation)));
		if (!FFileHelper::SaveStringToFile(Json, *File))
		{
			ReportFailure(FString::Printf(TEXT("Report could not be saved to %s"), *File));
		}
		else
		{
			LastReportFile = File;
		}
	}
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
