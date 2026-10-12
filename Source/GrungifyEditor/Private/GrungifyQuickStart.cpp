// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyQuickStart.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "GrungifyEditorLibrary.h"
#include "GrungifyPreset.h"
#include "Engine/Blueprint.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GrungifyMaterialHookup.h"
#include "GrungifySettings.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Misc/MessageDialog.h"
#include "ScopedTransaction.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "GrungifyQuickStart"

namespace
{
	const TCHAR* ZoneBlueprintPath = TEXT("/Grungify/BP_WeatheringZone.BP_WeatheringZone");
	constexpr double MinPaddingCm = 25.;
	constexpr double PaddingFraction = 0.1;
	/** Extra reach below the selection, so Trace Floors finds the floor it stands on. */
	constexpr double FloorReachCm = 100.;
	/** Where Weather Selected Actors puts private material copies (FGrungifyLocalWeatherOptions has its own setting). */
	const TCHAR* PrivateFolderDefault = TEXT("/Game/Grungify/Private");

	bool HasWeatherableMesh(const AActor* Actor)
	{
		TInlineComponentArray<UMeshComponent*> Components(Actor);
		return Components.ContainsByPredicate([](const UMeshComponent* Component)
		{
			return Component && Component->IsRegistered() && (Component->IsA<UStaticMeshComponent>() || Component->IsA<USkeletalMeshComponent>());
		});
	}

	UClass* ZoneClass()
	{
		// The plugin's Blueprint when it exists (it is what users place by hand), the native class otherwise.
		if (const UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, ZoneBlueprintPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(AGrungifyWeatheringZone::StaticClass()))
			{
				return Blueprint->GeneratedClass;
			}
		}
		return AGrungifyWeatheringZone::StaticClass();
	}

	void Notify(const FText& Message, bool bError)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bError ? 8.f : 5.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

FBox FGrungifyQuickStart::ZoneBoxFor(const FBox& Bounds)
{
	const FVector Padding = FVector::Max(Bounds.GetExtent() * PaddingFraction, FVector(MinPaddingCm));
	FBox Box(Bounds.Min - Padding, Bounds.Max + Padding);
	Box.Min.Z -= FloorReachCm;
	return Box;
}

AGrungifyWeatheringZone* FGrungifyQuickStart::WeatherActors(UWorld* World, const TArray<AActor*>& Actors, bool bPreview, FString& OutMessage)
{
	TArray<AActor*> Targets;
	FBox Bounds(ForceInit);
	for (AActor* Actor : Actors)
	{
		if (IsValid(Actor) && !Actor->IsA<AGrungifyWeatheringZone>() && Actor->GetWorld() == World && HasWeatherableMesh(Actor))
		{
			Targets.AddUnique(Actor);
			Bounds += Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
		}
	}
	if (!World || Targets.Num() == 0 || !Bounds.IsValid)
	{
		OutMessage = TEXT("Select one or more actors with static or skeletal meshes first.");
		return nullptr;
	}

	const FScopedTransaction Transaction(LOCTEXT("WeatherSelected", "Grungify: Weather Selected Actors"));
	const FBox Box = ZoneBoxFor(Bounds);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AGrungifyWeatheringZone* Zone = World->SpawnActor<AGrungifyWeatheringZone>(ZoneClass(), Box.GetCenter(), FRotator::ZeroRotator, Params);
	if (!Zone || !Zone->GetZoneComponent())
	{
		OutMessage = TEXT("Could not spawn a weathering zone.");
		return nullptr;
	}

	// SetActorLabel sends PostEditChangeProperty. Do not generate with an empty selection filter.
	// Restore the prior live-preview flag on return so subsequent artist edits retain their usual behaviour.
	TGuardValue<bool> LivePreviewGuard(Zone->bLivePreview, false);

	const FString First = Targets[0]->GetActorLabel();
	Zone->SetActorLabel(Targets.Num() == 1 ? FString::Printf(TEXT("Grungify_%s"), *First)
		: FString::Printf(TEXT("Grungify_%s_and_%d_more"), *First, Targets.Num() - 1));
	Zone->GetZoneComponent()->SetBoxExtent(Box.GetExtent());
	// Only the selection: neighbours that happen to overlap the padded box are left alone.
	for (AActor* Target : Targets)
	{
		Zone->IncludeOnlyActors.Add(Target);
	}
	if (bPreview)
	{
		Zone->Preview();
	}
	OutMessage = FString::Printf(TEXT("%s %d actor(s) with %s. Adjust it in the Details panel, then Apply to keep it."),
		bPreview ? TEXT("Previewing") : TEXT("Created a zone for"), Targets.Num(), *Zone->GetActorLabel());
	return Zone;
}

