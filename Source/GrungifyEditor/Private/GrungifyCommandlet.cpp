// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyCommandlet.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h" // UEditorLoadingAndSavingUtils
#include "GrungifyBlueprintLibrary.h"
#include "GrungifyWeatheringZone.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogGrungifyCommandlet, Log, All);

#define LOCTEXT_NAMESPACE "GrungifyCommandlet"

namespace
{
	enum EGrungifyExitCode : int32
	{
		ExitSuccess = 0,
		ExitIssues = 1,
		ExitUsage = 2
	};

	const TCHAR* const UsageLines[] =
	{
		TEXT("Usage:"),
		TEXT("  UnrealEditor-Cmd.exe Project.uproject -run=Grungify -Map=/Game/Maps/MyMap[,/Game/Maps/Other]"),
		TEXT("      -Operation=Apply|Clear|DryRun|Bake [-Zones=LabelA,LabelB] [-Save] [-Report=Path/To/Dir]"),
		TEXT("      [-LoadPadding=200] [-unattended -nullrhi]"),
		TEXT(""),
		TEXT("  -Map          Long package names of the maps to process, comma separated."),
		TEXT("  -Operation    What to run on each zone. Bake = Bake To Assets and requires -Save."),
		TEXT("  -Zones        Only zones with these actor labels (case-insensitive), comma separated."),
		TEXT("  -Save         Save dirty map, actor and content packages. Without it nothing is saved."),
		TEXT("  -Report       Folder for one JSON report per zone (default Saved/Grungify/Commandlet)."),
		TEXT("  -LoadPadding  World Partition: extra cm loaded around each zone's bounds (default 200)."),
		TEXT(""),
		TEXT("  Exit code: 0 ok, 1 warnings, custom data conflicts or failed meshes, 2 usage error or missing map."),
	};

	void PrintUsage()
	{
		for (const TCHAR* Line : UsageLines)
		{
			UE_LOG(LogGrungifyCommandlet, Display, TEXT("%s"), Line);
		}
	}

	TArray<FString> SplitList(const FString& Value)
	{
		TArray<FString> Items;
		Value.TrimQuotes().ParseIntoArray(Items, TEXT(","), true);
		for (FString& Item : Items)
		{
			Item.TrimStartAndEndInline();
		}
		Items.RemoveAll([](const FString& Item) { return Item.IsEmpty(); });
		return Items;
	}

	bool ParseOperation(const FString& Text, EGrungifyOperation& OutOperation)
	{
		// Preview and Cancel Preview are interactive-only; they'd be discarded before anything could see them.
		if (Text.Equals(TEXT("Apply"), ESearchCase::IgnoreCase)) { OutOperation = EGrungifyOperation::Apply; return true; }
		if (Text.Equals(TEXT("Clear"), ESearchCase::IgnoreCase)) { OutOperation = EGrungifyOperation::Clear; return true; }
		if (Text.Equals(TEXT("DryRun"), ESearchCase::IgnoreCase)) { OutOperation = EGrungifyOperation::DryRun; return true; }
		if (Text.Equals(TEXT("Bake"), ESearchCase::IgnoreCase)) { OutOperation = EGrungifyOperation::Bake; return true; }
		return false;
	}

	const TCHAR* OperationName(EGrungifyOperation Operation)
	{
		switch (Operation)
		{
		case EGrungifyOperation::Apply: return TEXT("Apply");
		case EGrungifyOperation::Clear: return TEXT("Clear");
		case EGrungifyOperation::DryRun: return TEXT("DryRun");
		case EGrungifyOperation::Bake: return TEXT("Bake");
		default: return TEXT("Unknown");
		}
	}

	bool NormalizeMapName(const FString& Input, FString& OutPackageName)
	{
		if (FPackageName::IsValidLongPackageName(Input))
		{
			OutPackageName = Input;
		}
		else if (!FPackageName::TryConvertFilenameToLongPackageName(Input, OutPackageName))
		{
			return false;
		}
		return FPackageName::DoesPackageExist(OutPackageName);
	}

	const FString* FindLabel(const FString& Label, const TArray<FString>& Filter)
	{
		return Filter.FindByPredicate([&Label](const FString& Wanted) { return Wanted.Equals(Label, ESearchCase::IgnoreCase); });
	}

