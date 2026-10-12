// Copyright Fallen Signal Studios. All Rights Reserved.
// Landscape weathering for AGrungifyWeatheringZone: a world-projected mask per landscape proxy under the zone, bound
// through a material override on each landscape component. Kept apart from the mesh path, which is per component.
#include "GrungifyWeatheringZone.h"

#if WITH_EDITOR

#include "CollisionQueryParams.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GrungifyGenerator.h"
#include "GrungifySettings.h"
#include "GrungifyZoneComponent.h"
#include "HAL/PlatformTime.h"
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInstanceConstant.h"

namespace
{
	/** At most this many texels per axis; the texel size grows on very large zones. */
	constexpr int32 MaxLandscapeTexels = 2048;

	UTexture2D* CreateLandscapeTexture(UObject* Outer, const TCHAR* BaseName, int32 SizeX, int32 SizeY, const TArray<FColor>& Pixels)
	{
		if (Pixels.Num() != SizeX * SizeY || SizeX <= 0 || SizeY <= 0)
		{
			return nullptr;
		}
		// Lives inside the landscape proxy, so it is saved in the same package as the components that use it
		// (World Partition keeps each actor in its own package).
		UTexture2D* Texture = NewObject<UTexture2D>(Outer, MakeUniqueObjectName(Outer, UTexture2D::StaticClass(), BaseName), RF_Transactional);
		Texture->Source.Init(SizeX, SizeY, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Default;
		Texture->LODGroup = TEXTUREGROUP_World;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->PostEditChange();
		return Texture;
	}

	/** Landscape components another zone has weathered or is previewing. */
	TSet<const UPrimitiveComponent*> ComponentsOwnedByOthers(UWorld* World, const AGrungifyWeatheringZone* Self,
		TFunctionRef<void(const AGrungifyWeatheringZone*, TSet<const UPrimitiveComponent*>&)> Collect)
	{
		TSet<const UPrimitiveComponent*> Owned;
		for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
		{
			if (*It != Self)
			{
				Collect(*It, Owned);
			}
		}
		return Owned;
	}
}

bool AGrungifyWeatheringZone::GenerateLandscapes(const FGrungifyGenerationJob& Job, const TFunction<bool()>& ShouldCancel)
{
	PendingLandscapes.Reset();
	UWorld* World = GetWorld();
	if (!bWeatherLandscape || !World || !ZoneComponent)
	{
		return true;
	}

	const FBox ZoneBox = ZoneComponent->Bounds.GetBox();
	TArray<ALandscapeProxy*> Proxies;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		if (ShouldConsiderActor(*It, ZoneBox))
		{
			Proxies.Add(*It);
		}
	}
	if (Proxies.Num() == 0)
	{
		return true;
	}

	struct FPhase
	{
		FGrungifyRunReport& Report;
		double Start = FPlatformTime::Seconds();
		int32 Items = 0;
		~FPhase()
		{
			FGrungifyPhaseTiming& Timing = Report.Phases.AddDefaulted_GetRef();
			Timing.Phase = TEXT("Landscape");
			Timing.Seconds = float(FPlatformTime::Seconds() - Start);
			Timing.Items = Items;
		}
	} Phase{ LastReport };

	FCollisionQueryParams Query(SCENE_QUERY_STAT(GrungifyLandscapeContact), /*bTraceComplex*/ false, this);
	for (ALandscapeProxy* Proxy : Proxies)
	{
		// Contact traces run parallel to the ground; terrain must never count as a wall.
		Query.AddIgnoredActor(Proxy);
	}

