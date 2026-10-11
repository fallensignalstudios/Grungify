// Copyright Fallen Signal Studios. All Rights Reserved.

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GrungifyContentBuilder.h"
#include "GrungifyLibraryBuilder.h"
#include "GrungifyMaterialHookup.h"
#include "GrungifyReferenceScene.h"
#include "GrungifyShowcase.h"
#include "GrungifySettings.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "GrungifyZoneVisualizer.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "SGrungifyZonesPanel.h"
#include "ScopedTransaction.h"
#include "ToolMenus.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UnrealEdGlobals.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "GrungifyEditor"

namespace
{
	enum class EZoneAction : uint8
	{
		Preview,
		Apply,
		Clear,
		CancelPreview,
		DryRun
	};

	void Notify(const FText& Message, bool bError = false)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bError ? 8.f : 4.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	void RunOnAllZones(EZoneAction Action)
	{
		UWorld* World = EditorWorld();
		if (!World)
		{
			return;
		}

		TArray<AGrungifyWeatheringZone*> Zones;
		for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
		{
			Zones.Add(*It);
		}
		if (Zones.Num() == 0)
		{
			Notify(LOCTEXT("NoZones", "No Grungify weathering zones in this level."));
			return;
		}

		const FScopedTransaction Transaction(LOCTEXT("AllZones", "Grungify: All Zones"));
		for (AGrungifyWeatheringZone* Zone : Zones)
		{
			switch (Action)
			{
			case EZoneAction::Preview: Zone->Preview(); break;
			case EZoneAction::Apply: Zone->Apply(); break;
			case EZoneAction::Clear: Zone->Clear(); break;
			case EZoneAction::CancelPreview: Zone->CancelPreview(); break;
			case EZoneAction::DryRun: Zone->DryRun(); break;
			}
		}
		Notify(FText::Format(LOCTEXT("ZonesDone", "Grungify: processed {0} zone(s). Reports are in Saved/Grungify/Reports."), Zones.Num()));
	}

	void RunContentBuild(bool bForce)
	{
		const FGrungifyContentReport Report = FGrungifyContentBuilder::Run(bForce);
		if (Report.HasErrors())
		{
			Notify(FText::Format(LOCTEXT("ContentErrors", "Grungify content build had {0} error(s). See the Output Log or Saved/Grungify/ContentReport.txt."), Report.Errors.Num() + (Report.bSaved ? 0 : 1)), true);
		}
		else if (Report.Created.Num() + Report.Rebuilt.Num() > 0)
		{
			Notify(FText::Format(LOCTEXT("ContentBuilt", "Grungify: created {0}, rebuilt {1} asset(s); connections, compile and save OK."), Report.Created.Num(), Report.Rebuilt.Num()));
		}
		else
		{
			Notify(LOCTEXT("ContentCurrent", "Grungify: plugin content is up to date."));
		}
	}

	void BuildLibrary(bool bOverwrite)
	{
		FString Message;
		const bool bOk = FGrungifyLibraryBuilder::Build(bOverwrite, Message);
		UE_LOG(LogGrungify, Log, TEXT("%s"), *Message);
		Notify(FText::FromString(TEXT("Grungify: ") + Message), !bOk);
	}

	void BuildShowcase()
	{
		FString Message;
		const bool bOk = FGrungifyShowcase::Build(Message);
		UE_LOG(LogGrungify, Log, TEXT("%s"), *Message);
		Notify(FText::FromString(TEXT("Grungify: ") + Message), !bOk);
	}

	void BuildReferenceScene()
	{
		FString Message;
		const bool bOk = FGrungifyReferenceScene::Build(Message);
		UE_LOG(LogGrungify, Log, TEXT("%s"), *Message);
		Notify(FText::FromString(TEXT("Grungify: ") + Message), !bOk);
	}
}

class FGrungifyEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (GUnrealEd)
		{
			const TSharedPtr<FComponentVisualizer> Visualizer = MakeShared<FGrungifyZoneVisualizer>();
			GUnrealEd->RegisterComponentVisualizer(UGrungifyZoneComponent::StaticClass()->GetFName(), Visualizer);
			Visualizer->OnRegister();
		}

		GrungifyZonesPanel::Register();
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGrungifyEditorModule::RegisterMenus));

		// Create missing assets and migrate outdated ones once the editor is fully up.
		EngineInitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([this]()
		{
			BindExistingEditorLevels();
			if (UGrungifySettings::Get()->bAutoBuildContent && FGrungifyContentBuilder::NeedsWork())
			{
				RunContentBuild(/*bForce*/ false);
			}
		});

		// Previews must never be saved: drop them before any package save.
		PreSaveHandle = UPackage::PreSavePackageWithContextEvent.AddRaw(this, &FGrungifyEditorModule::OnPreSavePackage);

		// World Partition unloads actors without destroying them; drop a zone's preview when it goes.
		WorldInitializedHandle = FWorldDelegates::OnPostWorldInitialization.AddRaw(this, &FGrungifyEditorModule::OnWorldInitialized);
		LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddRaw(this, &FGrungifyEditorModule::OnLevelAdded);
		LevelRemovedHandle = FWorldDelegates::PreLevelRemovedFromWorld.AddRaw(this, &FGrungifyEditorModule::OnLevelRemoving);
		WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &FGrungifyEditorModule::OnWorldCleanup);
		BindExistingEditorLevels();

		SettingsHandle = GetMutableDefault<UGrungifySettings>()->OnSettingChanged().AddRaw(this, &FGrungifyEditorModule::OnSettingChanged);
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(EngineInitHandle);
		UPackage::PreSavePackageWithContextEvent.Remove(PreSaveHandle);
		FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitializedHandle);
		FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
		FWorldDelegates::PreLevelRemovedFromWorld.Remove(LevelRemovedHandle);
		FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
		for (const auto& Binding : ActorRemovedHandles)
		{
			if (ULevel* Level = Binding.Key.Get())
			{
				Level->OnLoadedActorRemovedFromLevelEvent.Remove(Binding.Value);
			}
		}
		ActorRemovedHandles.Empty();

		if (UObjectInitialized())
		{
			if (UGrungifySettings* Settings = GetMutableDefault<UGrungifySettings>())
			{
				Settings->OnSettingChanged().Remove(SettingsHandle);
			}
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
		}

		GrungifyZonesPanel::Unregister();

		if (GUnrealEd)
		{
			GUnrealEd->UnregisterComponentVisualizer(UGrungifyZoneComponent::StaticClass()->GetFName());
		}
	}

