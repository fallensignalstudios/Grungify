// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyRuntimeWear.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GrungifySettings.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "RHI.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

namespace GrungifyRuntimeWear
{
	constexpr float DefaultTraceDistanceCm = 100.f;
	/** Zone default; used when an instance predates the texel-size parameter. */
	constexpr float FallbackTexelSizeCm = 4.f;
	constexpr int32 ChannelsPerMask = 4;

	/** Mask channel order is R, G, B, A (layers 0-3 and 4-7); FColor's memory order is B, G, R, A. */
	uint8& ChannelOf(FColor& Color, int32 Channel)
	{
		switch (Channel)
		{
		case 0: return Color.R;
		case 1: return Color.G;
		case 2: return Color.B;
		default: return Color.A;
		}
	}

	int32 WrapIndex(int32 Index, int32 Size)
	{
		const int32 Wrapped = Index % Size;
		return Wrapped < 0 ? Wrapped + Size : Wrapped;
	}
	/** The generated material's unweathered default is not a baked runtime mask. */
	const TCHAR* NoMaskTexturePath = TEXT("/Grungify/Materials/T_GrungifyNoMask.T_GrungifyNoMask");
}

UGrungifyRuntimeWearSubsystem* UGrungifyRuntimeWearSubsystem::Get(const UObject* WorldContextObject)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<UGrungifyRuntimeWearSubsystem>() : nullptr;
}

bool UGrungifyRuntimeWearSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UGrungifyRuntimeWearSubsystem::Deinitialize()
{
	ResetRuntimeWear(nullptr);
	Super::Deinitialize();
}

bool UGrungifyRuntimeWearSubsystem::AddWearAtLocation(const FVector& Location, const FVector& Direction, EGrungifyWearLayer Layer, float RadiusCm, float Amount)
{
	return TraceAndAddWear(Location, Direction, GrungifyRuntimeWear::DefaultTraceDistanceCm, nullptr, Layer, RadiusCm, Amount);
}

bool UGrungifyRuntimeWearSubsystem::TraceAndAddWear(const FVector& Location, const FVector& Direction, float TraceDistanceCm, const AActor* IgnoredActor, EGrungifyWearLayer Layer, float RadiusCm, float Amount)
{
	UWorld* World = GetWorld();
	const FVector Dir = Direction.GetSafeNormal();
	if (!World || Dir.IsNearlyZero() || TraceDistanceCm <= 0.f)
	{
		return false;
	}

	// Complex collision with face indices is what FindCollisionUV needs to recover the mesh UV.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(GrungifyRuntimeWear), true, IgnoredActor);
	Params.bReturnFaceIndex = true;

	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, Location, Location + Dir * TraceDistanceCm, ECC_Visibility, Params))
	{
		return false;
	}
	return AddWearAtHit(Hit, Layer, RadiusCm, Amount);
}