	for (ALandscapeProxy* Proxy : Proxies)
	{
		const double Start = FPlatformTime::Seconds();
		const FBox ProxyBox = Proxy->GetComponentsBoundingBox(true);
		const FVector2D Min(FMath::Max(ZoneBox.Min.X, ProxyBox.Min.X), FMath::Max(ZoneBox.Min.Y, ProxyBox.Min.Y));
		const FVector2D Max(FMath::Min(ZoneBox.Max.X, ProxyBox.Max.X), FMath::Min(ZoneBox.Max.Y, ProxyBox.Max.Y));
		const FVector2D Size = Max - Min;
		if (Size.X <= 1.0 || Size.Y <= 1.0)
		{
			continue;
		}

		TSharedPtr<FGrungifyLandscapeResult> Result = MakeShared<FGrungifyLandscapeResult>();
		Result->Proxy = Proxy;
		Result->ProxyPath = Proxy->GetPathName();
		Result->Min = Min;
		Result->Size = Size;
		const double Texel = FMath::Max(double(LandscapeTexelSizeCm), FMath::Max(Size.X, Size.Y) / MaxLandscapeTexels);
		Result->SizeX = FMath::Clamp(FMath::CeilToInt(Size.X / Texel), 1, MaxLandscapeTexels);
		Result->SizeY = FMath::Clamp(FMath::CeilToInt(Size.Y / Texel), 1, MaxLandscapeTexels);

		// Bounds and proxy path cannot detect a same-bounds sculpt or collision rebuild.
		// Recompute terrain masks until a complete height/contact/profile revision key exists;
		// the separate per-mesh cache is unaffected.

		const int32 Count = Result->SizeX * Result->SizeY;
		const double StepX = Size.X / Result->SizeX;
		const double StepY = Size.Y / Result->SizeY;
		Result->Positions.SetNumUninitialized(Count);
		Result->Normals.SetNumUninitialized(Count);
		Result->Valid.Init(false, Count);
		Result->Contact.Init(0.f, Count);

		auto HeightAt = [Proxy](double X, double Y, double& OutZ)
		{
			const TOptional<float> Height = Proxy->GetHeightAtLocation(FVector(X, Y, 0.0));
			OutZ = Height.IsSet() ? double(Height.GetValue()) : 0.0;
			return Height.IsSet();
		};

		for (int32 Y = 0; Y < Result->SizeY; ++Y)
		{
			if (ShouldCancel && ShouldCancel())
			{
				LastReport.bCancelled = true;
				LastReport.CancelledDuring = TEXT("Landscape");
				PendingLandscapes.Reset();
				return false;
			}
			for (int32 X = 0; X < Result->SizeX; ++X)
			{
				const int32 Index = Y * Result->SizeX + X;
				const double WX = Min.X + (X + 0.5) * StepX;
				const double WY = Min.Y + (Y + 0.5) * StepY;
				double Z = 0.0;
				if (!HeightAt(WX, WY, Z))
				{
					continue;
				}
				// Normal from neighbouring heights (falls back to up at holes and edges).
				double ZX0 = Z, ZX1 = Z, ZY0 = Z, ZY1 = Z;
				const double H = FMath::Max(StepX, StepY) * 0.5;
				HeightAt(WX - H, WY, ZX0);
				HeightAt(WX + H, WY, ZX1);
				HeightAt(WX, WY - H, ZY0);
				HeightAt(WX, WY + H, ZY1);
				const FVector Normal = FVector(-(ZX1 - ZX0) / (2.0 * H), -(ZY1 - ZY0) / (2.0 * H), 1.0).GetSafeNormal();
				Result->Positions[Index] = FVector(WX, WY, Z);
				Result->Normals[Index] = FVector3f(Normal.IsNearlyZero() ? FVector::UpVector : Normal);
				Result->Valid[Index] = true;
			}
		}

		// Contact: distance to the nearest wall or prop, from horizontal traces on a coarse grid, then interpolated.
		if (LandscapeContactRadiusCm > 0.f && LandscapeContactStrength > 0.f)
		{
			const double Radius = LandscapeContactRadiusCm;
			const double Coarse = FMath::Max(Radius * 0.5, FMath::Max(StepX, StepY));
			const int32 CX = FMath::Max(FMath::CeilToInt(Size.X / Coarse), 1) + 1;
			const int32 CY = FMath::Max(FMath::CeilToInt(Size.Y / Coarse), 1) + 1;
			TArray<float> Near;
			Near.Init(0.f, CX * CY);
			for (int32 Y = 0; Y < CY; ++Y)
			{
				if (ShouldCancel && ShouldCancel())
				{
					LastReport.bCancelled = true;
					LastReport.CancelledDuring = TEXT("Landscape");
					PendingLandscapes.Reset();
					return false;
				}
				for (int32 X = 0; X < CX; ++X)
				{
					const double WX = Min.X + X * Coarse;
					const double WY = Min.Y + Y * Coarse;
					double Z = 0.0;
					if (!HeightAt(WX, WY, Z))
					{
						continue;
					}
					const FVector Origin(WX, WY, Z + 25.0);
					double Closest = Radius;
					for (int32 Direction = 0; Direction < 8; ++Direction)
					{
						const double Angle = Direction * UE_DOUBLE_PI / 4.0;
						const FVector End = Origin + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0) * Radius;
						FHitResult Hit;
						if (World->LineTraceSingleByChannel(Hit, Origin, End, ECC_Visibility, Query) && !Hit.bStartPenetrating)
						{
							Closest = FMath::Min(Closest, double(Hit.Distance));
						}
					}
					Near[Y * CX + X] = float(1.0 - Closest / Radius);
				}
			}
			for (int32 Y = 0; Y < Result->SizeY; ++Y)
			{
				for (int32 X = 0; X < Result->SizeX; ++X)
				{
					const double FX = ((X + 0.5) * StepX) / Coarse;
					const double FY = ((Y + 0.5) * StepY) / Coarse;
					const int32 X0 = FMath::Clamp(FMath::FloorToInt(FX), 0, CX - 1);
					const int32 Y0 = FMath::Clamp(FMath::FloorToInt(FY), 0, CY - 1);
					const int32 X1 = FMath::Min(X0 + 1, CX - 1);
					const int32 Y1 = FMath::Min(Y0 + 1, CY - 1);
					const float TX = float(FMath::Clamp(FX - X0, 0.0, 1.0));
					const float TY = float(FMath::Clamp(FY - Y0, 0.0, 1.0));
					const float Top = FMath::Lerp(Near[Y0 * CX + X0], Near[Y0 * CX + X1], TX);
					const float Bottom = FMath::Lerp(Near[Y1 * CX + X0], Near[Y1 * CX + X1], TX);
					// Squared so it hugs the base of the wall instead of a broad gradient.
					Result->Contact[Y * Result->SizeX + X] = FMath::Square(FMath::Lerp(Top, Bottom, TY));
				}
			}
		}