namespace
{
	/** A unique package for a private copy of Source under Folder: <Folder>/<Source>_Private, _Private_2, ... */
	FString UniquePrivatePackage(const FString& Folder, const UObject* Source)
	{
		const FString Base = FString::Printf(TEXT("%s/%s_Private"), *Folder, *Source->GetName());
		FString Candidate = Base;
		for (int32 Index = 2; FPackageName::DoesPackageExist(Candidate) || FindPackage(nullptr, *Candidate); ++Index)
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, Index);
		}
		return Candidate;
	}

	template <typename T>
	T* DuplicateAsPrivate(T* Source, const FString& Folder, TArray<FString>& Created)
	{
		const FString PackageName = UniquePrivatePackage(Folder, Source);
		UPackage* Package = CreatePackage(*PackageName);
		T* Copy = DuplicateObject<T>(Source, Package, *FPackageName::GetShortName(PackageName));
		if (!Copy)
		{
			return nullptr;
		}
		Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		Copy->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Copy);
		Created.Add(Copy->GetPathName());
		return Copy;
	}
	/**
	 * Clones each slot's master material (and its material-instance chain) under Folder, hooks Grungify into the clone,
	 * and assigns the clone to only those slots of Component. Shared assets are never modified.
	 */
	void ClonePrivateMaterials(UMeshComponent* Component, const TArray<int32>& Slots, const FString& Folder, FGrungifyLocalWeatherResult& Result)
	{
		// One clone per distinct material chain, shared by the chosen slots that use it.
		TMap<UMaterialInterface*, UMaterialInterface*> CloneOf;
		for (int32 Slot : Slots)
		{
			UMaterialInterface* Leaf = Component->GetMaterial(Slot);
			if (!Leaf)
			{
				Result.Notes.Add(FString::Printf(TEXT("Slot %d has no material; left alone."), Slot));
				continue;
			}
			UMaterialInterface** Existing = CloneOf.Find(Leaf);
			UMaterialInterface* Clone = Existing ? *Existing : nullptr;
			if (!Existing)
			{
				// Leaf -> ... -> base material. Only constant instances can be cloned; a dynamic one belongs to gameplay.
				TArray<UMaterialInstanceConstant*> Chain;
				UMaterialInterface* It = Leaf;
				bool bClonable = true;
				while (UMaterialInstance* Instance = Cast<UMaterialInstance>(It))
				{
					UMaterialInstanceConstant* Constant = Cast<UMaterialInstanceConstant>(Instance);
					if (!Constant)
					{
						bClonable = false;
						break;
					}
					Chain.Add(Constant);
					It = Instance->Parent;
				}
				UMaterial* Base = Cast<UMaterial>(It);
				if (!bClonable || !Base)
				{
					Result.Notes.Add(FString::Printf(TEXT("Slot %d: %s isn't a material or a chain of material instance constants; it can't be cloned privately and is left alone."),
						Slot, *Leaf->GetPathName()));
					CloneOf.Add(Leaf, nullptr);
					continue;
				}

				UMaterial* PrivateBase = DuplicateAsPrivate(Base, Folder, Result.Created);
				if (!PrivateBase)
				{
					Result.Notes.Add(FString::Printf(TEXT("Slot %d: could not duplicate %s."), Slot, *Base->GetPathName()));
					CloneOf.Add(Leaf, nullptr);
					continue;
				}
				// The clone is ours alone, so no backup: the original master is the backup.
				const FGrungifyHookupResult Hookup = UGrungifyMaterialHookup::AddGrungifyToMaterial(PrivateBase, /*bBackup*/ false);
				Result.Notes.Add(FString::Printf(TEXT("Private master %s: hookup %s%s"), *PrivateBase->GetPathName(), *Hookup.Outcome,
					Hookup.Notes.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Hookup.Notes)));
				if (Hookup.Outcome != TEXT("added") && Hookup.Outcome != TEXT("already"))
				{
					CloneOf.Add(Leaf, nullptr);
					continue;
				}

				// Re-create the instance chain on top of the private master, base first, keeping every parameter.
				UMaterialInterface* Parent = PrivateBase;
				for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
				{
					UMaterialInstanceConstant* Copy = DuplicateAsPrivate(Chain[Index], Folder, Result.Created);
					if (!Copy)
					{
						Parent = nullptr;
						break;
					}
					Copy->SetParentEditorOnly(Parent);
					Copy->PostEditChange();
					Parent = Copy;
				}
				Clone = Parent;
				CloneOf.Add(Leaf, Clone);
			}
			if (Clone)
			{
				Component->Modify();
				Component->SetMaterial(Slot, Clone);
				Result.PrivateSlots.Add(Slot);
			}
		}
	}
}

