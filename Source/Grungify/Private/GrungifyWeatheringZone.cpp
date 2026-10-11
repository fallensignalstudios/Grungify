#include "GrungifyWeatheringZone.h"

#include "Components/ArrowComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GrungifyZoneComponent.h"

#if WITH_EDITOR
#include "Components/InstancedStaticMeshComponent.h"
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

	// Writing is quick and must not stop halfway, so this dialog has no Cancel button.
	FGrungifyPhaseScope WritePhase(LastReport, TEXT("Write"));
	FScopedSlowTask WriteTask(float(FMath::Max(Results.Num(), 1)), LOCTEXT("WritingPreview", "Grungify: writing preview..."));
	WriteTask.MakeDialogDelayed(0.3f, /*bShowCancelButton*/ false);
	for (const FGrungifyComponentResult& Result : Results)
	{
		WriteTask.EnterProgressFrame(1.f);
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
	FinishReport(Start);
#endif
}

void AGrungifyWeatheringZone::Apply()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	const double Start = FPlatformTime::Seconds();
	const FScopedTransaction Transaction(LOCTEXT("ApplyWeathering", "Grungify: Apply Weathering"));
	Modify();
	if (!RestorePreview() || !CancelDependentPreviews())
	{
		return;
	}

	TArray<FGrungifyComponentResult> Results;
	TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*> TakeOvers;
	if (!GenerateResults(TEXT("Apply"), Results, TakeOvers))
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

	for (const FGrungifyComponentResult& Result : Results)
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
	}

	WritePhase.Finish();
	FinishReport(Start);
#endif
}

void AGrungifyWeatheringZone::Clear()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("ClearWeathering", "Grungify: Clear Weathering"));
	Modify();
	if (!RestorePreview() || !CancelDependentPreviews())
	{
		return;
	}

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

void AGrungifyWeatheringZone::DryRun()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

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
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, bWriteReportFiles))
	{
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
		Preview();
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
	struct FGrungifyCandidate
	{
		UStaticMeshComponent* Component = nullptr;
		FGrungifyComponentReport Report;
	};
}