		FGrungifyGenerationParams Params = Job.Params;
		Params.ShouldCancel = ShouldCancel;
		if (!Grungify::GenerateLandscapeMasks(*Result, StoneProfile, LandscapeContactStrength, Params))
		{
			if (Result->bCancelled)
			{
				LastReport.bCancelled = true;
				LastReport.CancelledDuring = TEXT("Landscape");
				PendingLandscapes.Reset();
				return false;
			}
			continue;
		}
		// The inputs are only needed to generate; keep the pending result lean.
		Result->Positions.Empty();
		Result->Normals.Empty();
		Result->Contact.Empty();
		Result->Valid.Empty();
		Result->Seconds = float(FPlatformTime::Seconds() - Start);
		PendingLandscapes.Add(Result);
		++Phase.Items;
	}

	for (const TSharedPtr<FGrungifyLandscapeResult>& Result : PendingLandscapes)
	{
		FGrungifyComponentReport& Row = LastReport.Components.AddDefaulted_GetRef();
		const AActor* Proxy = Result->Proxy.Get();
		Row.Actor = Proxy ? Proxy->GetActorNameOrLabel() : Result->ProxyPath;
		Row.Component = TEXT("(landscape)");
		Row.Result = TEXT("landscape");
		Row.TextureSize = FMath::Max(Result->SizeX, Result->SizeY);
		Row.ExtendedTextureSize = Result->ExtendedPixels.Num() > 0 ? Row.TextureSize : 0;
		Row.MeanMasks = Result->MeanMasks;
		Row.Seconds = Result->Seconds;
		Row.Notes = FString::Printf(TEXT("%d x %d texels over %.0f x %.0f cm, %d on terrain"), Result->SizeX, Result->SizeY,
			Result->Size.X, Result->Size.Y, Result->Covered);
	}
	return true;
}