bool UGrungifyRuntimeWearSubsystem::AddWearAtHit(const FHitResult& Hit, EGrungifyWearLayer Layer, float RadiusCm, float Amount)
{
	// Invalid layer values never create targets or replace materials.
	if (!SupportsRuntimeLayer(Layer))
	{
		if (!bWarnedUnsupportedLayer)
		{
			bWarnedUnsupportedLayer = true;
			UE_LOG(LogGrungify, Warning, TEXT("Runtime wear: unsupported layer %d is skipped; valid layers are 0-7."), int32(Layer));
		}
		return false;
	}

	UStaticMeshComponent* Component = Cast<UStaticMeshComponent>(Hit.GetComponent());
	Amount = FMath::Clamp(Amount, 0.f, 1.f);
	if (!IsValid(Component) || RadiusCm <= 0.f || Amount <= 0.f)
	{
		return false;
	}

	// Only meshes whose active instances read the mask texture can show painted wear.
	const UGrungifySettings* Settings = UGrungifySettings::Get();
	const TArray<float>& CustomData = Component->GetCustomPrimitiveData().Data;
	const int32 ModeIndex = Settings->CustomPrimitiveDataIndex + Grungify::CPD_MaskMode;
	if (!CustomData.IsValidIndex(ModeIndex) || CustomData[ModeIndex] != Grungify::MaskModeTexture)
	{
		return false;
	}

	// A face must resolve to an eligible binding; excluded or unresolved faces cannot
	// fall back to painting other slots that happen to share the same UV space.
	int32 HitSection = INDEX_NONE;
	const UMaterialInterface* KnownHitMaterial = Component->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, HitSection);
	if (!KnownHitMaterial || !IsEligibleMaterial(KnownHitMaterial))
	{
		return false;
	}

	// FindCollisionUV needs Project Settings > Physics > "Support UV From Hit Results" (cooked into the collision
	// data) and a complex trace that returned a face index; simple collision has no UVs.
	FVector2D UV;
	if (!UGameplayStatics::FindCollisionUV(Hit, Settings->MaskUVChannel, UV))
	{
		if (!bWarnedMissingUV)
		{
			bWarnedMissingUV = true;
			UE_LOG(LogGrungify, Warning, TEXT("Runtime wear: no UV for the hit on %s. Enable Project Settings > Physics > Support UV From Hit Results and trace complex collision with bReturnFaceIndex."), *Component->GetPathName());
		}
		return false;
	}
	UV.X = FMath::Frac(UV.X);
	UV.Y = FMath::Frac(UV.Y);

	PruneStaleStates();
	FGrungifyRuntimeWearState* State = FindOrCreateState(Component);
	if (!State)
	{
		return false;
	}

	// State creation replaces bindings, so resolve the current face again and require
	// its corresponding runtime slot. Never fall back to painting every active slot.
	int32 SectionIndex = INDEX_NONE;
	const UMaterialInterface* HitMaterial = Component->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, SectionIndex);
	if (!HitMaterial)
	{
		return false;
	}
	const int32 HitSlot = State->Slots.IndexOfByPredicate([HitMaterial](const FGrungifyRuntimeWearSlot& Slot)
	{
		return Slot.Dynamic == HitMaterial;
	});
	if (HitSlot == INDEX_NONE)
	{
		return false;
	}

	bool bMissingExtended = false;
	const bool bPainted = PaintSlot(*State, HitSlot, UV, Layer, RadiusCm, Amount, bMissingExtended);
	if (bMissingExtended && !State->bWarnedNoExtended)
	{
		State->bWarnedNoExtended = true;
		UE_LOG(LogGrungify, Warning, TEXT("Runtime wear: %s has no Grungify Extended Mask Texture; layer %s is skipped."),
			*Component->GetPathName(), *UEnum::GetValueAsString(Layer));
	}
	return bPainted;
}

bool UGrungifyRuntimeWearSubsystem::PaintSlot(FGrungifyRuntimeWearState& State, int32 SlotIndex, const FVector2D& UV, EGrungifyWearLayer Layer, float RadiusCm, float Amount, bool& bOutMissingExtended)
{
	bOutMissingExtended = false;
	UStaticMeshComponent* Component = State.Component.Get();
	if (!SupportsRuntimeLayer(Layer) || !IsValid(Component) || !State.Slots.IsValidIndex(SlotIndex))
	{
		return false;
	}
	const FGrungifyRuntimeWearSlot& Slot = State.Slots[SlotIndex];
	if (Component->GetMaterial(Slot.MaterialIndex) != Slot.Dynamic || !IsEligibleMaterial(Slot.Dynamic))
	{
		return false;
	}
	const int32 LayerIndex = int32(Layer);
	const int32 TargetIndex = LayerIndex >= GrungifyRuntimeWear::ChannelsPerMask ? Slot.ExtendedTarget : Slot.MaskTarget;
	if (!State.Targets.IsValidIndex(TargetIndex))
	{
		bOutMissingExtended = TargetIndex == INDEX_NONE && LayerIndex >= GrungifyRuntimeWear::ChannelsPerMask;
		return false;
	}
	return PaintTarget(State.Targets[TargetIndex], UV, RadiusCm / Slot.TexelSizeCm,
		LayerIndex % GrungifyRuntimeWear::ChannelsPerMask, FMath::Clamp(Amount, 0.f, 1.f));
}

bool UGrungifyRuntimeWearSubsystem::SupportsRuntimeLayer(EGrungifyWearLayer Layer)
{
	// Every channel is written on the CPU and uploaded whole, so alpha layers (Handling, Moss) are as safe as RGB.
	const int32 Index = int32(Layer);
	return Index >= 0 && Index < 2 * GrungifyRuntimeWear::ChannelsPerMask;
}

