#include "GrungifyWeatheringZone.h"

#include "Components/ArrowComponent.h"
#include "GrungifyZoneComponent.h"

#if WITH_EDITOR
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "GrungifyGenerator.h"
#include "GrungifySettings.h"
#include "Materials/MaterialInterface.h"
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

// ------------------------------------------------------------------ Actions

void AGrungifyWeatheringZone::Preview()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	Modify();
	RestorePreview();

	TArray<FGrungifyComponentResult> Results;
	FString Summary;
	GenerateResults(Results, Summary);

	for (const FGrungifyComponentResult& Result : Results)
	{
		FGrungifyComponentRecord& Record = PreviewRecords.AddDefaulted_GetRef();
		Grungify::CaptureRecord(Result.Component, PreviewDisplay != EGrungifyPreviewDisplay::FinalMaterial, Record);
		WriteResult(Result, PreviewDisplay);
	}

	bPreviewActive = PreviewRecords.Num() > 0;
	Status = FString::Printf(TEXT("Previewing: %s"), *Summary);
	UE_LOG(LogGrungify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
#endif
}

void AGrungifyWeatheringZone::Apply()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("ApplyWeathering", "Grungify: Apply Weathering"));
	Modify();
	RestorePreview();

	TArray<FGrungifyComponentResult> Results;
	FString Summary;
	GenerateResults(Results, Summary);

	// Components this zone applied to before but no longer covers go back to their originals.
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
		const bool bKnown = AppliedRecords.ContainsByPredicate([&Result](const FGrungifyComponentRecord& Record) { return Record.Component.Get() == Result.Component; });
		if (!bKnown)
		{
			Grungify::CaptureRecord(Result.Component, /*bCaptureMaterials*/ false, AppliedRecords.AddDefaulted_GetRef());
		}
		WriteResult(Result, EGrungifyPreviewDisplay::FinalMaterial);
	}

	Status = FString::Printf(TEXT("Applied: %s"), *Summary);
	UE_LOG(LogGrungify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
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
		|| MemberName == GET_MEMBER_NAME_CHECKED(AGrungifyWeatheringZone, bPreviewActive))
	{
		return;
	}

	// Dragging a slider fires Interactive changes every frame; wait for the release.
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
		// Keep the viewport's vent gizmos current even when not previewing.
		RefreshRunoffSources();
	}
}