bool AGrungifyWeatheringZone::GenerateResults(const TCHAR* Operation, TArray<FGrungifyComponentResult>& OutResults, TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*>& OutTakeOvers)
{
	UWorld* World = GetWorld();
	const FBox ZoneBox = ZoneComponent->Bounds.GetBox();

	LastReport = FGrungifyRunReport();
	LastReport.Operation = Operation;
	LastReport.Zone = GetActorNameOrLabel();
	LastReport.Seed = Seed;
	LastReport.Time = FDateTime::Now().ToIso8601();
	LastReport.Preset = Preset ? Preset->GetPathName() : FString();

	// Progress dialog with a Cancel button, shown only if the run takes noticeable time.
	// Cancellation is honoured at every phase boundary and inside long phases; nothing is written before this returns.
	FScopedSlowTask Task(100.f, FText::Format(LOCTEXT("Generating", "Grungify {0}: {1}"), FText::FromString(Operation), FText::FromString(GetActorNameOrLabel())));
	Task.MakeDialogDelayed(0.3f, /*bShowCancelButton*/ true);
	const TFunction<bool()> ShouldCancel = [&Task]() { return Task.ShouldCancel(); };
	auto Cancelled = [this](const TCHAR* Phase)
	{
		LastReport.bCancelled = true;
		LastReport.CancelledDuring = Phase;
		// These rows describe generated CPU masks, not writes. A later mesh can
		// cancel the whole run before the write phase, so retain their diagnostics
		// without reporting them as weathered in either the rows or aggregate.
		for (FGrungifyComponentReport& Entry : LastReport.Components)
		{
			if (Entry.Result == TEXT("texture") || Entry.Result == TEXT("vertex"))
			{
				Entry.Notes += FString::Printf(TEXT(" Generated %s masks were discarded before writing."), *Entry.Result);
				Entry.Result = TEXT("cancelled_before_write");
			}
		}
		return false;
	};

	FGrungifyFloorField Floor;
	Task.EnterProgressFrame(15.f, LOCTEXT("PhaseFloors", "Tracing floors..."));
	{
		FGrungifyPhaseScope Phase(LastReport, TEXT("Floor traces"));
		if (FloorMode == EGrungifyFloorMode::Trace)
		{
			if (!Floor.BuildTraced(World, ZoneBox, FloorProbeSpacingCm, this, ShouldCancel))
			{
				return Cancelled(TEXT("Floor traces"));
			}
			Phase.Items = Floor.NumCells();
		}
		else
		{
			Floor.BuildFlat(ZoneBox.Min.Z);
		}
	}

	Task.EnterProgressFrame(5.f, LOCTEXT("PhaseSources", "Finding runoff sources..."));
	{
		FGrungifyPhaseScope Phase(LastReport, TEXT("Runoff sources"));
		RefreshRunoffSources();
		Phase.Items = CachedRunoffSources.Num();
	}
	LastReport.RunoffSources = CachedRunoffSources.Num();
	if (ShouldCancel())
	{
		return Cancelled(TEXT("Runoff sources"));
	}

	FGrungifyGenerationParams Params;
	BuildParams(Params);
	Params.Floor = &Floor;
	Params.Sources = CachedRunoffSources;
	Params.ShouldCancel = ShouldCancel;

	auto Skip = [this](FGrungifyComponentReport& Report, const FString& Reason)
	{
		Report.Result = TEXT("skipped");
		Report.Notes = Reason;
		LastReport.Components.Add(MoveTemp(Report));
	};

	// Scan: decide which components are candidates (and report why the others in the zone aren't).
	Task.EnterProgressFrame(5.f, LOCTEXT("PhaseScan", "Scanning meshes..."));
	TArray<FGrungifyCandidate> Candidates;
	{
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
				// One set of vertex colours and materials is shared by every instance.
				if (Component->IsA<UInstancedStaticMeshComponent>())
				{
					Skip(Report, TEXT("instanced mesh (shared by all instances)"));
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
				if (AGrungifyWeatheringZone* const* AppliedOwner = AppliedBy.Find(Component))
				{
					Report.PreviousOwner = (*AppliedOwner)->GetActorNameOrLabel();
					// TakeOver is an Apply operation, never a temporary snapshot of another owner's layer.
					if (OverlapPolicy == EGrungifyOverlapPolicy::SkipOwned || FCString::Strcmp(Operation, TEXT("Preview")) == 0)
					{
						Skip(Report, FCString::Strcmp(Operation, TEXT("Preview")) == 0
							? FString::Printf(TEXT("owned by %s (Preview never takes over; use Take Over and Apply)"), *Report.PreviousOwner)
							: FString::Printf(TEXT("owned by %s (set Overlap Policy to Take Over)"), *Report.PreviousOwner));
						continue;
					}
					OutTakeOvers.Add(Component, *AppliedOwner);
				}

				FGrungifyCandidate& Candidate = Candidates.AddDefaulted_GetRef();
				Candidate.Component = Component;
				Candidate.Report = MoveTemp(Report);
			}
		}
		Phase.Items = Candidates.Num();
	}
	if (ShouldCancel())
	{
		return Cancelled(TEXT("Scan"));
	}

	// Generate: the expensive part, one progress step per mesh.
	Task.EnterProgressFrame(75.f);
	{
		FGrungifyPhaseScope Phase(LastReport, TEXT("Mask generation"));
		FScopedSlowTask MeshTask(float(FMath::Max(Candidates.Num(), 1)), LOCTEXT("PhaseGenerate", "Generating masks..."));
		TMap<UTexture2D*, TUniquePtr<FGrungifyGateSampler>> Samplers;

		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			FGrungifyCandidate& Candidate = Candidates[Index];
			UStaticMeshComponent* Component = Candidate.Component;
			FGrungifyComponentReport& Report = Candidate.Report;
			MeshTask.EnterProgressFrame(1.f, FText::Format(LOCTEXT("PhaseGenerateMesh", "Generating masks: {0} / {1}  ({2} of {3})"),
				FText::FromString(Report.Actor), FText::FromString(Report.Component), Index + 1, Candidates.Num()));
			if (ShouldCancel())
			{
				return Cancelled(TEXT("Mask generation"));
			}

			const double MeshStart = FPlatformTime::Seconds();
			FGrungifyComponentContext Context;
			BuildContext(Component, Samplers, Context, Report);
			if (!Context.Slots.ContainsByPredicate([](const FGrungifySlotInfo& Slot) { return !Slot.bExcluded; }))
			{
				OutTakeOvers.Remove(Component);
				Skip(Report, TEXT("every material slot is excluded (see Slot Details for each reason)"));
				continue;
			}

			FGrungifyComponentResult Result;
			const bool bGenerated = Grungify::GenerateComponent(Component, Context, Params, Result);
			Report.Seconds = float(FPlatformTime::Seconds() - MeshStart);
			Report.MaskUV = Result.MaskUV;
			if (Result.bCancelled)
			{
				return Cancelled(TEXT("Mask generation"));
			}
			if (!bGenerated)
			{
				OutTakeOvers.Remove(Component);
				Skip(Report, TEXT("no readable mesh data"));
				continue;
			}
			++Phase.Items;

			Report.Result = Result.Output == EGrungifyMaskOutput::Texture ? TEXT("texture") : TEXT("vertex");
			Report.Notes = Result.Notes.TrimEnd();
			Report.Vertices = Result.Vertices;
			Report.TextureSize = Result.TextureSize;
			Report.MeanMasks = Result.MeanMasks;
			Report.MaxMasks = Result.MaxMasks;
			Report.bNanite = Result.bNanite;
			for (const FGrungifySlotInfo& Slot : Context.Slots)
			{
				Result.MaterialSurfaces.Add(Slot.bExcluded ? EGrungifySurfaceType::Auto : Slot.Surface);
			}

			Result.ReportIndex = LastReport.Components.Add(MoveTemp(Report));
			OutResults.Add(MoveTemp(Result));
		}
	}
	return true;
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

	const bool bTexture = Result.Output == EGrungifyMaskOutput::Texture;
	if (bTexture)
	{
		UTexture2D* Texture = Grungify::CreateMaskTexture(Component, Result.TextureSize, Result.TexturePixels);
		if (!Texture || !Grungify::BindOwnedMaterials(Component, Texture, Result.MaterialSurfaces, DebugMaterial, &Palette))
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
	}

	float DebugChannel = 0.f;
	switch (Display)
	{
	case EGrungifyPreviewDisplay::EdgeChips: DebugChannel = 1.f; break;
	case EGrungifyPreviewDisplay::Dust:      DebugChannel = 2.f; break;
	case EGrungifyPreviewDisplay::Runoff:    DebugChannel = 3.f; break;
	case EGrungifyPreviewDisplay::Handling:  DebugChannel = 4.f; break;
	default: break;
	}

	float CustomData[Grungify::CPD_Count];
	CustomData[Grungify::CPD_MaskMode] = bTexture ? Grungify::MaskModeTexture : Grungify::MaskModeVertex;
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