void AGrungifyWeatheringZone::WriteLandscapes(TArray<FGrungifyLandscapeRecord>& OutRecords)
{
	if (PendingLandscapes.Num() == 0)
	{
		return;
	}

	// Never take a landscape component from another zone; landscapes have no Take Over (one zone per area).
	const TSet<const UPrimitiveComponent*> Others = ComponentsOwnedByOthers(GetWorld(), this,
		[](const AGrungifyWeatheringZone* Zone, TSet<const UPrimitiveComponent*>& Owned)
		{
			for (const FGrungifyLandscapeRecord& Record : Zone->AppliedLandscapeRecords) { Owned.Add(Record.Component.Get()); }
			for (const FGrungifyLandscapeRecord& Record : Zone->PreviewLandscapeRecords) { Owned.Add(Record.Component.Get()); }
		});
	const FBox ZoneBox = ZoneComponent->Bounds.GetBox();
	const float SeedOffset = float((uint32(Seed) * 2654435761u) % 100000u) * 0.01f;

	for (const TSharedPtr<FGrungifyLandscapeResult>& Result : PendingLandscapes)
	{
		ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Result->Proxy.Get());
		if (!Proxy || !ShouldConsiderActor(Proxy, ZoneBox))
		{
			continue;
		}
		// Recheck component admission before any texture creation or package mutation.
		// A component Ignore tag must work even when its landscape proxy is included.
		TArray<ULandscapeComponent*> Admitted;
		int32 Taken = 0;
		for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
		{
			if (!Component || !Component->Bounds.GetBox().Intersect(ZoneBox)
				|| Grungify::HasIgnoreTag(Proxy, Component))
			{
				continue;
			}
			if (Others.Contains(Component))
			{
				++Taken;
				continue;
			}
			if (Component->GetLandscapeMaterial())
			{
				Admitted.Add(Component);
			}
		}
		if (Admitted.Num() == 0)
		{
			continue;
		}
		Proxy->Modify();
		UTexture2D* Texture = CreateLandscapeTexture(Proxy, TEXT("GrungifyLandscapeMask"), Result->SizeX, Result->SizeY, Result->Pixels);
		UTexture2D* ExtendedTexture = Result->ExtendedPixels.Num() > 0
			? CreateLandscapeTexture(Proxy, TEXT("GrungifyLandscapeExtendedMask"), Result->SizeX, Result->SizeY, Result->ExtendedPixels) : nullptr;
		if (!Texture)
		{
			continue;
		}

		// One instance per distinct landscape material under the zone, shared by its components.
		TMap<UMaterialInterface*, UMaterialInstanceConstant*> Instances;
		int32 Bound = 0;
		int32 NotHooked = 0;
		for (ULandscapeComponent* Component : Admitted)
		{
			UMaterialInterface* Parent = Component->GetLandscapeMaterial();
			if (!Parent)
			{
				continue;
			}
			float Active = 0.f;
			if (!Parent->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(Grungify::ActiveParameterName()), Active))
			{
				++NotHooked;
			}

			UMaterialInstanceConstant*& Instance = Instances.FindOrAdd(Parent);
			if (!Instance)
			{
				Instance = NewObject<UMaterialInstanceConstant>(Proxy, MakeUniqueObjectName(Proxy, UMaterialInstanceConstant::StaticClass(), TEXT("GrungifyLandscapeMI")), RF_Transactional);
				Grungify::ConfigureOwnedInstance(Instance, Parent, Texture, ExtendedTexture, EGrungifySurfaceType::Stone, &Palette, LandscapeTexelSizeCm);
				// MF_GrungifyLandscape reads these as ordinary parameters (landscapes have no custom primitive data).
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grungify Mask Mode")), Grungify::MaskModeTexture);
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grungify Surface Type")), 0.f);
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grungify Feature Scale")), FeatureScaleCm);
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grungify Seed Offset")), SeedOffset);
				Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Grungify::LandscapeRectParameterName()),
					FLinearColor(float(Result->Min.X), float(Result->Min.Y), float(Result->Size.X), float(Result->Size.Y)));
				Instance->PostEditChange();
			}

			FGrungifyLandscapeRecord& Record = OutRecords.AddDefaulted_GetRef();
			Record.Component = Component;
			Record.OriginalOverride = Component->OverrideMaterial;
			Component->SetFlags(RF_Transactional);
			Component->Modify();
			Component->OverrideMaterial = Instance;
			++Bound;
		}
		if (Bound > 0)
		{
			Proxy->UpdateAllComponentMaterialInstances();
		}

		for (FGrungifyComponentReport& Row : LastReport.Components)
		{
			if (Row.Result == TEXT("landscape") && Row.Actor == Proxy->GetActorNameOrLabel())
			{
				Row.Notes += FString::Printf(TEXT("; %d component(s) bound"), Bound);
				if (Taken > 0)
				{
					Row.Notes += FString::Printf(TEXT(", %d left to another zone"), Taken);
				}
				if (NotHooked > 0)
				{
					Row.Notes += TEXT("; the landscape material doesn't use MF_GrungifyLandscape, so the wear won't show (use Hook Up Materials)");
				}
			}
		}
	}
}

bool AGrungifyWeatheringZone::RestoreLandscapes(TArray<FGrungifyLandscapeRecord>& Records)
{
	TSet<ALandscapeProxy*> Touched;
	for (int32 Index = Records.Num() - 1; Index >= 0; --Index)
	{
		ULandscapeComponent* Component = Cast<ULandscapeComponent>(Records[Index].Component.Get());
		if (!Component)
		{
			// Not loaded (another World Partition cell): keep the record for a later Clear.
			if (Records[Index].Component.IsNull())
			{
				Records.RemoveAt(Index);
			}
			continue;
		}
		Component->SetFlags(RF_Transactional);
		Component->Modify();
		Component->OverrideMaterial = Records[Index].OriginalOverride;
		if (ALandscapeProxy* Proxy = Component->GetLandscapeProxy())
		{
			Touched.Add(Proxy);
		}
		Records.RemoveAt(Index);
	}
	for (ALandscapeProxy* Proxy : Touched)
	{
		Proxy->UpdateAllComponentMaterialInstances();
	}
	return true;
}

#endif // WITH_EDITOR