private:
	FDelegateHandle EngineInitHandle;
	FDelegateHandle PreSaveHandle;
	FDelegateHandle WorldInitializedHandle;
	FDelegateHandle LevelAddedHandle;
	FDelegateHandle LevelRemovedHandle;
	FDelegateHandle WorldCleanupHandle;
	TMap<TWeakObjectPtr<ULevel>, FDelegateHandle> ActorRemovedHandles;
	FDelegateHandle SettingsHandle;
	bool bDiscarding = false;

	void BindLevel(ULevel* Level, UWorld* World)
	{
		if (!Level || !World || World->WorldType != EWorldType::Editor || ActorRemovedHandles.Contains(Level))
		{
			return;
		}
		ActorRemovedHandles.Add(Level, Level->OnLoadedActorRemovedFromLevelEvent.AddRaw(this, &FGrungifyEditorModule::OnLoadedActorRemoved));
	}

	void BindWorldLevels(UWorld* World)
	{
		if (World && World->WorldType == EWorldType::Editor)
		{
			for (ULevel* Level : World->GetLevels())
			{
				BindLevel(Level, World);
			}
		}
	}

	void BindExistingEditorLevels()
	{
		for (TObjectIterator<UWorld> It; It; ++It)
		{
			BindWorldLevels(*It);
		}
	}

	void OnWorldInitialized(UWorld* World, const UWorld::InitializationValues InitializationValues)
	{
		BindWorldLevels(World);
	}

	void OnLevelAdded(ULevel* Level, UWorld* World)
	{
		BindLevel(Level, World);
	}

	void UnbindLevel(ULevel* Level)
	{
		if (FDelegateHandle* Handle = ActorRemovedHandles.Find(Level))
		{
			Level->OnLoadedActorRemovedFromLevelEvent.Remove(*Handle);
			ActorRemovedHandles.Remove(Level);
		}
	}

	void OnLevelRemoving(ULevel* Level, UWorld* World)
	{
		if (!World || World->WorldType != EWorldType::Editor)
		{
			return;
		}
		if (!Level)
		{
			OnWorldCleanup(World, false, false);
			return;
		}
		for (AActor* Actor : Level->Actors)
		{
			if (Actor)
			{
				OnLoadedActorRemoved(*Actor);
			}
		}
		UnbindLevel(Level);
	}

	void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
	{
		if (!World || World->WorldType != EWorldType::Editor)
		{
			return;
		}
		for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
		{
			OnLoadedActorRemoved(**It);
		}
		for (auto It = ActorRemovedHandles.CreateIterator(); It; ++It)
		{
			ULevel* Level = It.Key().Get();
			if (!Level || Level->GetWorld() == World)
			{
				if (Level)
				{
					Level->OnLoadedActorRemovedFromLevelEvent.Remove(It.Value());
				}
				It.RemoveCurrent();
			}
		}
	}

	void OnPreSavePackage(UPackage* Package, FObjectPreSaveContext Context)
	{
		// Autosaves are recovery copies; keep the preview (its records are saved with it) rather than dropping it every few minutes.
		if (bDiscarding || Context.IsProceduralSave() || Context.IsCooking() || (Context.GetSaveFlags() & SAVE_FromAutosave) != 0)
		{
			return;
		}

		UWorld* World = EditorWorld();
		if (!World)
		{
			return;
		}

		TGuardValue<bool> Guard(bDiscarding, true);
		int32 NumDiscarded = 0;
		int32 NumRetained = 0;
		for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
		{
			if (It->bPreviewActive)
			{
				It->DiscardPreview(TEXT("level saved"));
				if (It->bPreviewActive)
				{
					++NumRetained;
				}
				else
				{
					++NumDiscarded;
				}
			}
		}
		if (NumDiscarded > 0)
		{
			Notify(FText::Format(LOCTEXT("PreviewDiscarded", "Grungify: discarded {0} preview(s) before saving. Apply to keep weathering, or Preview again."), NumDiscarded));
		}
		if (NumRetained > 0)
		{
			Notify(FText::Format(LOCTEXT("PreviewRestoreFailed", "Grungify: could not restore {0} preview(s). Their original backups were retained; see the Output Log and repair the changed meshes before trying again."), NumRetained), true);
		}
	}

	void OnLoadedActorRemoved(AActor& Actor)
	{
		if (AGrungifyWeatheringZone* Zone = Cast<AGrungifyWeatheringZone>(&Actor))
		{
			if (Zone->bPreviewActive && !GIsTransacting)
			{
				Zone->DiscardPreview(TEXT("zone unloaded"));
			}
		}
	}

	void OnSettingChanged(UObject* Settings, FPropertyChangedEvent& Event)
	{
		const FName Name = Event.GetPropertyName();
		if (Name == GET_MEMBER_NAME_CHECKED(UGrungifySettings, CustomPrimitiveDataIndex) || Name == GET_MEMBER_NAME_CHECKED(UGrungifySettings, MaskUVChannel)
			|| Name == GET_MEMBER_NAME_CHECKED(UGrungifySettings, InstanceCustomDataIndex))
		{
			RunContentBuild(/*bForce*/ false);
			Notify(LOCTEXT("ReapplyZones", "Grungify: material content updated. Clear and re-apply existing zones so meshes use the new settings."));
		}
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("Grungify", LOCTEXT("Section", "Grungify"));

		Section.AddMenuEntry("GrungifyZonesPanel",
			LOCTEXT("ZonesPanel", "Weathering Zones Panel"),
			LOCTEXT("ZonesPanelTip", "List every weathering zone in the level with its status, and run actions on one or all of them."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&GrungifyZonesPanel::Open)));

		Section.AddMenuEntry("GrungifyHookUpLevel",
			LOCTEXT("HookUpLevel", "Hook Up Materials In Level"),
			LOCTEXT("HookUpLevelTip", "Find the master materials used inside weathering zones (or the whole level when there are none) that don't use Grungify yet, and wire Grungify into them. Each is backed up first."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UGrungifyMaterialHookup::HookUpLevel)));

		Section.AddMenuEntry("GrungifyHookUpSelected",
			LOCTEXT("HookUpSelected", "Add Grungify to Selected Materials"),
			LOCTEXT("HookUpSelectedTip", "Wire Grungify into the materials (or the parents of the material instances) selected in the Content Browser."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UGrungifyMaterialHookup::AddToSelectedAssets)));

		Section.AddMenuEntry("GrungifyUnhookSelected",
			LOCTEXT("UnhookSelected", "Remove Grungify from Selected Materials"),
			LOCTEXT("UnhookSelectedTip", "Put back the original wiring of materials Grungify was added to by Add Grungify or Hook Up Materials."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UGrungifyMaterialHookup::RemoveFromSelectedAssets)));

		// Right-click on materials and material instances in the Content Browser.
		for (const TCHAR* MenuName : { TEXT("ContentBrowser.AssetContextMenu.Material"), TEXT("ContentBrowser.AssetContextMenu.MaterialInstanceConstant") })
		{
			UToolMenu* AssetMenu = UToolMenus::Get()->ExtendMenu(MenuName);
			FToolMenuSection& AssetSection = AssetMenu->FindOrAddSection("Grungify", LOCTEXT("AssetSection", "Grungify"));
			AssetSection.AddMenuEntry("GrungifyAddToMaterial",
				LOCTEXT("AddToMaterial", "Add Grungify"),
				LOCTEXT("AddToMaterialTip", "Wire Grungify weathering into this material (for an instance, its parent). Legacy, Material Attributes and Substrate materials are supported. A backup goes to /Game/Grungify/Backups."),
				FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UGrungifyMaterialHookup::AddToSelectedAssets)));
			AssetSection.AddMenuEntry("GrungifyRemoveFromMaterial",
				LOCTEXT("RemoveFromMaterial", "Remove Grungify"),
				LOCTEXT("RemoveFromMaterialTip", "Put back the wiring this material had before Add Grungify."),
				FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UGrungifyMaterialHookup::RemoveFromSelectedAssets)));
		}

		Section.AddMenuEntry("GrungifyDryRunAll",
			LOCTEXT("DryRunAll", "Dry Run All Weathering Zones"),
			LOCTEXT("DryRunAllTip", "Report what every zone would do, without touching any mesh."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::DryRun)));

		Section.AddMenuEntry("GrungifyPreviewAll",
			LOCTEXT("PreviewAll", "Preview All Weathering Zones"),
			LOCTEXT("PreviewAllTip", "Preview every weathering zone in the level."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::Preview)));

		Section.AddMenuEntry("GrungifyApplyAll",
			LOCTEXT("ApplyAll", "Apply All Weathering Zones"),
			LOCTEXT("ApplyAllTip", "Apply every weathering zone in the level (one undo step)."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::Apply)));

		Section.AddMenuEntry("GrungifyCancelAll",
			LOCTEXT("CancelAll", "Cancel All Previews"),
			LOCTEXT("CancelAllTip", "Drop every zone's preview and go back to the applied state."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::CancelPreview)));

		Section.AddMenuEntry("GrungifyClearAll",
			LOCTEXT("ClearAll", "Clear All Weathering Zones"),
			LOCTEXT("ClearAllTip", "Remove the weathering from every zone in the level (one undo step)."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::Clear)));

		Section.AddMenuEntry("GrungifyUpdateContent",
			LOCTEXT("UpdateContent", "Update Plugin Content"),
			LOCTEXT("UpdateContentTip", "Create missing Grungify assets and rebuild any generated by an older version or other settings. Validates connections, compile and save."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunContentBuild, false)));

		Section.AddMenuEntry("GrungifyRebuildContent",
			LOCTEXT("RebuildContent", "Rebuild Plugin Content"),
			LOCTEXT("RebuildContentTip", "Rebuild MF_GrungifyWeathering and the Grungify materials in place, even if they look current."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunContentBuild, true)));

		Section.AddMenuEntry("GrungifyBuildLibrary",
			LOCTEXT("BuildLibrary", "Create Material Library"),
			LOCTEXT("BuildLibraryTip", "Create starter presets and material instances (Abandoned Industrial, Old Temple, Wet Sewer) in /Game/Grungify/Library. Existing assets are kept."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&BuildLibrary, false)));

		Section.AddMenuEntry("GrungifyRebuildLibrary",
			LOCTEXT("RebuildLibrary", "Reset Material Library"),
			LOCTEXT("RebuildLibraryTip", "Overwrite the starter presets and material instances in /Game/Grungify/Library with their defaults."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&BuildLibrary, true)));

		Section.AddMenuEntry("GrungifyReferenceScene",
			LOCTEXT("ReferenceScene", "Build Reference Scene"),
			LOCTEXT("ReferenceSceneTip", "Create /Game/Grungify/Reference/L_GrungifyReference with a fixed-seed zone and screenshot tests for visual regression checks."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&BuildReferenceScene)));

		Section.AddMenuEntry("GrungifyShowcase",
			LOCTEXT("Showcase", "Build Showcase Map"),
			LOCTEXT("ShowcaseTip", "Create or rebuild /Grungify/Showcase/L_GrungifyShowcase in the plugin's content: six lit bays (Before / After, the three library looks, Fire & Soot, Instanced & Stamps) with cameras and fixed-seed zones, for demos and store screenshots. Needs a writable plugin folder."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&BuildShowcase)));
	}
};

IMPLEMENT_MODULE(FGrungifyEditorModule, GrungifyEditor)

#undef LOCTEXT_NAMESPACE