FGrungifyLocalWeatherResult FGrungifyQuickStart::WeatherComponentSlots(UMeshComponent* Component, const FGrungifyLocalWeatherOptions& Options)
{
	FGrungifyLocalWeatherResult Result;
	UWorld* World = Component ? Component->GetWorld() : nullptr;
	AActor* Owner = Component ? Component->GetOwner() : nullptr;
	if (!World || !Owner || !Component->IsRegistered())
	{
		Result.Notes.Add(TEXT("No registered component in a world."));
		return Result;
	}

	TArray<int32> Slots = Options.Slots;
	if (Slots.Num() == 0)
	{
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			Slots.Add(Slot);
		}
	}
	for (int32 Slot : Slots)
	{
		if (Slot < 0 || Slot >= Component->GetNumMaterials())
		{
			Result.Notes.Add(FString::Printf(TEXT("Slot %d doesn't exist (the component has %d)."), Slot, Component->GetNumMaterials()));
			return Result;
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("WeatherSlots", "Grungify: Weather Component Slots"));

	// ------------------------------------------------ Private master clones
	if (Options.bPrivateMaster)
	{
		ClonePrivateMaterials(Component, Slots, Options.PrivateFolder, Result);
	}
	else
	{
		for (int32 Slot : Slots)
		{
			if (!UGrungifyMaterialHookup::UsesGrungify(Component->GetMaterial(Slot)))
			{
				Result.Notes.Add(FString::Printf(TEXT("Slot %d's material doesn't use Grungify: its masks will be written but won't show."), Slot));
			}
		}
	}

	// ------------------------------------------------ Zone scoped to exactly these slots
	const FBox Box = ZoneBoxFor(Component->Bounds.GetBox());
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AGrungifyWeatheringZone* Zone = World->SpawnActor<AGrungifyWeatheringZone>(ZoneClass(), Box.GetCenter(), FRotator::ZeroRotator, Params);
	if (!Zone || !Zone->GetZoneComponent())
	{
		Result.Notes.Add(TEXT("Could not spawn a weathering zone."));
		return Result;
	}
	{
		// No generation until the scope is fully set (SetActorLabel and LoadPreset notify property changes).
		TGuardValue<bool> LivePreviewGuard(Zone->bLivePreview, false);
		Zone->SetActorLabel(FString::Printf(TEXT("Grungify_%s_Slots"), *Owner->GetActorLabel()));
		if (Options.Preset)
		{
			Zone->Preset = Options.Preset;
			Zone->LoadPreset();
		}
		Zone->GetZoneComponent()->SetBoxExtent(Box.GetExtent());
		Zone->IncludeOnlyActors.Add(Owner);
		Zone->bOnlySlotsMatchedByRules = true;
		for (int32 Slot : Slots)
		{
			FGrungifySlotRule& Rule = Zone->SlotRules.AddDefaulted_GetRef();
			Rule.Label = FString::Printf(TEXT("Local slot %d"), Slot);
			Rule.Actor = Owner;
			Rule.ComponentName = Component->GetFName();
			Rule.SlotIndex = Slot;
			Rule.Gate.GateTexture = Options.PermissionGate;
			Rule.Gate.GateChannel = Options.GateChannel;
			Rule.Gate.GateUVChannel = Options.GateUVChannel;
			Rule.Gate.bInvertGate = Options.bInvertGate;
		}

		if (Options.bPreview)
		{
			Zone->Preview();
		}
	}
	Result.Zone = Zone;
	Result.bSucceeded = true;

	// Counts come from what the zone's report says happened, after writes.
	if (Options.bPreview)
	{
		const FGrungifyRunReport& Report = Zone->GetLastReport();
		Result.Weathered = Report.Weathered;
		Result.Skipped = Report.Skipped;
		Result.Failed = Report.Failed + Report.Cancelled;
		Result.Queued = Zone->IsGeneratingInBackground() ? 1 : 0;
	}
	return Result;
}

