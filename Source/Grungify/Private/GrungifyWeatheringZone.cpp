#include "GrungifyWeatheringZone.h"

#include "Components/ArrowComponent.h"
#include "GrungifyZoneComponent.h"

#if WITH_EDITOR
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "GrungifyGenerator.h"
#include "GrungifySettings.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#endif

#define LOCTEXT_NAMESPACE "GrungifyWeatheringZone"

AGrungifyWeatheringZone::AGrungifyWeatheringZone()
{
	PrimaryActorTick.bCanEverTick = false;

	ZoneComponent = CreateDefaultSubobject<UGrungifyZoneComponent>(TEXT("Zone"));
	RootComponent = ZoneComponent;

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
	RestorePreview();

	TArray<FGrungifyComponentResult> Results;
	TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*> TakeOvers;
	GenerateResults(TEXT("Preview"), Results, TakeOvers);

	for (const FGrungifyComponentResult& Result : Results)
	{
		FGrungifyComponentRecord Record;
		Grungify::CaptureRecord(Result.Component, Record);
		const TArray<float>& Original = OriginalCustomDataFor(Result.Component, Record.CustomData);
		if (WriteResult(Result, PreviewDisplay, Original))
		{
			PreviewRecords.Add(MoveTemp(Record));
		}
		else if (LastReport.Components.IsValidIndex(Result.ReportIndex))
		{
			LastReport.Components[Result.ReportIndex].bCustomDataConflict = true;
			LastReport.Components[Result.ReportIndex].Result = TEXT("skipped");
		}
	}

	bPreviewActive = PreviewRecords.Num() > 0;
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
	RestorePreview();

	TArray<FGrungifyComponentResult> Results;
	TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*> TakeOvers;
	GenerateResults(TEXT("Apply"), Results, TakeOvers);

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
			Grungify::RestoreRecord(AppliedRecords[Index]);
			AppliedRecords.RemoveAt(Index);
		}
	}

	for (const FGrungifyComponentResult& Result : Results)
	{
		int32 RecordIndex = AppliedRecords.IndexOfByPredicate([&Result](const FGrungifyComponentRecord& Record) { return Record.Component.Get() == Result.Component; });
		bool bNewRecord = false;

		if (RecordIndex != INDEX_NONE)
		{
			// Start again from the original so nothing from the previous Apply lingers.
			Grungify::RestoreRecord(AppliedRecords[RecordIndex]);
		}
		else
		{
			FGrungifyComponentRecord Record;
			AGrungifyWeatheringZone* const* PreviousOwner = TakeOvers.Find(Result.Component);
			if (PreviousOwner && *PreviousOwner && (*PreviousOwner)->ReleaseAppliedRecord(Result.Component, Record))
			{
				Grungify::RestoreRecord(Record);
			}
			else
			{
				Grungify::CaptureRecord(Result.Component, Record);
			}
			RecordIndex = AppliedRecords.Add(MoveTemp(Record));
			bNewRecord = true;
		}

		if (!WriteResult(Result, EGrungifyPreviewDisplay::FinalMaterial, AppliedRecords[RecordIndex].CustomData))
		{
			if (bNewRecord)
			{
				AppliedRecords.RemoveAt(RecordIndex);
			}
			if (LastReport.Components.IsValidIndex(Result.ReportIndex))
			{
				LastReport.Components[Result.ReportIndex].bCustomDataConflict = true;
				LastReport.Components[Result.ReportIndex].Result = TEXT("skipped");
			}
		}
	}

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
	RestorePreview();

	int32 NumCleared = 0;
	for (int32 Index = AppliedRecords.Num() - 1; Index >= 0; --Index)
	{
		// Meshes that aren't loaded keep their record so a later Clear can still restore them.
		if (AppliedRecords[Index].Component.Get())
		{
			Grungify::RestoreRecord(AppliedRecords[Index]);
			AppliedRecords.RemoveAt(Index);
			++NumCleared;
		}
	}

	Status = AppliedRecords.Num() > 0
		? FString::Printf(TEXT("Cleared %d mesh(es). %d are not loaded; load them and Clear again."), NumCleared, AppliedRecords.Num())
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
	RestorePreview();
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
	GenerateResults(TEXT("DryRun"), Results, TakeOvers);

	// Flag the custom data conflicts a real run would hit.
	for (const FGrungifyComponentResult& Result : Results)
	{
		const TArray<float>& Original = OriginalCustomDataFor(Result.Component, Result.Component->GetDefaultCustomPrimitiveData().Data);
		if (!Grungify::CustomDataSlotsFree(Original) && LastReport.Components.IsValidIndex(Result.ReportIndex))
		{
			LastReport.Components[Result.ReportIndex].bCustomDataConflict = true;
			LastReport.Components[Result.ReportIndex].Result = TEXT("skipped");
		}
	}

	FinishReport(Start);
