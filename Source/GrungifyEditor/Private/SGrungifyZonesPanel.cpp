#include "SGrungifyZonesPanel.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GrungifyPreset.h"
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "GrungifyZonesPanel"

const FName GrungifyZonesPanel::TabId(TEXT("GrungifyZones"));

// Named rather than anonymous so unity builds can't collide with the module's own helpers.
namespace GrungifyZonesPanelPrivate
{
	enum class EPanelAction : uint8
	{
		Preview,
		Apply,
		Clear,
		CancelPreview,
		DryRun
	};

	const FName ColZone(TEXT("Zone"));
	const FName ColStatus(TEXT("Status"));
	const FName ColPreview(TEXT("Preview"));
	const FName ColApplied(TEXT("Applied"));
	const FName ColLastRun(TEXT("LastRun"));
	const FName ColWarnings(TEXT("Warnings"));
	const FName ColPreset(TEXT("Preset"));
	const FName ColActions(TEXT("Actions"));

	const FName ColComponent(TEXT("Component"));
	const FName ColResult(TEXT("Result"));
	const FName ColNotes(TEXT("Notes"));
	const FName ColTexture(TEXT("Texture"));
	const FName ColSeconds(TEXT("Seconds"));
	const FName ColCache(TEXT("Cache"));

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	void Notify(const FText& Message)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 4.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	void RunAction(AGrungifyWeatheringZone* Zone, EPanelAction Action)
	{
		if (!IsValid(Zone))
		{
			return;
		}
		switch (Action)
		{
		case EPanelAction::Preview: Zone->Preview(); break;
		case EPanelAction::Apply: Zone->Apply(); break;
		case EPanelAction::Clear: Zone->Clear(); break;
		case EPanelAction::CancelPreview: Zone->CancelPreview(); break;
		case EPanelAction::DryRun: Zone->DryRun(); break;
		}
	}

	FText ActionName(EPanelAction Action)
	{
		switch (Action)
		{
		case EPanelAction::Preview: return LOCTEXT("ActPreview", "Preview");
		case EPanelAction::Apply: return LOCTEXT("ActApply", "Apply");
		case EPanelAction::Clear: return LOCTEXT("ActClear", "Clear");
		case EPanelAction::CancelPreview: return LOCTEXT("ActCancel", "Cancel Preview");
		case EPanelAction::DryRun: return LOCTEXT("ActDryRun", "Dry Run");
		}
		return FText::GetEmpty();
	}

	/** Run-level warnings plus every component that has a problem of its own. */
	int32 CountWarnings(const FGrungifyRunReport& Report)
	{
		int32 Count = Report.Warnings.Num();
		for (const FGrungifyComponentReport& Component : Report.Components)
		{
			bool bSlotWarning = false;
			for (const FGrungifySlotReport& Slot : Component.SlotDetails)
			{
				if (Slot.Warnings.Num() > 0)
				{
					bSlotWarning = true;
					break;
				}
			}
			if (Component.bCustomDataConflict || bSlotWarning)
			{
				++Count;
			}
		}
		return Count;
	}

	FString LastRunString(const FGrungifyRunReport& Report)
	{
		if (Report.Operation.IsEmpty())
		{
			return TEXT("-");
		}
		FString Result = FString::Printf(TEXT("%s  %.2fs  %d weathered / %d skipped"), *Report.Operation, Report.Seconds, Report.Weathered, Report.Skipped);
		if (Report.bCancelled)
		{
			Result += TEXT("  (cancelled)");
		}
		return Result;
	}

	/** Changes whenever the zone produces a new report, so the details area only rebuilds when it has to. */
	FString ReportSignature(const AGrungifyWeatheringZone* Zone)
	{
		if (!IsValid(Zone))
		{
			return FString();
		}
		const FGrungifyRunReport& Report = Zone->GetLastReport();
		return FString::Printf(TEXT("%s|%s|%s|%.4f|%d|%d"), *Zone->GetPathName(), *Report.Operation, *Report.Time, Report.Seconds, Report.Components.Num(), Report.bCancelled ? 1 : 0);
	}

	FString ComponentKey(const FGrungifyComponentReport& Component)
	{
		return Component.Actor + TEXT("/") + Component.Component;
	}

	FString Percent(float Share)
	{
		return FString::Printf(TEXT("%.1f%%"), Share * 100.f);
	}