void AGrungifyWeatheringZone::FinishReport(double StartSeconds)
{
	LastReport.Seconds = float(FPlatformTime::Seconds() - StartSeconds);

	int32 NumVertex = 0;
	int32 NumTexture = 0;
	int32 NumConflicts = 0;
	for (const FGrungifyComponentReport& Entry : LastReport.Components)
	{
		NumVertex += Entry.Result == TEXT("vertex") ? 1 : 0;
		NumTexture += Entry.Result == TEXT("texture") ? 1 : 0;
		NumConflicts += Entry.bCustomDataConflict ? 1 : 0;
	}
	LastReport.Weathered = NumVertex + NumTexture;
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
		const TCHAR* Consequence = LastReport.Operation == TEXT("Apply")
			? TEXT("No meshes were weathered and applied meshes are unchanged; any active preview was removed.")
			: LastReport.Operation == TEXT("Preview")
				? TEXT("No meshes were written; the previous preview was removed.")
				: TEXT("Nothing was changed.");
		Status = FString::Printf(TEXT("%s cancelled during %s after %.2fs. %s%s"),
			*LastReport.Operation, *LastReport.CancelledDuring, LastReport.Seconds, Consequence, *Phases);
	}
	else
	{
		const TCHAR* Verb = LastReport.Operation == TEXT("Apply") ? TEXT("Applied") : LastReport.Operation == TEXT("Preview") ? TEXT("Previewing") : TEXT("Dry run");
		Status = FString::Printf(TEXT("%s: %d mesh(es) (%d vertex, %d texture), %d skipped, %d runoff source(s), seed %d, %.2fs%s. See Last Report."),
			Verb, LastReport.Weathered, NumVertex, NumTexture, LastReport.Skipped, LastReport.RunoffSources, Seed, LastReport.Seconds, *Phases);
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
	}
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