#endif
}

#if WITH_EDITOR

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
		Grungify::RestoreRecord(PreviewRecords[Index], /*bTransactional*/ false);
	}
	PreviewRecords.Reset();
	bPreviewActive = false;

	Status = FString::Printf(TEXT("Preview discarded (%s). Press Preview to see it again."), Reason);
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
	OutRecord = MoveTemp(AppliedRecords[Index]);
	AppliedRecords.RemoveAt(Index);
	Status = FString::Printf(TEXT("%s was taken over by another zone."), *Component->GetName());
	return true;
}

// ---------------------------------------------------------------- Internals

void AGrungifyWeatheringZone::RestorePreview()
{
	for (int32 Index = PreviewRecords.Num() - 1; Index >= 0; --Index)
	{
		Grungify::RestoreRecord(PreviewRecords[Index]);
	}
	PreviewRecords.Reset();
	bPreviewActive = false;
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
	const bool bComponentRail = Grungify::IsRail(Component);
	const UEnum* SurfaceEnum = StaticEnum<EGrungifySurfaceType>();
	const int32 NumSlots = FMath::Max(Component->GetNumMaterials(), 1);
	OutContext.Slots.SetNum(NumSlots);

	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		FGrungifySlotInfo& Info = OutContext.Slots[Slot];
		const FGrungifySlotRule* Rule = SlotRules.FindByPredicate([Component, Slot](const FGrungifySlotRule& Candidate)
		{
			return Grungify::RuleMatchesSlot(Candidate, Component, Slot);
		});

		Info.Surface = (Rule && Rule->Surface != EGrungifySurfaceType::Auto) ? Rule->Surface : Grungify::ClassifySlot(Component, Slot, SurfaceType);
		switch (Info.Surface)
		{
		case EGrungifySurfaceType::Metal: Info.Profile = MetalProfile; break;
		case EGrungifySurfaceType::Trim:  Info.Profile = TrimProfile; break;
		default:                          Info.Profile = StoneProfile; break;
		}

		Info.bRail = bComponentRail;
		if (Rule)
		{
			Info.bExcluded = Rule->bExclude;
			if (Rule->bOverrideProfile)
			{
				Info.Profile = Rule->Profile;
			}
			if (Rule->Rail != EGrungifyRailMode::Inherit)
			{
				Info.bRail = Rule->Rail == EGrungifyRailMode::Rail;
			}

			Info.Gate = &Rule->Gate;
			if (UTexture2D* GateTexture = Rule->Gate.GateTexture)
			{
				TUniquePtr<FGrungifyGateSampler>& Sampler = Samplers.FindOrAdd(GateTexture);
				if (!Sampler.IsValid())
				{
					Sampler = MakeUnique<FGrungifyGateSampler>();
					FString Error;
					if (!Sampler->Init(GateTexture, Error))
					{
						LastReport.Warnings.AddUnique(FString::Printf(TEXT("Rule '%s': %s. Slots using it are left untouched."), *Rule->Label, *Error));
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
				}
			}
		}

		const UMaterialInterface* Material = Component->GetMaterial(Slot);
		Info.Description = FString::Printf(TEXT("%d %s -> %s%s%s%s"),
			Slot,
			Material ? *Material->GetName() : TEXT("(none)"),
			*SurfaceEnum->GetNameStringByValue(int64(Info.Surface)),
			Rule ? *FString::Printf(TEXT(" [rule '%s']"), *Rule->Label) : TEXT(""),
			Info.bRail ? TEXT(" [rail]") : TEXT(""),
			Info.bExcluded ? TEXT(" [excluded]") : TEXT(""));
		Report.Slots.Add(Info.Description);
	}
}