	FString BuildDetailsText(const FGrungifyRunReport& Report, const FGrungifyComponentReport* Component)
	{
		if (Report.Operation.IsEmpty())
		{
			return LOCTEXT("NoReport", "No report yet. Run Dry Run, Preview or Apply on this zone.").ToString();
		}

		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("%s of %s at %s, seed %d, %.2fs"), *Report.Operation, *Report.Zone, *Report.Time, Report.Seed, Report.Seconds));
		Lines.Add(FString::Printf(TEXT("Weathered %d, skipped %d, cache hits %d, scattered %d, runoff sources %d%s"),
			Report.Weathered, Report.Skipped, Report.CacheHits, Report.Scattered, Report.RunoffSources, Report.bBackground ? TEXT(", background") : TEXT("")));
		if (Report.bCancelled)
		{
			Lines.Add(FString::Printf(TEXT("Cancelled during: %s"), Report.CancelledDuring.IsEmpty() ? TEXT("?") : *Report.CancelledDuring));
		}
		if (!Report.Preset.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("Preset: %s"), *Report.Preset));
		}

		if (Component)
		{
			Lines.Add(FString());
			Lines.Add(FString::Printf(TEXT("== %s / %s (%s) =="), *Component->Actor, *Component->Component, *Component->Mesh));
			Lines.Add(FString::Printf(TEXT("Result: %s%s"), *Component->Result, Component->Notes.IsEmpty() ? TEXT("") : *(TEXT("  - ") + Component->Notes)));
			if (Component->bCustomDataConflict)
			{
				Lines.Add(TEXT("Warning: custom primitive data indices are already used by something else; the material path stays inactive."));
			}
			if (!Component->PreviousOwner.IsEmpty())
			{
				Lines.Add(FString::Printf(TEXT("Previous owner: %s"), *Component->PreviousOwner));
			}

			Lines.Add(TEXT("Slots:"));
			if (Component->SlotDetails.Num() == 0)
			{
				Lines.Add(TEXT("  (no slot details)"));
			}
			for (const FGrungifySlotReport& Slot : Component->SlotDetails)
			{
				Lines.Add(FString::Printf(TEXT("  [%d] %s  material %s  %s: %s"),
					Slot.Slot, *Slot.SlotName, *Slot.Material, Slot.bIncluded ? TEXT("included") : TEXT("excluded"), *Slot.Reason));
				if (!Slot.Rule.IsEmpty())
				{
					Lines.Add(FString::Printf(TEXT("      rule: %s"), *Slot.Rule));
				}
				Lines.Add(FString::Printf(TEXT("      surface %s (%s), profile %s, rail %s%s"),
					*Slot.Surface, *Slot.SurfaceSource, *Slot.Profile, Slot.bRail ? TEXT("yes") : TEXT("no"),
					Slot.RailSource.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Slot.RailSource)));
				if (!Slot.Gates.IsEmpty())
				{
					Lines.Add(FString::Printf(TEXT("      gates: %s"), *Slot.Gates));
				}
				for (const FString& Warning : Slot.Warnings)
				{
					Lines.Add(FString::Printf(TEXT("      warning: %s"), *Warning));
				}
			}

			const FGrungifyUVReport& UV = Component->MaskUV;
			if (UV.bChecked)
			{
				Lines.Add(FString::Printf(TEXT("Mask UV channel %d: %s%s  out of range %s, overlap %s, degenerate %s, coverage %s, density spread %.2f"),
					UV.Channel, UV.Verdict.IsEmpty() ? TEXT("?") : *UV.Verdict, UV.bPresent ? TEXT("") : TEXT(" (channel missing)"),
					*Percent(UV.OutOfRange), *Percent(UV.Overlap), *Percent(UV.Degenerate), *Percent(UV.Coverage), UV.DensitySpread));
			}
			else
			{
				Lines.Add(TEXT("Mask UV: not checked"));
			}
			for (const FString& Problem : UV.Problems)
			{
				Lines.Add(FString::Printf(TEXT("  problem: %s"), *Problem));
			}
		}
		else if (Report.Components.Num() > 0)
		{
			Lines.Add(FString());
			Lines.Add(LOCTEXT("PickComponent", "Select a component to see its slot decisions and mask UV checks.").ToString());
		}

		Lines.Add(FString());
		Lines.Add(TEXT("Phases:"));
		if (Report.Phases.Num() == 0)
		{
			Lines.Add(TEXT("  (none)"));
		}
		for (const FGrungifyPhaseTiming& Phase : Report.Phases)
		{
			Lines.Add(FString::Printf(TEXT("  %s: %.3fs, %d items"), *Phase.Phase, Phase.Seconds, Phase.Items));
		}

		Lines.Add(FString());
		Lines.Add(FString::Printf(TEXT("Warnings (%d):"), Report.Warnings.Num()));
		for (const FString& Warning : Report.Warnings)
		{
			Lines.Add(FString::Printf(TEXT("  - %s"), *Warning));
		}

		if (Report.BakedAssets.Num() > 0)
		{
			Lines.Add(FString());
			Lines.Add(TEXT("Baked assets:"));
			for (const FString& Asset : Report.BakedAssets)
			{
				Lines.Add(FString::Printf(TEXT("  %s"), *Asset));
			}
		}

		return FString::Join(Lines, TEXT("\n"));
	}

	struct FZoneItem
	{
		TWeakObjectPtr<AGrungifyWeatheringZone> Zone;
		FString Label;
	};

	struct FComponentItem
	{
		// A copy: the zone replaces its report on every run, and rows must never point into a stale one.
		FGrungifyComponentReport Report;
	};

	using FZoneItemPtr = TSharedPtr<FZoneItem>;
	using FComponentItemPtr = TSharedPtr<FComponentItem>;

