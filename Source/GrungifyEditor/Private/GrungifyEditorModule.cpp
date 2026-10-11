#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
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
		CancelPreview
	};

	void Notify(const FText& Message)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 4.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	void RunOnAllZones(EZoneAction Action)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
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
			}
		}
		Notify(FText::Format(LOCTEXT("ZonesDone", "Grungify: processed {0} zone(s)."), Zones.Num()));
	}

	void BuildContent()
	{
		const int32 Created = FGrungifyContentBuilder::BuildMissingContent();
		Notify(Created > 0
			? FText::Format(LOCTEXT("ContentBuilt", "Grungify: created {0} asset(s) in /Grungify."), Created)
			: LOCTEXT("ContentPresent", "Grungify: all plugin content already exists."));
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

		// Create BP_WeatheringZone and the materials once the editor is fully up.
		EngineInitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([]()
		{
			if (UGrungifySettings::Get()->bAutoBuildContent && FGrungifyContentBuilder::IsContentMissing())
			{
				FGrungifyContentBuilder::BuildMissingContent();
			}
		});
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(EngineInitHandle);

		if (GUnrealEd)
		{
			GUnrealEd->UnregisterComponentVisualizer(UGrungifyZoneComponent::StaticClass()->GetFName());
		}

		if (UObjectInitialized())
		{
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
		}
	}

private:
	FDelegateHandle EngineInitHandle;

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("Grungify", LOCTEXT("Section", "Grungify"));

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

		Section.AddMenuEntry("GrungifyBuildContent",
			LOCTEXT("BuildContent", "Build Plugin Content"),
			LOCTEXT("BuildContentTip", "Create any missing Grungify assets (BP_WeatheringZone, MF_GrungifyWeathering, mask debug and example materials)."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&BuildContent)));
	}
};

IMPLEMENT_MODULE(FGrungifyEditorModule, GrungifyEditor)

#undef LOCTEXT_NAMESPACE