bool AGrungifyWeatheringZone::ShouldConsiderActor(const AActor* Actor, const FBox& ZoneBox) const
{
	if (!Actor || Actor == this || Actor->IsA<AGrungifyWeatheringZone>() || Actor->IsEditorOnly() || Actor->IsHiddenEd())
	{
		return false;
	}
	if (Grungify::HasIgnoreTag(Actor, nullptr))
	{
		return false;
	}
	if (ExcludedActors.ContainsByPredicate([Actor](const TSoftObjectPtr<AActor>& Excluded) { return Excluded.Get() == Actor; }))
	{
		return false;
	}
	if (IncludeOnlyActors.Num() > 0 && !IncludeOnlyActors.ContainsByPredicate([Actor](const TSoftObjectPtr<AActor>& Included) { return Included.Get() == Actor; }))
	{
		return false;
	}
	const FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
	return ActorBox.IsValid && ActorBox.Intersect(ZoneBox);
}

const TArray<float>& AGrungifyWeatheringZone::OriginalCustomDataFor(const UStaticMeshComponent* Component, const TArray<float>& Current) const
{
	// Whether the slots are free is decided by the pre-Grungify values, not by values this or another zone wrote.
	if (const FGrungifyComponentRecord* Own = FindAppliedRecord(Component))
	{
		return Own->CustomData;
	}
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
		{
			if (*It != this)
			{
				if (const FGrungifyComponentRecord* Other = It->FindAppliedRecord(Component))
				{
					return Other->CustomData;
				}
			}
		}
	}
	return Current;
}

void AGrungifyWeatheringZone::GenerateResults(const TCHAR* Operation, TArray<FGrungifyComponentResult>& OutResults, TMap<UStaticMeshComponent*, AGrungifyWeatheringZone*>& OutTakeOvers)
{
	UWorld* World = GetWorld();
	const FBox ZoneBox = ZoneComponent->Bounds.GetBox();

	LastReport = FGrungifyRunReport();
	LastReport.Operation = Operation;
	LastReport.Zone = GetActorNameOrLabel();
	LastReport.Seed = Seed;
	LastReport.Time = FDateTime::Now().ToIso8601();

	FGrungifyFloorField Floor;
	if (FloorMode == EGrungifyFloorMode::Trace)
	{
		Floor.BuildTraced(World, ZoneBox, FloorProbeSpacingCm, this);
	}
	else
	{
		Floor.BuildFlat(ZoneBox.Min.Z);
	}

	RefreshRunoffSources();
	LastReport.RunoffSources = CachedRunoffSources.Num();

	FGrungifyGenerationParams Params;
	BuildParams(Params);
	Params.Floor = &Floor;
	Params.Sources = CachedRunoffSources;

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

	TMap<UTexture2D*, TUniquePtr<FGrungifyGateSampler>> Samplers;

	auto Skip = [this](FGrungifyComponentReport& Report, const FString& Reason)
	{
		Report.Result = TEXT("skipped");
		Report.Notes = Reason;
		LastReport.Components.Add(MoveTemp(Report));
	};

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!ShouldConsiderActor(Actor, ZoneBox))
		{
			continue;
		}

		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (UStaticMeshComponent* Component : Components)
		{
			if (!Component || !Component->IsRegistered() || Component->IsEditorOnly() || !Component->GetStaticMesh())
			{
				continue;
			}
			if (Grungify::HasIgnoreTag(nullptr, Component) || !Component->Bounds.GetBox().Intersect(ZoneBox))
			{
				continue;
			}
			if (RequiredComponentTags.Num() > 0 && !RequiredComponentTags.ContainsByPredicate([Component](const FName& Tag) { return Component->ComponentHasTag(Tag); }))
			{
				continue;
			}

			FGrungifyComponentReport Report;
			Report.Actor = Actor->GetActorNameOrLabel();
			Report.Component = Component->GetName();
			Report.Mesh = Component->GetStaticMesh()->GetName();

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
			if (AGrungifyWeatheringZone* const* Owner = AppliedBy.Find(Component))
			{
				Report.PreviousOwner = (*Owner)->GetActorNameOrLabel();
				if (OverlapPolicy == EGrungifyOverlapPolicy::SkipOwned)
				{
					Skip(Report, FString::Printf(TEXT("owned by %s (set Overlap Policy to Take Over)"), *Report.PreviousOwner));
					continue;
				}
				OutTakeOvers.Add(Component, *Owner);
			}

			FGrungifyComponentContext Context;
			BuildContext(Component, Samplers, Context, Report);
			if (!Context.Slots.ContainsByPredicate([](const FGrungifySlotInfo& Slot) { return !Slot.bExcluded; }))
			{
				OutTakeOvers.Remove(Component);
				Skip(Report, TEXT("every slot excluded by rules"));
				continue;
			}

			FGrungifyComponentResult Result;
			if (!Grungify::GenerateComponent(Component, Context, Params, Result))
			{
				OutTakeOvers.Remove(Component);
				Skip(Report, TEXT("no readable mesh data"));
				continue;
			}

			Report.Result = Result.Output == EGrungifyMaskOutput::Texture ? TEXT("texture") : TEXT("vertex");
			Report.Notes = Result.Notes.TrimEnd();
			Report.Vertices = Result.Vertices;
			Report.TextureSize = Result.TextureSize;
			Report.MeanMasks = Result.MeanMasks;
			Report.MaxMasks = Result.MaxMasks;
			Report.bNanite = Result.bNanite;

			Result.ReportIndex = LastReport.Components.Add(MoveTemp(Report));
			OutResults.Add(MoveTemp(Result));
		}
	}
}