	/** True if the report contains anything CI should flag; OutWhy lists it. */
	bool ReportHasIssues(const FGrungifyRunReport& Report, FString& OutWhy)
	{
		TArray<FString> Why;
		if (Report.Warnings.Num() > 0)
		{
			Why.Add(FString::Printf(TEXT("%d warning(s)"), Report.Warnings.Num()));
		}
		if (Report.bCancelled)
		{
			Why.Add(FString::Printf(TEXT("cancelled during %s"), *Report.CancelledDuring));
		}
		int32 NumFailed = 0;
		int32 NumConflicts = 0;
		for (const FGrungifyComponentReport& Entry : Report.Components)
		{
			NumFailed += Entry.Result == TEXT("failed") ? 1 : 0;
			NumConflicts += Entry.bCustomDataConflict ? 1 : 0;
		}
		if (NumFailed > 0)
		{
			Why.Add(FString::Printf(TEXT("%d failed component(s)"), NumFailed));
		}
		if (NumConflicts > 0)
		{
			Why.Add(FString::Printf(TEXT("%d custom data conflict(s)"), NumConflicts));
		}
		OutWhy = FString::Join(Why, TEXT(", "));
		return Why.Num() > 0;
	}

	/** Without -Save, drop dirty flags so the next map load doesn't try to save or prompt about this one. */
	void DiscardDirtyFlags()
	{
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (It->IsDirty())
			{
				It->SetDirtyFlag(false);
			}
		}
	}

	bool SaveDirty()
	{
		const bool bSaved = UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true);
		if (!bSaved)
		{
			UE_LOG(LogGrungifyCommandlet, Error, TEXT("Saving dirty packages failed."));
		}
		return bSaved;
	}

	/** Undo history holds references to modified objects, which would keep unloaded actors alive. */
	void ReleaseUndoHistory()
	{
		if (GEditor)
		{
			GEditor->ResetTransaction(LOCTEXT("ResetTransaction", "Grungify commandlet"));
		}
	}

	class FRunner
	{
	public:
		EGrungifyOperation Operation = EGrungifyOperation::DryRun;
		TArray<FString> ZoneFilter;
		TSet<FString> MatchedLabels;
		FString ReportDir;
		double LoadPaddingCm = 200.0;
		bool bSave = false;
		int32 ExitCode = ExitSuccess;

		void ProcessMap(const FString& MapName)
		{
			UE_LOG(LogGrungifyCommandlet, Display, TEXT("Loading %s"), *MapName);
			const FString MapFile = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(MapName, FPackageName::GetMapPackageExtension()));
			UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(MapFile);
			if (!World || World->GetPackage()->GetName() != MapName)
			{
				UE_LOG(LogGrungifyCommandlet, Error, TEXT("Could not load map %s"), *MapName);
				Raise(ExitUsage);
				return;
			}

			MapShortName = FPackageName::GetShortName(MapName);
			if (UWorldPartition* WorldPartition = World->GetWorldPartition())
			{
				ProcessWorldPartitionMap(World, WorldPartition);
			}
			else
			{
				ProcessLoadedMap(World);
			}

			if (bSave && !SaveDirty())
			{
				Raise(ExitIssues);
			}
			if (!bSave)
			{
				DiscardDirtyFlags();
			}
			ReleaseUndoHistory();
		}

	private:
		FString MapShortName;
		TSet<FString> UsedReportNames;

		void Raise(int32 Code)
		{
			ExitCode = FMath::Max(ExitCode, Code);
		}

		bool WantsZone(const FString& Label)
		{
			if (ZoneFilter.Num() == 0)
			{
				return true;
			}
			if (const FString* Wanted = FindLabel(Label, ZoneFilter))
			{
				MatchedLabels.Add(Wanted->ToLower());
				return true;
			}
			return false;
		}

		void ProcessLoadedMap(UWorld* World)
		{
			TArray<AGrungifyWeatheringZone*> Zones;
			for (AGrungifyWeatheringZone* Zone : UGrungifyBlueprintLibrary::GetZones(World))
			{
				if (WantsZone(Zone->GetActorLabel()))
				{
					Zones.Add(Zone);
				}
			}
			if (Zones.Num() == 0)
			{
				UE_LOG(LogGrungifyCommandlet, Warning, TEXT("%s: no matching weathering zones."), *MapShortName);
			}
			for (AGrungifyWeatheringZone* Zone : Zones)
			{
				RunZone(Zone);
			}
		}

		void ProcessWorldPartitionMap(UWorld* World, UWorldPartition* WorldPartition)
		{
			struct FTarget
			{
				FGuid Guid;
				FString Label;
				FBox Bounds;
			};

			// Zones may live in unloaded cells, so find them through the actor descriptors rather than the loaded actors.
			// FWorldPartitionActorDescInstance / ForEachActorDescInstance are the UE 5.4+ API (5.3 used FWorldPartitionActorDesc / ForEachActorDesc).
			TArray<FTarget> Targets;
			FWorldPartitionHelpers::ForEachActorDescInstance<AGrungifyWeatheringZone>(WorldPartition, [this, &Targets](const FWorldPartitionActorDescInstance* Desc)
			{
				const FString Label = Desc->GetActorLabel().ToString();
				if (WantsZone(Label))
				{
					Targets.Add({ Desc->GetGuid(), Label, Desc->GetEditorBounds() });
				}
				return true;
			});
			if (Targets.Num() == 0)
			{
				UE_LOG(LogGrungifyCommandlet, Warning, TEXT("%s: no matching weathering zones."), *MapShortName);
			}

			for (const FTarget& Target : Targets)
			{
				// Everything the zone can touch overlaps its box; the padding also brings in nearby vents and neighbouring zones.
				const FBox LoadBox = Target.Bounds.ExpandBy(LoadPaddingCm);
				UE_LOG(LogGrungifyCommandlet, Display, TEXT("%s: loading region around %s (%s)"), *MapShortName, *Target.Label, *LoadBox.ToString());

				TUniquePtr<FLoaderAdapterShape> Loader = MakeUnique<FLoaderAdapterShape>(World, LoadBox, FString::Printf(TEXT("Grungify %s"), *Target.Label));
				Loader->Load();

				AGrungifyWeatheringZone* Zone = nullptr;
				for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
				{
					if (It->GetActorGuid() == Target.Guid)
					{
						Zone = *It;
						break;
					}
				}

				if (Zone)
				{
					RunZone(Zone);
				}
				else
				{
					UE_LOG(LogGrungifyCommandlet, Error, TEXT("%s: zone %s did not load (is it in an unloaded Data Layer?)"), *MapShortName, *Target.Label);
					Raise(ExitIssues);
				}

				// Save before unloading: the region's actors and their changes go away with the loader.
				if (bSave && !SaveDirty())
				{
					Raise(ExitIssues);
				}
				ReleaseUndoHistory();
				Loader->Unload();
				Loader.Reset();
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			}
		}

		void RunZone(AGrungifyWeatheringZone* Zone)
		{
			const FString Label = Zone->GetActorLabel();
			FGrungifyRunReport Report;

			if (Operation == EGrungifyOperation::Clear)
			{
				// Clear doesn't produce a run report, so describe what it did here.
				const int32 Before = Zone->GetAppliedCount();
				Zone->Clear();
				const int32 Retained = Zone->GetAppliedCount();
				Report.Operation = TEXT("Clear");
				Report.Zone = Label;
				Report.Seed = Zone->Seed;
				Report.Time = FDateTime::Now().ToIso8601();
				Report.Weathered = Before - Retained;
				if (Retained > 0)
				{
					Report.Warnings.Add(FString::Printf(TEXT("%d mesh(es) could not be restored and keep their backup. %s"), Retained, *Zone->Status));
				}
			}
			else
			{
				const FString PreviousTime = Zone->GetLastReport().Time;
				Report = UGrungifyBlueprintLibrary::RunZone(Zone, Operation);
				// Every generating run stamps a fresh report; an unchanged one means the zone refused to run.
				if (Report.Time.IsEmpty() || Report.Time == PreviousTime)
				{
					Report = FGrungifyRunReport();
					Report.Operation = OperationName(Operation);
					Report.Zone = Label;
					Report.Time = FDateTime::Now().ToIso8601();
					Report.Warnings.Add(FString::Printf(TEXT("Zone did not run. Status: %s"), *Zone->Status));
				}
			}

			FString Why;
			const bool bIssues = ReportHasIssues(Report, Why);
			if (bIssues)
			{
				Raise(ExitIssues);
			}

			const FString File = WriteReport(Report, Label);
			UE_LOG(LogGrungifyCommandlet, Display, TEXT("%s | %s | %s | weathered %d, skipped %d, %.2fs | %s | %s"),
				*MapShortName, *Label, *Report.Operation, Report.Weathered, Report.Skipped, Report.Seconds,
				bIssues ? *Why : TEXT("ok"), File.IsEmpty() ? TEXT("report not written") : *File);
			for (const FString& Warning : Report.Warnings)
			{
				UE_LOG(LogGrungifyCommandlet, Warning, TEXT("%s | %s: %s"), *MapShortName, *Label, *Warning);
			}
		}

		FString WriteReport(const FGrungifyRunReport& Report, const FString& Label)
		{
			// Labels need not be unique, so number repeats instead of overwriting.
			const FString Base = FPaths::MakeValidFileName(FString::Printf(TEXT("%s_%s_%s"), *MapShortName, *Label, *Report.Operation));
			FString Name = Base;
			for (int32 Suffix = 2; UsedReportNames.Contains(Name); ++Suffix)
			{
				Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
			}
			UsedReportNames.Add(Name);

			const FString File = FPaths::Combine(ReportDir, Name + TEXT(".json"));
			if (!UGrungifyBlueprintLibrary::SaveReport(Report, File))
			{
				UE_LOG(LogGrungifyCommandlet, Error, TEXT("Could not write report %s"), *File);
				Raise(ExitIssues);
				return FString();
			}
			return File;
		}
	};
}