bool UGrungifyRuntimeWearSubsystem::IsEligibleMaterial(const UMaterialInterface* Material)
{
	const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material);
	if (!Instance)
	{
		return false;
	}
	float Active = 0.f;
	UTexture* Mask = nullptr;
	return Instance->GetScalarParameterValue(FMaterialParameterInfo(Grungify::ActiveParameterName()), Active)
		&& FMath::IsNearlyEqual(Active, 1.f)
		&& Instance->GetTextureParameterValue(FMaterialParameterInfo(Grungify::MaskTextureParameterName()), Mask, true)
		&& Mask != nullptr && Mask->GetPathName() != GrungifyRuntimeWear::NoMaskTexturePath;
}

int32 UGrungifyRuntimeWearSubsystem::FindStateIndex(const UStaticMeshComponent* Component) const
{
	return States.IndexOfByPredicate([Component](const FGrungifyRuntimeWearState& State)
	{
		return State.Component.Get() == Component;
	});
}

FGrungifyRuntimeWearState* UGrungifyRuntimeWearSubsystem::FindOrCreateState(UStaticMeshComponent* Component)
{
	const int32 ExistingIndex = FindStateIndex(Component);
	if (ExistingIndex != INDEX_NONE)
	{
		return &States[ExistingIndex];
	}

	FGrungifyRuntimeWearState State;
	State.Component = Component;
	State.OriginalOverrideMaterials = Component->OverrideMaterials;
	for (int32 MaterialIndex = 0; MaterialIndex < Component->GetNumMaterials(); ++MaterialIndex)
	{
		UMaterialInstance* Instance = Cast<UMaterialInstance>(Component->GetMaterial(MaterialIndex));
		if (!IsEligibleMaterial(Instance))
		{
			continue;
		}

		UTexture* Mask = nullptr;
		if (!Instance->GetTextureParameterValue(FMaterialParameterInfo(Grungify::MaskTextureParameterName()), Mask, true) || !Mask)
		{
			continue;
		}
		UTexture* Extended = nullptr;
		Instance->GetTextureParameterValue(FMaterialParameterInfo(Grungify::ExtendedMaskTextureParameterName()), Extended, true);
		if (Extended && Extended->GetPathName() == GrungifyRuntimeWear::NoMaskTexturePath)
		{
			Extended = nullptr;
		}

		float TexelSizeCm = 0.f;
		if (!Instance->GetScalarParameterValue(FMaterialParameterInfo(Grungify::MaskTexelSizeParameterName()), TexelSizeCm) || TexelSizeCm <= 0.f)
		{
			if (!bWarnedMissingTexelSize)
			{
				bWarnedMissingTexelSize = true;
				UE_LOG(LogGrungify, Warning, TEXT("Runtime wear: %s has no Grungify Mask Texel Size; assuming %.0f cm. Re-apply the zone to store it."),
					*Instance->GetPathName(), GrungifyRuntimeWear::FallbackTexelSizeCm);
			}
			TexelSizeCm = GrungifyRuntimeWear::FallbackTexelSizeCm;
		}

		FGrungifyRuntimeWearSlot Slot;
		Slot.MaterialIndex = MaterialIndex;
		Slot.OriginalMaterial = Instance;
		Slot.TexelSizeCm = TexelSizeCm;
		Slot.MaskTarget = FindOrCreateTarget(State, Mask);
		Slot.ExtendedTarget = Extended ? FindOrCreateTarget(State, Extended) : INDEX_NONE;
		if (Slot.MaskTarget == INDEX_NONE)
		{
			continue;
		}

		Slot.Dynamic = UMaterialInstanceDynamic::Create(Instance, Component);
		Slot.Dynamic->SetTextureParameterValue(Grungify::MaskTextureParameterName(), State.Targets[Slot.MaskTarget].Texture);
		if (Slot.ExtendedTarget != INDEX_NONE)
		{
			Slot.Dynamic->SetTextureParameterValue(Grungify::ExtendedMaskTextureParameterName(), State.Targets[Slot.ExtendedTarget].Texture);
		}
		Component->SetMaterial(MaterialIndex, Slot.Dynamic);
		State.Slots.Add(Slot);
	}

	if (State.Slots.Num() == 0)
	{
		return nullptr;
	}
	State.RuntimeOverrideMaterials = Component->OverrideMaterials;
	return &States.Add_GetRef(MoveTemp(State));
}