class SGrungifyZonesPanel;

class SGrungifyZoneRow : public SMultiColumnTableRow<FZoneItemPtr>
{
public:
	SLATE_BEGIN_ARGS(SGrungifyZoneRow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable, FZoneItemPtr InItem, TWeakPtr<SGrungifyZonesPanel> InPanel)
	{
		Item = InItem;
		Panel = InPanel;
		SMultiColumnTableRow<FZoneItemPtr>::Construct(FSuperRowType::FArguments().Padding(FMargin(0.f, 1.f)), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override;

private:
	FZoneItemPtr Item;
	TWeakPtr<SGrungifyZonesPanel> Panel;

	AGrungifyWeatheringZone* GetZone() const
	{
		AGrungifyWeatheringZone* Zone = Item.IsValid() ? Item->Zone.Get() : nullptr;
		return IsValid(Zone) ? Zone : nullptr;
	}

	TSharedRef<SWidget> MakeText(TAttribute<FText> Text) const
	{
		return SNew(SBox)
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.f, 0.f))
			[
				SNew(STextBlock).Text(Text)
			];
	}

	TSharedRef<SWidget> MakeButton(const FText& Label, const FText& ToolTip, TFunction<void()> OnClick, TAttribute<bool> Enabled = true) const
	{
		return SNew(SButton)
			.Text(Label)
			.ToolTipText(ToolTip)
			.ContentPadding(FMargin(4.f, 0.f))
			.IsEnabled(Enabled)
			.OnClicked_Lambda([OnClick]()
			{
				OnClick();
				return FReply::Handled();
			});
	}
};

class SGrungifyComponentRow : public SMultiColumnTableRow<FComponentItemPtr>
{
public:
	SLATE_BEGIN_ARGS(SGrungifyComponentRow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable, FComponentItemPtr InItem)
	{
		Item = InItem;
		SMultiColumnTableRow<FComponentItemPtr>::Construct(FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid())
		{
			return SNullWidget::NullWidget;
		}
		const FGrungifyComponentReport& Report = Item->Report;

		FString Text;
		FString ToolTip;
		if (ColumnName == ColComponent)
		{
			Text = ComponentKey(Report);
			ToolTip = Report.Mesh.IsEmpty() ? Text : FString::Printf(TEXT("%s\nMesh: %s"), *Text, *Report.Mesh);
		}
		else if (ColumnName == ColResult)
		{
			Text = Report.Result;
		}
		else if (ColumnName == ColNotes)
		{
			Text = Report.Notes;
			ToolTip = Report.Notes;
		}
		else if (ColumnName == ColTexture)
		{
			Text = Report.TextureSize > 0 ? FString::Printf(TEXT("%d"), Report.TextureSize) : TEXT("-");
		}
		else if (ColumnName == ColSeconds)
		{
			Text = FString::Printf(TEXT("%.3f"), Report.Seconds);
		}
		else if (ColumnName == ColCache)
		{
			Text = Report.bFromCache ? TEXT("yes") : TEXT("no");
		}

		return SNew(SBox)
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.f, 0.f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Text))
				.ToolTipText(FText::FromString(ToolTip))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			];
	}

private:
	FComponentItemPtr Item;
};

class SGrungifyZonesPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGrungifyZonesPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		ChildSlot
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.f)
			[
				MakeToolbar()
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			.Padding(4.f, 0.f, 4.f, 4.f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Vertical)

				+ SSplitter::Slot()
				.Value(0.5f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(0.f)
					[
						SNew(SOverlay)

						+ SOverlay::Slot()
						[
							SAssignNew(ZoneList, SListView<FZoneItemPtr>)
							.ListItemsSource(&FilteredItems)
							.SelectionMode(ESelectionMode::Single)
							.OnGenerateRow(this, &SGrungifyZonesPanel::GenerateZoneRow)
							.OnSelectionChanged(this, &SGrungifyZonesPanel::OnZoneSelectionChanged)
							.HeaderRow(MakeZoneHeader())
						]

						+ SOverlay::Slot()
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Visibility_Lambda([this]() { return FilteredItems.Num() == 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
							.Text_Lambda([this]()
							{
								return AllItems.Num() == 0
									? LOCTEXT("NoZones", "No Grungify weathering zones in this level.")
									: LOCTEXT("NoMatch", "No zones match the filter.");
							})
						]
					]
				]

				+ SSplitter::Slot()
				.Value(0.5f)
				[
					SNew(SSplitter)
					.Orientation(Orient_Horizontal)

					+ SSplitter::Slot()
					.Value(0.55f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.Padding(0.f)
						[
							SNew(SVerticalBox)

							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(4.f)
							[
								SNew(STextBlock)
								.Text_Lambda([this]()
								{
									if (!SelectedZone.IsValid())
									{
										return LOCTEXT("ComponentsNoZone", "Components (select a zone)");
									}
									return FText::Format(LOCTEXT("ComponentsCount", "Components in last report ({0})"), ComponentItems.Num());
								})
							]

							+ SVerticalBox::Slot()
							.FillHeight(1.f)
							[
								SAssignNew(ComponentList, SListView<FComponentItemPtr>)
								.ListItemsSource(&ComponentItems)
								.SelectionMode(ESelectionMode::Single)
								.OnGenerateRow(this, &SGrungifyZonesPanel::GenerateComponentRow)
								.OnSelectionChanged(this, &SGrungifyZonesPanel::OnComponentSelectionChanged)
								.HeaderRow(MakeComponentHeader())
							]
						]
					]

					+ SSplitter::Slot()
					.Value(0.45f)
					[
						SNew(SMultiLineEditableTextBox)
						.IsReadOnly(true)
						.AutoWrapText(true)
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
						.Text_Lambda([this]() { return DetailsText; })
					]
				]
			]
		];

		DetailsText = LOCTEXT("SelectZone", "Select a zone to see its last report.");
		RefreshZones();

		RegisterActiveTimer(1.f, FWidgetActiveTimerDelegate::CreateSP(this, &SGrungifyZonesPanel::OnRefreshTimer));
		FEditorDelegates::MapChange.AddSP(this, &SGrungifyZonesPanel::OnMapChange);
		FEditorDelegates::OnMapOpened.AddSP(this, &SGrungifyZonesPanel::OnMapOpened);
	}

	virtual ~SGrungifyZonesPanel() override
	{
		FEditorDelegates::MapChange.RemoveAll(this);
		FEditorDelegates::OnMapOpened.RemoveAll(this);
	}

	void RunZoneAction(const FZoneItemPtr& Item, EPanelAction Action)
	{
		AGrungifyWeatheringZone* Zone = Item.IsValid() ? Item->Zone.Get() : nullptr;
		if (!IsValid(Zone))
		{
			RefreshZones();
			return;
		}
		{
			const FScopedTransaction Transaction(FText::Format(LOCTEXT("ZoneTransaction", "Grungify: {0} {1}"), ActionName(Action), FText::FromString(Item->Label)));
			RunAction(Zone, Action);
		}
		RefreshDetails(/*bForce*/ true);
	}

	void SelectInViewport(const FZoneItemPtr& Item)
	{
		AGrungifyWeatheringZone* Zone = Item.IsValid() ? Item->Zone.Get() : nullptr;
		if (!GEditor || !IsValid(Zone))
		{
			return;
		}
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true, /*WarnAboutManyActors*/ false);
		GEditor->SelectActor(Zone, /*bInSelected*/ true, /*bNotify*/ true, /*bSelectEvenIfHidden*/ true);
		GEditor->MoveViewportCamerasToActor(*Zone, /*bActiveViewportOnly*/ false);

		if (ZoneList.IsValid() && FilteredItems.Contains(Item))
		{
			ZoneList->SetSelection(Item, ESelectInfo::Direct);
		}
	}

	static FString ReportFilePath(const AGrungifyWeatheringZone* Zone)
	{
		if (!IsValid(Zone) || Zone->GetLastReportFile().IsEmpty())
		{
			return FString();
		}
		const FString Path = FPaths::ConvertRelativePathToFull(Zone->GetLastReportFile());
		return FPaths::FileExists(Path) ? Path : FString();
	}

	static void OpenReport(const FZoneItemPtr& Item)
	{
		const FString Path = ReportFilePath(Item.IsValid() ? Item->Zone.Get() : nullptr);
		if (!Path.IsEmpty())
		{
			FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
		}
	}