UGrungifyCommandlet::UGrungifyCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;

	HelpDescription = TEXT("Run Grungify weathering zones (Apply, Clear, Dry Run or Bake) on maps without opening the editor UI.");
	HelpUsage = TEXT("-run=Grungify -Map=/Game/Maps/MyMap[,/Game/Maps/Other] -Operation=Apply|Clear|DryRun|Bake [-Zones=A,B] [-Save] [-Report=Dir]");
}

int32 UGrungifyCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamMap;
	ParseCommandLine(*Params, Tokens, Switches, ParamMap);

	if (Switches.Contains(TEXT("Help")))
	{
		PrintUsage();
		return ExitSuccess;
	}

	const FString* MapParam = ParamMap.Find(TEXT("Map"));
	const FString* OperationParam = ParamMap.Find(TEXT("Operation"));
	if (!MapParam || MapParam->IsEmpty())
	{
		UE_LOG(LogGrungifyCommandlet, Error, TEXT("Missing -Map."));
		PrintUsage();
		return ExitUsage;
	}

	FRunner Runner;
	if (!OperationParam || !ParseOperation(OperationParam->TrimQuotes(), Runner.Operation))
	{
		UE_LOG(LogGrungifyCommandlet, Error, TEXT("Missing or unknown -Operation (expected Apply, Clear, DryRun or Bake)."));
		PrintUsage();
		return ExitUsage;
	}
	Runner.bSave = Switches.Contains(TEXT("Save"));
	// BakeToAssets saves its outputs itself; reject before loading or modifying any map.
	if (Runner.Operation == EGrungifyOperation::Bake && !Runner.bSave)
	{
		UE_LOG(LogGrungifyCommandlet, Error, TEXT("Bake requires -Save because it writes reusable assets."));
		return ExitUsage;
	}

	// Check every map up front so a typo fails fast instead of after an hour of baking.
	TArray<FString> Maps;
	for (const FString& Input : SplitList(*MapParam))
	{
		FString PackageName;
		if (!NormalizeMapName(Input, PackageName))
		{
			UE_LOG(LogGrungifyCommandlet, Error, TEXT("Map not found: %s"), *Input);
			return ExitUsage;
		}
		Maps.AddUnique(PackageName);
	}
	if (Maps.Num() == 0)
	{
		UE_LOG(LogGrungifyCommandlet, Error, TEXT("-Map lists no maps."));
		return ExitUsage;
	}

	if (const FString* ZonesParam = ParamMap.Find(TEXT("Zones")))
	{
		Runner.ZoneFilter = SplitList(*ZonesParam);
	}
	if (const FString* PaddingParam = ParamMap.Find(TEXT("LoadPadding")))
	{
		Runner.LoadPaddingCm = FMath::Max(0.0, FCString::Atod(**PaddingParam));
	}
	const FString* ReportParam = ParamMap.Find(TEXT("Report"));
	Runner.ReportDir = ReportParam && !ReportParam->IsEmpty()
		? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), ReportParam->TrimQuotes())
		: FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"), TEXT("Commandlet")));
	if (!IFileManager::Get().MakeDirectory(*Runner.ReportDir, true))
	{
		UE_LOG(LogGrungifyCommandlet, Error, TEXT("Could not create report folder %s"), *Runner.ReportDir);
		return ExitUsage;
	}

	UE_LOG(LogGrungifyCommandlet, Display, TEXT("Grungify %s on %d map(s)%s. Reports: %s"),
		OperationName(Runner.Operation), Maps.Num(), Runner.bSave ? TEXT(", saving") : TEXT(", not saving"), *Runner.ReportDir);

	for (const FString& Map : Maps)
	{
		Runner.ProcessMap(Map);
	}

	for (const FString& Wanted : Runner.ZoneFilter)
	{
		if (!Runner.MatchedLabels.Contains(Wanted.ToLower()))
		{
			UE_LOG(LogGrungifyCommandlet, Error, TEXT("No zone labelled %s in any map."), *Wanted);
			Runner.ExitCode = FMath::Max(Runner.ExitCode, int32(ExitUsage));
		}
	}

	UE_LOG(LogGrungifyCommandlet, Display, TEXT("Grungify commandlet finished with exit code %d."), Runner.ExitCode);
	return Runner.ExitCode;
}

#undef LOCTEXT_NAMESPACE