int32 UGrungifyRuntimeWearSubsystem::FindOrCreateTarget(FGrungifyRuntimeWearState& State, UTexture* Source)
{
	const int32 Existing = State.Targets.IndexOfByPredicate([Source](const FGrungifyRuntimeWearTarget& Target)
	{
		return Target.Source == Source;
	});
	if (Existing != INDEX_NONE)
	{
		return Existing;
	}

	const int32 Width = FMath::RoundToInt(Source->GetSurfaceWidth());
	const int32 Height = FMath::RoundToInt(Source->GetSurfaceHeight());
	if (Width <= 0 || Height <= 0)
	{
		return INDEX_NONE;
	}

	// The copy reads the GPU resource, so the full-resolution mip must be resident or the copy would be blurry.
	bool bWrapX = true;
	bool bWrapY = true;
	TextureFilter Filter = TF_Bilinear;
	if (UTexture2D* Source2D = Cast<UTexture2D>(Source))
	{
		bWrapX = Source2D->AddressX != TA_Clamp;
		bWrapY = Source2D->AddressY != TA_Clamp;
		Filter = Source2D->Filter;
		Source2D->SetForceMipLevelsToBeResident(30.f);
		Source2D->WaitForStreaming();
	}

	TArray<FColor> Pixels;
	if (!ReadTextureOnGPU(Source, Width, Height, Pixels))
	{
		UE_LOG(LogGrungify, Warning, TEXT("Runtime wear: could not read %s back from the GPU; its slot is not painted."), *Source->GetPathName());
		return INDEX_NONE;
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8, MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("GrungifyRuntimeMask")));
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
	{
		return INDEX_NONE;
	}
	// Masks are data: no sRGB decode, and the source's filtering and addressing so the look doesn't change.
	Texture->SRGB = false;
	Texture->Filter = Filter;
	Texture->AddressX = bWrapX ? TA_Wrap : TA_Clamp;
	Texture->AddressY = bWrapY ? TA_Wrap : TA_Clamp;
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();

	FGrungifyRuntimeWearTarget& Target = State.Targets.AddDefaulted_GetRef();
	Target.Source = Source;
	Target.Texture = Texture;
	Target.Pixels = MoveTemp(Pixels);
	Target.Width = Width;
	Target.Height = Height;
	Target.bWrapX = bWrapX;
	Target.bWrapY = bWrapY;
	return State.Targets.Num() - 1;
}

uint8 UGrungifyRuntimeWearSubsystem::StrokeIncrement(float DX, float DY, float RadiusTexels, float Amount)
{
	if (RadiusTexels <= 0.f || Amount <= 0.f)
	{
		return 0;
	}
	const float Distance = FMath::Sqrt(DX * DX + DY * DY) / RadiusTexels;
	const float Falloff = FMath::SmoothStep(0.f, 1.f, 1.f - Distance);
	return uint8(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Amount, 0.f, 1.f) * Falloff * 255.f), 0, 255));
}