void FGrungifyQuickStart::WeatherSelected()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TArray<AActor*> Actors;
	if (GEditor)
	{
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				Actors.Add(Actor);
			}
		}
	}

	// One undo step for everything below that touches the level (private slot assignments, zone, masks).
	const FScopedTransaction Transaction(LOCTEXT("WeatherSelectedAll", "Grungify: Weather Selected Actors"));

	// Masks only show through materials that call Grungify, so offer to hook up what the selection uses.
	FGrungifyLocalWeatherResult Private;
	const TArray<UMaterial*> Unhooked = UGrungifyMaterialHookup::FindUnhookedMaterialsOn(Actors);
	if (Unhooked.Num() > 0)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < Unhooked.Num() && Index < 10; ++Index)
		{
			Names.Add(TEXT("  ") + Unhooked[Index]->GetPathName());
		}
		if (Unhooked.Num() > 10)
		{
			Names.Add(FString::Printf(TEXT("  ...and %d more"), Unhooked.Num() - 10));
		}
		const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNoCancel, FText::Format(LOCTEXT("ConfirmHookupPrivate",
			"The selection uses {0} master material(s) that don't show Grungify weathering yet:\n\n{1}\n\n"
			"Yes (recommended): make private copies for the selected actors only, in {2}. Shared masters and every other "
			"actor are left unchanged; Undo puts the selection's original materials back (the copies stay on disk to delete or keep).\n\n"
			"No: hook up the shared masters themselves. Each is backed up first, but every instance of them in the project is affected.\n\n"
			"Cancel: do nothing."),
			Unhooked.Num(), FText::FromString(FString::Join(Names, TEXT("\n"))), FText::FromString(PrivateFolderDefault)));
		if (Answer == EAppReturnType::Cancel)
		{
			return;
		}
		if (Answer == EAppReturnType::Yes)
		{
			for (AActor* Actor : Actors)
			{
				TInlineComponentArray<UMeshComponent*> Components(Actor);
				for (UMeshComponent* Component : Components)
				{
					if (!Component || !Component->IsRegistered() || (!Component->IsA<UStaticMeshComponent>() && !Component->IsA<USkeletalMeshComponent>()))
					{
						continue;
					}
					TArray<int32> Slots;
					for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
					{
						const UMaterialInterface* Material = Component->GetMaterial(Slot);
						if (Material && Unhooked.Contains(Material->GetMaterial()))
						{
							Slots.Add(Slot);
						}
					}
					ClonePrivateMaterials(Component, Slots, PrivateFolderDefault, Private);
				}
			}
		}
		else
		{
			UGrungifyMaterialHookup::HookUpMaterials(Unhooked);
		}
	}

	FString Message;
	AGrungifyWeatheringZone* Zone = WeatherActors(World, Actors, /*bPreview*/ true, Message);
	if (Zone)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		GEditor->SelectActor(Zone, /*bInSelected*/ true, /*bNotify*/ true);
	}
	if (Private.Created.Num() > 0)
	{
		Message += FString::Printf(TEXT(" Created %d private material asset(s) in %s; save or delete them."), Private.Created.Num(), PrivateFolderDefault);
		for (const FString& Note : Private.Notes)
		{
			UE_LOG(LogGrungify, Log, TEXT("Weather Selected Actors: %s"), *Note);
		}
	}
	Notify(FText::FromString(TEXT("Grungify: ") + Message), Zone == nullptr);
}

#undef LOCTEXT_NAMESPACE