private:
	TArray<FZoneItemPtr> AllItems;
	TArray<FZoneItemPtr> FilteredItems;
	TSharedPtr<SListView<FZoneItemPtr>> ZoneList;
	FString FilterString;

	FZoneItemPtr SelectedZone;
	FString DetailsSignature;
	FGrungifyRunReport DetailsReport;
	TArray<FComponentItemPtr> ComponentItems;
	TSharedPtr<SListView<FComponentItemPtr>> ComponentList;
	FComponentItemPtr SelectedComponent;
	FText DetailsText;

	TSharedRef<SWidget> MakeToolbar()
	{
		auto MakeAllButton = [this](const FText& Label, const FText& ToolTip, EPanelAction Action) -> TSharedRef<SWidget>
		{
			return SNew(SButton)
				.Text(Label)
				.ToolTipText(ToolTip)
				.OnClicked_Lambda([this, Action]()
				{
					RunOnAllZones(Action);
					return FReply::Handled();
				});
		};

		return SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.f, 0.f, 2.f, 0.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "Refresh"))
				.ToolTipText(LOCTEXT("RefreshTip", "Rescan the level for weathering zones."))
				.OnClicked_Lambda([this]()
				{
					RefreshZones();
					RefreshDetails(/*bForce*/ true);
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
			[
				MakeAllButton(LOCTEXT("DryRunAll", "Dry Run All"), LOCTEXT("DryRunAllTip", "Report what every zone in the level would do, without touching any mesh."), EPanelAction::DryRun)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
			[
				MakeAllButton(LOCTEXT("PreviewAll", "Preview All"), LOCTEXT("PreviewAllTip", "Preview every weathering zone in the level."), EPanelAction::Preview)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
			[
				MakeAllButton(LOCTEXT("ApplyAll", "Apply All"), LOCTEXT("ApplyAllTip", "Apply every weathering zone in the level (one undo step)."), EPanelAction::Apply)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
			[
				MakeAllButton(LOCTEXT("CancelAll", "Cancel All Previews"), LOCTEXT("CancelAllTip", "Drop every zone's preview and go back to the applied state."), EPanelAction::CancelPreview)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
			[
				MakeAllButton(LOCTEXT("ClearAll", "Clear All"), LOCTEXT("ClearAllTip", "Remove the weathering from every zone in the level (one undo step)."), EPanelAction::Clear)
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("FilterHint", "Filter zones by label"))
				.OnTextChanged(this, &SGrungifyZonesPanel::OnFilterTextChanged)
			];
	}

	TSharedRef<SHeaderRow> MakeZoneHeader() const
	{
		return SNew(SHeaderRow)
			+ SHeaderRow::Column(ColZone).DefaultLabel(LOCTEXT("ColZone", "Zone")).FillWidth(0.18f)
			+ SHeaderRow::Column(ColStatus).DefaultLabel(LOCTEXT("ColStatus", "Status")).FillWidth(0.26f)
			+ SHeaderRow::Column(ColPreview).DefaultLabel(LOCTEXT("ColPreview", "Preview")).ManualWidth(90.f)
			+ SHeaderRow::Column(ColApplied).DefaultLabel(LOCTEXT("ColApplied", "Applied")).ManualWidth(60.f)
			+ SHeaderRow::Column(ColLastRun).DefaultLabel(LOCTEXT("ColLastRun", "Last run")).FillWidth(0.26f)
			+ SHeaderRow::Column(ColWarnings).DefaultLabel(LOCTEXT("ColWarnings", "Warnings")).ManualWidth(70.f)
			+ SHeaderRow::Column(ColPreset).DefaultLabel(LOCTEXT("ColPreset", "Preset")).FillWidth(0.14f)
			+ SHeaderRow::Column(ColActions).DefaultLabel(LOCTEXT("ColActions", "")).ManualWidth(290.f);
	}

	TSharedRef<SHeaderRow> MakeComponentHeader() const
	{
		return SNew(SHeaderRow)
			+ SHeaderRow::Column(ColComponent).DefaultLabel(LOCTEXT("ColComponent", "Actor / Component")).FillWidth(0.4f)
			+ SHeaderRow::Column(ColResult).DefaultLabel(LOCTEXT("ColResult", "Result")).ManualWidth(70.f)
			+ SHeaderRow::Column(ColNotes).DefaultLabel(LOCTEXT("ColNotes", "Notes")).FillWidth(0.6f)
			+ SHeaderRow::Column(ColTexture).DefaultLabel(LOCTEXT("ColTexture", "Texture")).ManualWidth(60.f)
			+ SHeaderRow::Column(ColSeconds).DefaultLabel(LOCTEXT("ColSeconds", "Seconds")).ManualWidth(65.f)
			+ SHeaderRow::Column(ColCache).DefaultLabel(LOCTEXT("ColCache", "Cached")).ManualWidth(55.f);
	}

	TSharedRef<ITableRow> GenerateZoneRow(FZoneItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
	{
		return SNew(SGrungifyZoneRow, OwnerTable, Item, SharedThis(this));
	}

	TSharedRef<ITableRow> GenerateComponentRow(FComponentItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
	{
		return SNew(SGrungifyComponentRow, OwnerTable, Item);
	}

	EActiveTimerReturnType OnRefreshTimer(double InCurrentTime, float InDeltaTime)
	{
		RefreshZones();
		RefreshDetails(/*bForce*/ false);
		return EActiveTimerReturnType::Continue;
	}

	EActiveTimerReturnType OnDeferredRefresh(double InCurrentTime, float InDeltaTime)
	{
		RefreshZones();
		RefreshDetails(/*bForce*/ true);
		return EActiveTimerReturnType::Stop;
	}

	void OnMapChange(uint32 MapChangeFlags)
	{
		ScheduleRefreshAfterMapChange();
	}

	void OnMapOpened(const FString& Filename, bool bAsTemplate)
	{
		ScheduleRefreshAfterMapChange();
	}

	// The old world may still be tearing down when these fire: drop everything now and rescan on the next tick.
	void ScheduleRefreshAfterMapChange()
	{
		AllItems.Reset();
		FilteredItems.Reset();
		if (ZoneList.IsValid())
		{
			ZoneList->ClearSelection();
			ZoneList->RequestListRefresh();
		}
		SetSelectedZone(nullptr);
		RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SGrungifyZonesPanel::OnDeferredRefresh));
	}

	void OnFilterTextChanged(const FText& Text)
	{
		FilterString = Text.ToString().TrimStartAndEnd();
		ApplyFilter();
	}

	void RefreshZones()
	{
		TArray<FZoneItemPtr> Previous = MoveTemp(AllItems);
		AllItems.Reset();

		if (UWorld* World = EditorWorld())
		{
			for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
			{
				AGrungifyWeatheringZone* Zone = *It;
				if (!IsValid(Zone))
				{
					continue;
				}

				// Reuse the existing item so the list keeps its selection and rows.
				FZoneItemPtr Item;
				if (const FZoneItemPtr* Existing = Previous.FindByPredicate([Zone](const FZoneItemPtr& Candidate) { return Candidate.IsValid() && Candidate->Zone.Get() == Zone; }))
				{
					Item = *Existing;
				}
				else
				{
					Item = MakeShared<FZoneItem>();
					Item->Zone = Zone;
				}
				Item->Label = Zone->GetActorLabel();
				AllItems.Add(Item);
			}
		}

		AllItems.Sort([](const FZoneItemPtr& A, const FZoneItemPtr& B) { return A->Label < B->Label; });
		ApplyFilter();
	}

	void ApplyFilter()
	{
		TArray<FZoneItemPtr> NewFiltered;
		for (const FZoneItemPtr& Item : AllItems)
		{
			if (Item.IsValid() && (FilterString.IsEmpty() || Item->Label.Contains(FilterString)))
			{
				NewFiltered.Add(Item);
			}
		}

		if (NewFiltered != FilteredItems)
		{
			FilteredItems = MoveTemp(NewFiltered);
			if (ZoneList.IsValid())
			{
				ZoneList->RequestListRefresh();
			}
		}

		if (SelectedZone.IsValid() && !FilteredItems.Contains(SelectedZone))
		{
			if (ZoneList.IsValid())
			{
				ZoneList->ClearSelection();
			}
			SetSelectedZone(nullptr);
		}
		else if (SelectedZone.IsValid() && ZoneList.IsValid() && !ZoneList->IsItemSelected(SelectedZone))
		{
			ZoneList->SetSelection(SelectedZone, ESelectInfo::Direct);
		}
	}

	void RunOnAllZones(EPanelAction Action)
	{
		// Pick up zones placed since the last tick.
		RefreshZones();

		TArray<TWeakObjectPtr<AGrungifyWeatheringZone>> Zones;
		for (const FZoneItemPtr& Item : AllItems)
		{
			if (Item.IsValid() && Item->Zone.IsValid())
			{
				Zones.Add(Item->Zone);
			}
		}
		if (Zones.Num() == 0)
		{
			Notify(LOCTEXT("NoZonesNotify", "No Grungify weathering zones in this level."));
			return;
		}

		{
			const FScopedTransaction Transaction(LOCTEXT("AllZones", "Grungify: All Zones"));
			for (const TWeakObjectPtr<AGrungifyWeatheringZone>& Zone : Zones)
			{
				RunAction(Zone.Get(), Action);
			}
		}
		Notify(FText::Format(LOCTEXT("ZonesDone", "Grungify: processed {0} zone(s). Reports are in Saved/Grungify/Reports."), Zones.Num()));
		RefreshDetails(/*bForce*/ true);
	}

	void OnZoneSelectionChanged(FZoneItemPtr Item, ESelectInfo::Type SelectInfo)
	{
		SetSelectedZone(Item);
	}

	void SetSelectedZone(const FZoneItemPtr& Item)
	{
		if (Item == SelectedZone)
		{
			return;
		}
		SelectedZone = Item;
		SelectedComponent.Reset();
		RefreshDetails(/*bForce*/ true);
	}

	void OnComponentSelectionChanged(FComponentItemPtr Item, ESelectInfo::Type SelectInfo)
	{
		SelectedComponent = Item;
		UpdateDetailsText();
	}

	void RefreshDetails(bool bForce)
	{
		AGrungifyWeatheringZone* Zone = SelectedZone.IsValid() ? SelectedZone->Zone.Get() : nullptr;
		if (!IsValid(Zone))
		{
			if (bForce || !DetailsSignature.IsEmpty() || ComponentItems.Num() > 0)
			{
				DetailsSignature.Reset();
				DetailsReport = FGrungifyRunReport();
				ComponentItems.Reset();
				SelectedComponent.Reset();
				if (ComponentList.IsValid())
				{
					ComponentList->ClearSelection();
					ComponentList->RequestListRefresh();
				}
				DetailsText = SelectedZone.IsValid()
					? LOCTEXT("ZoneGone", "This zone no longer exists.")
					: LOCTEXT("SelectZone", "Select a zone to see its last report.");
			}
			return;
		}

		const FString Signature = ReportSignature(Zone);
		if (!bForce && Signature == DetailsSignature)
		{
			return;
		}
		DetailsSignature = Signature;
		DetailsReport = Zone->GetLastReport();

		const FString PreviousKey = SelectedComponent.IsValid() ? ComponentKey(SelectedComponent->Report) : FString();
		SelectedComponent.Reset();
		ComponentItems.Reset();
		FComponentItemPtr Reselect;
		for (const FGrungifyComponentReport& Component : DetailsReport.Components)
		{
			FComponentItemPtr Item = MakeShared<FComponentItem>();
			Item->Report = Component;
			if (!Reselect.IsValid() && !PreviousKey.IsEmpty() && ComponentKey(Component) == PreviousKey)
			{
				Reselect = Item;
			}
			ComponentItems.Add(Item);
		}

		if (ComponentList.IsValid())
		{
			ComponentList->ClearSelection();
			ComponentList->RequestListRefresh();
			if (Reselect.IsValid())
			{
				ComponentList->SetSelection(Reselect, ESelectInfo::Direct);
			}
		}
		SelectedComponent = Reselect;
		UpdateDetailsText();
	}

	void UpdateDetailsText()
	{
		if (!SelectedZone.IsValid())
		{
			DetailsText = LOCTEXT("SelectZone", "Select a zone to see its last report.");
			return;
		}
		const FGrungifyComponentReport* Component = SelectedComponent.IsValid() ? &SelectedComponent->Report : nullptr;
		DetailsText = FText::FromString(BuildDetailsText(DetailsReport, Component));
	}
};

TSharedRef<SWidget> SGrungifyZoneRow::GenerateWidgetForColumn(const FName& ColumnName)
{
	if (!Item.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	if (ColumnName == ColZone)
	{
		return MakeText(TAttribute<FText>::CreateLambda([this]()
		{
			return GetZone() ? FText::FromString(Item->Label) : FText::Format(LOCTEXT("ZoneRemoved", "{0} (removed)"), FText::FromString(Item->Label));
		}));
	}

	if (ColumnName == ColStatus)
	{
		auto StatusText = [this]()
		{
			const AGrungifyWeatheringZone* Zone = GetZone();
			return Zone ? FText::FromString(Zone->Status) : FText::GetEmpty();
		};
		return SNew(SBox)
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.f, 0.f))
			[
				SNew(STextBlock)
				.Text_Lambda(StatusText)
				.ToolTipText_Lambda(StatusText)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			];
	}

	if (ColumnName == ColPreview)
	{
		return MakeText(TAttribute<FText>::CreateLambda([this]()
		{
			const AGrungifyWeatheringZone* Zone = GetZone();
			if (!Zone)
			{
				return FText::GetEmpty();
			}
			if (Zone->IsGeneratingInBackground())
			{
				return LOCTEXT("PreviewBackground", "background…");
			}
			return Zone->bPreviewActive
				? FText::Format(LOCTEXT("PreviewYes", "yes ({0})"), Zone->GetPreviewCount())
				: LOCTEXT("PreviewNo", "no");
		}));
	}

	if (ColumnName == ColApplied)
	{
		return MakeText(TAttribute<FText>::CreateLambda([this]()
		{
			const AGrungifyWeatheringZone* Zone = GetZone();
			return Zone ? FText::AsNumber(Zone->GetAppliedCount()) : FText::GetEmpty();
		}));
	}

	if (ColumnName == ColLastRun)
	{
		auto LastRunText = [this]()
		{
			const AGrungifyWeatheringZone* Zone = GetZone();
			return Zone ? FText::FromString(LastRunString(Zone->GetLastReport())) : FText::GetEmpty();
		};
		return SNew(SBox)
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.f, 0.f))
			[
				SNew(STextBlock)
				.Text_Lambda(LastRunText)
				.ToolTipText_Lambda(LastRunText)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			];
	}

	if (ColumnName == ColWarnings)
	{
		return MakeText(TAttribute<FText>::CreateLambda([this]()
		{
			const AGrungifyWeatheringZone* Zone = GetZone();
			return Zone ? FText::AsNumber(CountWarnings(Zone->GetLastReport())) : FText::GetEmpty();
		}));
	}

	if (ColumnName == ColPreset)
	{
		return MakeText(TAttribute<FText>::CreateLambda([this]()
		{
			const AGrungifyWeatheringZone* Zone = GetZone();
			if (!Zone)
			{
				return FText::GetEmpty();
			}
			const UGrungifyPreset* Preset = Zone->Preset.Get();
			return IsValid(Preset) ? FText::FromString(Preset->GetName()) : LOCTEXT("NoPreset", "-");
		}));
	}

	if (ColumnName == ColActions)
	{
		const FZoneItemPtr RowItem = Item;
		const TWeakPtr<SGrungifyZonesPanel> WeakPanel = Panel;
		auto Run = [RowItem, WeakPanel](EPanelAction Action)
		{
			return [RowItem, WeakPanel, Action]()
			{
				if (const TSharedPtr<SGrungifyZonesPanel> PinnedPanel = WeakPanel.Pin())
				{
					PinnedPanel->RunZoneAction(RowItem, Action);
				}
			};
		};
		const TAttribute<bool> HasZone = TAttribute<bool>::CreateLambda([this]() { return GetZone() != nullptr; });

		return SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().Padding(1.f)
			[
				MakeButton(LOCTEXT("Select", "Select"), LOCTEXT("SelectTip", "Select this zone and frame it in the viewports."),
					[RowItem, WeakPanel]()
					{
						if (const TSharedPtr<SGrungifyZonesPanel> PinnedPanel = WeakPanel.Pin())
						{
							PinnedPanel->SelectInViewport(RowItem);
						}
					}, HasZone)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(1.f)
			[
				MakeButton(LOCTEXT("RowPreview", "Preview"), LOCTEXT("RowPreviewTip", "Preview this zone."), Run(EPanelAction::Preview), HasZone)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(1.f)
			[
				MakeButton(LOCTEXT("RowApply", "Apply"), LOCTEXT("RowApplyTip", "Apply this zone (undoable)."), Run(EPanelAction::Apply), HasZone)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(1.f)
			[
				MakeButton(LOCTEXT("RowClear", "Clear"), LOCTEXT("RowClearTip", "Remove this zone's weathering (undoable)."), Run(EPanelAction::Clear), HasZone)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(1.f)
			[
				MakeButton(LOCTEXT("RowReport", "Report"), LOCTEXT("RowReportTip", "Open the last JSON report in the default application."),
					[RowItem]() { SGrungifyZonesPanel::OpenReport(RowItem); },
					TAttribute<bool>::CreateLambda([this]() { return !SGrungifyZonesPanel::ReportFilePath(GetZone()).IsEmpty(); }))
			];
	}

	return SNullWidget::NullWidget;
}
} // namespace GrungifyZonesPanelPrivate

namespace GrungifyZonesPanel
{
	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(GrungifyZonesPanelPrivate::SGrungifyZonesPanel)
			];
	}

	void Register()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		// Hidden from the Window menu: the module adds its own menu entries that call Open().
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateStatic(&SpawnTab))
			.SetDisplayName(LOCTEXT("TabTitle", "Grungify Zones"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Every Grungify weathering zone in the level: status, last report and actions."))
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Outliner"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);
	}

	void Unregister()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
		}
	}

	void Open()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->TryInvokeTab(TabId);
		}
	}
}

#undef LOCTEXT_NAMESPACE