bool AGrungifyWeatheringZone::WriteResult(const FGrungifyComponentResult& Result, EGrungifyPreviewDisplay Display, const TArray<float>& OriginalCustomData) const
{
	UStaticMeshComponent* Component = Result.Component;

	// Never overwrite custom data another system put on this mesh. Without our slots the material path stays
	// inactive, so don't write masks either.
	if (!Grungify::CustomDataSlotsFree(OriginalCustomData))
	{
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
		}
	}

	const bool bTexture = Result.Output == EGrungifyMaskOutput::Texture;
	if (bTexture)
	{
		if (UTexture2D* Texture = Grungify::CreateMaskTexture(Component, Result.TextureSize, Result.TexturePixels))
		{
			Grungify::BindMaskTexture(Component, Texture, DebugMaterial);
		}
	}
	else
	{
		Grungify::WriteVertexColors(Component, Result.LODColors);
		if (DebugMaterial)
		{
			Grungify::SwapMaterials(Component, DebugMaterial);
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
	CustomData[Grungify::CPD_MaskMode] = bTexture ? 2.f : 1.f;
	CustomData[Grungify::CPD_SurfaceType] = float(int32(Result.DominantSurface) - 1);
	CustomData[Grungify::CPD_FeatureScale] = FeatureScaleCm;
	// Scramble the seed into a stable 0..1000 offset so the shader detail changes with the seed too.
	CustomData[Grungify::CPD_SeedOffset] = float((uint32(Seed) * 2654435761u) % 100000u) * 0.01f;
	CustomData[Grungify::CPD_DebugChannel] = DebugChannel;
	Grungify::WriteCustomData(Component, CustomData);

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

	const TCHAR* Verb = LastReport.Operation == TEXT("Apply") ? TEXT("Applied") : LastReport.Operation == TEXT("Preview") ? TEXT("Previewing") : TEXT("Dry run");
	Status = FString::Printf(TEXT("%s: %d mesh(es) (%d vertex, %d texture), %d skipped, %d runoff source(s), seed %d, %.2fs. See Last Report."),
		Verb, LastReport.Weathered, NumVertex, NumTexture, LastReport.Skipped, LastReport.RunoffSources, Seed, LastReport.Seconds);
	UE_LOG(LogGrungify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
	for (const FString& Warning : LastReport.Warnings)
	{
		UE_LOG(LogGrungify, Warning, TEXT("%s: %s"), *GetActorNameOrLabel(), *Warning);
	}

	if (bWriteReportFiles)
	{
		FString Json;
		if (FJsonObjectConverter::UStructToJsonObjectString(LastReport, Json))
		{
			const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"), TEXT("Reports"),
				FPaths::MakeValidFileName(FString::Printf(TEXT("%s_%s.json"), *GetActorNameOrLabel(), *LastReport.Operation)));
			FFileHelper::SaveStringToFile(Json, *File);
		}
	}
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