bool UGrungifyRuntimeWearSubsystem::PaintTarget(FGrungifyRuntimeWearTarget& Target, const FVector2D& UV, float RadiusTexels, int32 Channel, float Amount)
{
	if (!Target.Texture || Channel < 0 || Channel >= GrungifyRuntimeWear::ChannelsPerMask
		|| Target.Width <= 0 || Target.Height <= 0 || Target.Pixels.Num() != Target.Width * Target.Height)
	{
		return false;
	}
	RadiusTexels = FMath::Max(RadiusTexels, 0.5f);

	// Texel X spans [X, X+1) in texel space, so its centre is X + 0.5. Wrapped spans are capped at one texture width
	// so no texel is visited twice by the same stroke.
	const float CentreX = float(UV.X) * Target.Width;
	const float CentreY = float(UV.Y) * Target.Height;
	const int32 X0 = FMath::FloorToInt(CentreX - RadiusTexels);
	const int32 Y0 = FMath::FloorToInt(CentreY - RadiusTexels);
	const int32 X1 = FMath::Min(FMath::CeilToInt(CentreX + RadiusTexels), X0 + Target.Width - 1);
	const int32 Y1 = FMath::Min(FMath::CeilToInt(CentreY + RadiusTexels), Y0 + Target.Height - 1);

	FIntRect Dirty(MAX_int32, MAX_int32, MIN_int32, MIN_int32);
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		const int32 TexelY = Target.bWrapY ? GrungifyRuntimeWear::WrapIndex(Y, Target.Height) : Y;
		if (TexelY < 0 || TexelY >= Target.Height)
		{
			continue;
		}
		for (int32 X = X0; X <= X1; ++X)
		{
			const int32 TexelX = Target.bWrapX ? GrungifyRuntimeWear::WrapIndex(X, Target.Width) : X;
			if (TexelX < 0 || TexelX >= Target.Width)
			{
				continue;
			}
			const uint8 Increment = StrokeIncrement(X + 0.5f - CentreX, Y + 0.5f - CentreY, RadiusTexels, Amount);
			if (Increment == 0)
			{
				continue;
			}
			// Saturating add on the selected channel only; the other three bytes of the texel are never written.
			uint8& Value = GrungifyRuntimeWear::ChannelOf(Target.Pixels[TexelY * Target.Width + TexelX], Channel);
			const uint8 Painted = uint8(FMath::Min(int32(Value) + int32(Increment), 255));
			if (Painted != Value)
			{
				Value = Painted;
				Dirty.Min.X = FMath::Min(Dirty.Min.X, TexelX);
				Dirty.Min.Y = FMath::Min(Dirty.Min.Y, TexelY);
				Dirty.Max.X = FMath::Max(Dirty.Max.X, TexelX + 1);
				Dirty.Max.Y = FMath::Max(Dirty.Max.Y, TexelY + 1);
			}
		}
	}
	if (Dirty.Min.X > Dirty.Max.X)
	{
		// Every touched texel was already saturated; the texture is unchanged but the stroke landed.
		return true;
	}

	// Upload whole texels of the dirty rectangle. The render thread owns the copy and frees it when done, so later
	// strokes can keep changing Pixels.
	const int32 DirtyWidth = Dirty.Width();
	const int32 DirtyHeight = Dirty.Height();
	FColor* Upload = static_cast<FColor*>(FMemory::Malloc(DirtyWidth * DirtyHeight * sizeof(FColor)));
	for (int32 Row = 0; Row < DirtyHeight; ++Row)
	{
		FMemory::Memcpy(Upload + Row * DirtyWidth, &Target.Pixels[(Dirty.Min.Y + Row) * Target.Width + Dirty.Min.X], DirtyWidth * sizeof(FColor));
	}
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(Dirty.Min.X, Dirty.Min.Y, 0, 0, DirtyWidth, DirtyHeight);
	Target.Texture->UpdateTextureRegions(0, 1, Region, DirtyWidth * sizeof(FColor), sizeof(FColor), reinterpret_cast<uint8*>(Upload),
		[](uint8* Data, const FUpdateTextureRegion2D* Regions)
		{
			FMemory::Free(Data);
			delete Regions;
		});
	return true;
}

bool UGrungifyRuntimeWearSubsystem::ReadTextureOnGPU(UTexture* Texture, int32 Width, int32 Height, TArray<FColor>& OutPixels)
{
	OutPixels.Reset();
	FTextureResource* TextureResource = Texture ? Texture->GetResource() : nullptr;
	if (!TextureResource || Width <= 0 || Height <= 0 || !FApp::CanEverRender())
	{
		return false;
	}

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	RenderTarget->RenderTargetFormat = RTF_RGBA8;
	RenderTarget->ClearColor = FLinearColor::Transparent;
	RenderTarget->bAutoGenerateMips = false;
	// Masks are data: linear storage and gamma 1 so the copy neither encodes nor decodes them.
	RenderTarget->bForceLinearGamma = true;
	RenderTarget->SRGB = false;
	RenderTarget->InitAutoFormat(Width, Height);
	RenderTarget->UpdateResourceImmediate(true);
	FTextureRenderTargetResource* TargetResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!TargetResource)
	{
		return false;
	}

	// An opaque 1:1 draw samples every texel at its centre and writes all four channels unchanged.
	{
		FCanvas Canvas(TargetResource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
		FCanvasTileItem Tile(FVector2D::ZeroVector, TextureResource, FVector2D(Width, Height), FLinearColor::White);
		Tile.BlendMode = SE_BLEND_Opaque;
		Canvas.DrawItem(Tile);
		Canvas.Flush_GameThread(true);
	}
	FReadSurfaceDataFlags Flags(RCM_UNorm, CubeFace_MAX);
	Flags.SetLinearToGamma(false);
	const bool bRead = TargetResource->ReadPixels(OutPixels, Flags);
	return bRead && OutPixels.Num() == Width * Height;
}

