#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GrungifyContentBuilder.h"
#include "GrungifySettings.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "GrungifyZoneVisualizer.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "ToolMenus.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
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

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGrungifyEditorModule::RegisterMenus));

		// Create missing assets and migrate outdated ones once the editor is fully up.
		EngineInitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([]()
		{
			if (UGrungifySettings::Get()->bAutoBuildContent && FGrungifyContentBuilder::NeedsWork())
			{
				RunContentBuild(/*bForce*/ false);
			}
		});

		// Previews must never be saved: drop them before any package save.
		PreSaveHandle = UPackage::PreSavePackageWithContextEvent.AddRaw(this, &FGrungifyEditorModule::OnPreSavePackage);

		// World Partition unloads actors without destroying them; drop a zone's preview when it goes.
		ActorRemovedHandle = ULevel::OnLoadedActorRemovedFromLevelEvent.AddRaw(this, &FGrungifyEditorModule::OnLoadedActorRemoved);

		SettingsHandle = GetMutableDefault<UGrungifySettings>()->OnSettingChanged().AddRaw(this, &FGrungifyEditorModule::OnSettingChanged);
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(EngineInitHandle);
		UPackage::PreSavePackageWithContextEvent.Remove(PreSaveHandle);
		ULevel::OnLoadedActorRemovedFromLevelEvent.Remove(ActorRemovedHandle);

		if (UObjectInitialized())
		{
			if (UGrungifySettings* Settings = GetMutableDefault<UGrungifySettings>())
			{
				Settings->OnSettingChanged().Remove(SettingsHandle);
			}
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
		}

		if (GUnrealEd)
		{
			GUnrealEd->UnregisterComponentVisualizer(UGrungifyZoneComponent::StaticClass()->GetFName());
		}
	}

private:
	FDelegateHandle EngineInitHandle;
	FDelegateHandle PreSaveHandle;
	FDelegateHandle ActorRemovedHandle;
	FDelegateHandle SettingsHandle;
	bool bDiscarding = false;

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
		for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
		{
			if (It->bPreviewActive)
			{
				It->DiscardPreview(TEXT("level saved"));
				++NumDiscarded;
			}
		}
		if (NumDiscarded > 0)
		{
			Notify(FText::Format(LOCTEXT("PreviewDiscarded", "Grungify: discarded {0} preview(s) before saving. Apply to keep weathering, or Preview again."), NumDiscarded));
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
		if (Name == GET_MEMBER_NAME_CHECKED(UGrungifySettings, CustomPrimitiveDataIndex) || Name == GET_MEMBER_NAME_CHECKED(UGrungifySettings, MaskUVChannel))
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
	}
};

IMPLEMENT_MODULE(FGrungifyEditorModule, GrungifyEditor)

#undef LOCTEXT_NAMESPACE