bool AGrungifyWeatheringZone::CanRunInEditorWorld() const
{
	const UWorld* World = GetWorld();
	return World && World->WorldType == EWorldType::Editor && !IsTemplate() && ZoneComponent;
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

	// Vents above the zone can still drip into it.
	FBox SearchBox = ZoneComponent->Bounds.GetBox();
	SearchBox.Max.Z += RunoffLengthCm;

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

	for (const TSoftObjectPtr<AActor>& Explicit : RunoffSources)
	{
		if (const AActor* Actor = Explicit.Get())
		{
			AddSource(Actor->GetComponentsBoundingBox(/*bNonColliding*/ true));
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
	P.FloorKickHeight = FloorKickHeightCm;

	P.Profiles[0] = StoneProfile;
	P.Profiles[1] = MetalProfile;
	P.Profiles[2] = TrimProfile;
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
	for (const TSoftObjectPtr<AActor>& Excluded : ExcludedActors)
	{
		if (Excluded.Get() == Actor)
		{
			return false;
		}
	}
	const FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
	return ActorBox.IsValid && ActorBox.Intersect(ZoneBox);
}

void AGrungifyWeatheringZone::GenerateResults(TArray<FGrungifyComponentResult>& OutResults, FString& OutSummary)
{
	UWorld* World = GetWorld();
	const FBox ZoneBox = ZoneComponent->Bounds.GetBox();

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

	FGrungifyGenerationParams Params;
	BuildParams(Params);
	Params.Floor = &Floor;
	Params.Sources = CachedRunoffSources;

	int32 NumSkippedInstanced = 0;
	int32 NumSkippedMovable = 0;
	int32 NumUnreadable = 0;
	int32 NumNanite = 0;
	int32 SurfaceCounts[3] = { 0, 0, 0 };

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
			// One set of vertex colours is shared by every instance, so instanced meshes can't be weathered per instance.
			if (Component->IsA<UInstancedStaticMeshComponent>())
			{
				++NumSkippedInstanced;
				continue;
			}
			if (!bIncludeMovable && Component->Mobility == EComponentMobility::Movable)
			{
				++NumSkippedMovable;
				continue;
			}

			FGrungifyComponentContext Context;
			Context.bIsRail = Grungify::IsRail(Component);
			Context.SlotSurfaces.SetNum(FMath::Max(Component->GetNumMaterials(), 1));
			for (int32 Slot = 0; Slot < Context.SlotSurfaces.Num(); ++Slot)
			{
				Context.SlotSurfaces[Slot] = Grungify::ClassifySlot(Component, Slot, SurfaceType);
			}

			FGrungifyComponentResult Result;
			if (!Grungify::GenerateComponent(Component, Context, Params, Result))
			{
				++NumUnreadable;
				continue;
			}

			NumNanite += Result.bNanite ? 1 : 0;
			++SurfaceCounts[FMath::Clamp(int32(Result.DominantSurface) - 1, 0, 2)];
			OutResults.Add(MoveTemp(Result));
		}
	}

	OutSummary = FString::Printf(TEXT("%d mesh(es) [stone %d, metal %d, trim %d], %d vent(s), seed %d."),
		OutResults.Num(), SurfaceCounts[0], SurfaceCounts[1], SurfaceCounts[2], CachedRunoffSources.Num(), Seed);
	if (NumSkippedInstanced > 0)
	{
		OutSummary += FString::Printf(TEXT(" Skipped %d instanced."), NumSkippedInstanced);
	}
	if (NumSkippedMovable > 0)
	{
		OutSummary += FString::Printf(TEXT(" Skipped %d movable."), NumSkippedMovable);
	}
	if (NumUnreadable > 0)
	{
		OutSummary += FString::Printf(TEXT(" %d had no readable mesh data."), NumUnreadable);
	}
	if (NumNanite > 0)
	{
		OutSummary += FString::Printf(TEXT(" %d Nanite mesh(es): painted vertex colours may only show on the fallback mesh."), NumNanite);
		UE_LOG(LogGrungify, Warning, TEXT("%s: %d Nanite mesh(es) in zone. Per-instance vertex colours may not render on Nanite; disable Nanite on those meshes or check the fallback."), *GetActorNameOrLabel(), NumNanite);
	}
}

void AGrungifyWeatheringZone::WriteResult(const FGrungifyComponentResult& Result, EGrungifyPreviewDisplay Display) const
{
	UStaticMeshComponent* Component = Result.Component;
	Grungify::WriteVertexColors(Component, Result.LODColors);

	float DebugChannel = 0.f;
	switch (Display)
	{
	case EGrungifyPreviewDisplay::EdgeChips: DebugChannel = 1.f; break;
	case EGrungifyPreviewDisplay::Dust:      DebugChannel = 2.f; break;
	case EGrungifyPreviewDisplay::Runoff:    DebugChannel = 3.f; break;
	case EGrungifyPreviewDisplay::Handling:  DebugChannel = 4.f; break;
	default: break;
	}

	// Scramble the seed into a stable 0..1000 offset so the shader detail changes with the seed too.
	const float SeedOffset = float((uint32(Seed) * 2654435761u) % 100000u) * 0.01f;

	float CustomData[Grungify::CPD_Count];
	CustomData[Grungify::CPD_SurfaceType] = float(int32(Result.DominantSurface) - 1);
	CustomData[Grungify::CPD_FeatureScale] = FeatureScaleCm;
	CustomData[Grungify::CPD_SeedOffset] = SeedOffset;
	CustomData[Grungify::CPD_DebugChannel] = DebugChannel;
	Grungify::WriteCustomData(Component, CustomData);

	if (Display != EGrungifyPreviewDisplay::FinalMaterial)
	{
		if (UMaterialInterface* DebugMaterial = UGrungifySettings::Get()->MaskDebugMaterial.LoadSynchronous())
		{
			Grungify::SwapMaterials(Component, DebugMaterial);
		}
		else
		{
			UE_LOG(LogGrungify, Warning, TEXT("Mask debug material not found. Run Tools > Grungify > Build Plugin Content."));
		}
	}

	Component->MarkRenderStateDirty();
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
