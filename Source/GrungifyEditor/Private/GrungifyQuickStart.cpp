// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyQuickStart.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GrungifyMaterialHookup.h"
#include "GrungifySettings.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "Materials/Material.h"
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

	// Masks only show through materials that call Grungify, so offer to hook up what the selection uses.
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
		const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNoCancel, FText::Format(LOCTEXT("ConfirmHookup",
			"The selection uses {0} master material(s) that don't show Grungify weathering yet:\n\n{1}\n\n"
			"Hook them up now? Each is backed up first, every instance of it is affected, and Remove Grungify puts the original wiring back.\n\n"
			"Yes: hook up, then weather.  No: weather without hooking up (the masks are written but won't show on these materials).  Cancel: do nothing."),
			Unhooked.Num(), FText::FromString(FString::Join(Names, TEXT("\n")))));
		if (Answer == EAppReturnType::Cancel)
		{
			return;
		}
		if (Answer == EAppReturnType::Yes)
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
	Notify(FText::FromString(TEXT("Grungify: ") + Message), Zone == nullptr);
}

#undef LOCTEXT_NAMESPACE