bool UGrungifyRuntimeWearSubsystem::ReadRuntimeWearMask(UPrimitiveComponent* Component, int32 MaterialIndex, bool bExtended, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
{
	OutPixels.Reset();
	OutWidth = OutHeight = 0;
	const int32 StateIndex = FindStateIndex(Cast<UStaticMeshComponent>(Component));
	if (StateIndex == INDEX_NONE)
	{
		return false;
	}
	const FGrungifyRuntimeWearState& State = States[StateIndex];
	const FGrungifyRuntimeWearSlot* Slot = State.Slots.FindByPredicate([MaterialIndex](const FGrungifyRuntimeWearSlot& Candidate)
	{
		return Candidate.MaterialIndex == MaterialIndex;
	});
	const int32 TargetIndex = Slot ? (bExtended ? Slot->ExtendedTarget : Slot->MaskTarget) : INDEX_NONE;
	if (!State.Targets.IsValidIndex(TargetIndex))
	{
		return false;
	}
	const FGrungifyRuntimeWearTarget& Target = State.Targets[TargetIndex];
	if (!ReadTextureOnGPU(Target.Texture, Target.Width, Target.Height, OutPixels))
	{
		return false;
	}
	OutWidth = Target.Width;
	OutHeight = Target.Height;
	return true;
}

void UGrungifyRuntimeWearSubsystem::ResetRuntimeWear(UPrimitiveComponent* Component)
{
	if (Component)
	{
		UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Component);
		const int32 Index = StaticMesh ? FindStateIndex(StaticMesh) : INDEX_NONE;
		if (Index != INDEX_NONE)
		{
			const FGrungifyRuntimeWearState State = MoveTemp(States[Index]);
			States.RemoveAtSwap(Index);
			RestoreState(StaticMesh, State);
		}
		return;
	}

	for (const FGrungifyRuntimeWearState& State : States)
	{
		RestoreState(State.Component.Get(), State);
	}
	States.Reset();
}

void UGrungifyRuntimeWearSubsystem::RestoreState(UStaticMeshComponent* Component, const FGrungifyRuntimeWearState& State)
{
	if (!IsValid(Component))
	{
		return;
	}
	if (Component->OverrideMaterials == State.RuntimeOverrideMaterials)
	{
		// Notify renderer/physics/material caches through the public setter, then restore
		// the raw array so inherited defaults, null holes and its length remain exact.
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const int32 MaterialCount = FMath::Max(Component->OverrideMaterials.Num(), State.OriginalOverrideMaterials.Num());
		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
		{
			UMaterialInterface* OriginalMaterial = State.OriginalOverrideMaterials.IsValidIndex(MaterialIndex)
				? State.OriginalOverrideMaterials[MaterialIndex].Get() : nullptr;
			if (!OriginalMaterial && Mesh)
			{
				OriginalMaterial = Mesh->GetMaterial(MaterialIndex);
			}
			Component->SetMaterial(MaterialIndex, OriginalMaterial);
		}
		Component->OverrideMaterials = State.OriginalOverrideMaterials;
		Component->MarkRenderStateDirty();
		return;
	}
	for (const FGrungifyRuntimeWearSlot& Slot : State.Slots)
	{
		// Leave slots alone that gameplay has re-assigned since we painted them.
		if (Component->GetMaterial(Slot.MaterialIndex) == Slot.Dynamic)
		{
			UMaterialInterface* OriginalOverride = State.OriginalOverrideMaterials.IsValidIndex(Slot.MaterialIndex)
				? State.OriginalOverrideMaterials[Slot.MaterialIndex].Get() : nullptr;
			Component->SetMaterial(Slot.MaterialIndex, OriginalOverride);
		}
	}
}

void UGrungifyRuntimeWearSubsystem::PruneStaleStates()
{
	States.RemoveAllSwap([](const FGrungifyRuntimeWearState& State)
	{
		return !State.Component.IsValid();
	});
}

UGrungifyRuntimeWearComponent::UGrungifyRuntimeWearComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UGrungifyRuntimeWearComponent::EmitWear(EGrungifyWearLayer Layer, float RadiusCm, float Amount)
{
	const AActor* Owner = GetOwner();
	UGrungifyRuntimeWearSubsystem* Subsystem = UGrungifyRuntimeWearSubsystem::Get(this);
	if (!Owner || !Subsystem)
	{
		return false;
	}
	return Subsystem->TraceAndAddWear(Owner->GetActorLocation(), FVector::DownVector, TraceDistanceCm, Owner, Layer, RadiusCm, Amount);
}
