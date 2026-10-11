// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyRuntimeWear.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GrungifySettings.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TextureResource.h"

namespace GrungifyRuntimeWear
{
	constexpr int32 BrushSize = 64;
	constexpr float DefaultTraceDistanceCm = 100.f;
	/** Zone default; used when an instance predates the texel-size parameter. */
	constexpr float FallbackTexelSizeCm = 4.f;
	constexpr int32 ChannelsPerMask = 4;
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
	BrushTexture = nullptr;
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
	// Canvas additive writes RGB only. AlphaHoldout erases alpha and attenuates RGB;
	// do not create targets or replace materials for unsupported alpha/invalid layers.
	if (!SupportsRuntimeLayer(Layer))
	{
		if (!bWarnedUnsupportedLayer)
		{
			bWarnedUnsupportedLayer = true;
			UE_LOG(LogGrungify, Warning, TEXT("Runtime wear: unsupported layer %d is skipped. Handling and Moss need alpha-only accumulation; invalid layer values are also rejected."), int32(Layer));
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

	const int32 LayerIndex = int32(Layer);
	const bool bExtended = LayerIndex >= GrungifyRuntimeWear::ChannelsPerMask;
	const int32 Channel = LayerIndex % GrungifyRuntimeWear::ChannelsPerMask;

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

	TArray<int32, TInlineAllocator<4>> PaintedTargets;
	bool bMissingExtended = false;
	for (int32 SlotIndex = 0; SlotIndex < State->Slots.Num(); ++SlotIndex)
	{
		if (SlotIndex != HitSlot)
		{
			continue;
		}
		const FGrungifyRuntimeWearSlot& Slot = State->Slots[SlotIndex];
		if (Component->GetMaterial(Slot.MaterialIndex) != Slot.Dynamic || !IsEligibleMaterial(Slot.Dynamic))
		{
			continue;
		}
		const int32 TargetIndex = bExtended ? Slot.ExtendedTarget : Slot.MaskTarget;
		if (TargetIndex == INDEX_NONE)
		{
			bMissingExtended = true;
			continue;
		}
		if (PaintedTargets.Contains(TargetIndex))
		{
			continue;
		}
		if (PaintBrush(State->Targets[TargetIndex].RenderTarget, UV, 2.f * RadiusCm / Slot.TexelSizeCm, Channel, Amount))
		{
			PaintedTargets.Add(TargetIndex);
		}
	}

	if (bMissingExtended && PaintedTargets.Num() == 0 && !State->bWarnedNoExtended)
	{
		State->bWarnedNoExtended = true;
		UE_LOG(LogGrungify, Warning, TEXT("Runtime wear: %s has no Grungify Extended Mask Texture; layer %s is skipped."),
			*Component->GetPathName(), *UEnum::GetValueAsString(Layer));
	}
	return PaintedTargets.Num() > 0;
}

bool UGrungifyRuntimeWearSubsystem::SupportsRuntimeLayer(EGrungifyWearLayer Layer)
{
	const int32 Index = int32(Layer);
	return Index >= 0 && Index < 2 * GrungifyRuntimeWear::ChannelsPerMask
		&& Index % GrungifyRuntimeWear::ChannelsPerMask < 3;
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
		Slot.Dynamic->SetTextureParameterValue(Grungify::MaskTextureParameterName(), State.Targets[Slot.MaskTarget].RenderTarget);
		if (Slot.ExtendedTarget != INDEX_NONE)
		{
			Slot.Dynamic->SetTextureParameterValue(Grungify::ExtendedMaskTextureParameterName(), State.Targets[Slot.ExtendedTarget].RenderTarget);
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
	TextureAddress AddressX = TA_Wrap;
	TextureAddress AddressY = TA_Wrap;
	if (UTexture2D* Source2D = Cast<UTexture2D>(Source))
	{
		AddressX = Source2D->AddressX;
		AddressY = Source2D->AddressY;
		Source2D->SetForceMipLevelsToBeResident(30.f);
		Source2D->WaitForStreaming();
	}

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
	RenderTarget->RenderTargetFormat = RTF_RGBA8;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->bAutoGenerateMips = false;
	// Masks are data: linear storage and gamma 1 so the canvas neither encodes nor decodes them.
	RenderTarget->bForceLinearGamma = true;
	RenderTarget->SRGB = false;
	RenderTarget->AddressX = AddressX;
	RenderTarget->AddressY = AddressY;
	RenderTarget->InitAutoFormat(Width, Height);
	RenderTarget->UpdateResourceImmediate(true);

	UCanvas* Canvas = nullptr;
	FVector2D Size;
	FDrawToRenderTargetContext Context;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(this, RenderTarget, Canvas, Size, Context);
	if (Canvas)
	{
		Canvas->K2_DrawTexture(Source, FVector2D::ZeroVector, Size, FVector2D::ZeroVector, FVector2D::UnitVector, FLinearColor::White, BLEND_Opaque);
	}
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(this, Context);

	FGrungifyRuntimeWearTarget& Target = State.Targets.AddDefaulted_GetRef();
	Target.Source = Source;
	Target.RenderTarget = RenderTarget;
	return State.Targets.Num() - 1;
}

UTexture2D* UGrungifyRuntimeWearSubsystem::GetBrushTexture()
{
	if (BrushTexture)
	{
		return BrushTexture;
	}

	const int32 Size = GrungifyRuntimeWear::BrushSize;
	UTexture2D* Brush = UTexture2D::CreateTransient(Size, Size, PF_B8G8R8A8, FName(TEXT("GrungifyRuntimeWearBrush")));
	if (!Brush || !Brush->GetPlatformData() || Brush->GetPlatformData()->Mips.Num() == 0)
	{
		return nullptr;
	}
	Brush->SRGB = false;
	Brush->Filter = TF_Bilinear;
	Brush->AddressX = TA_Clamp;
	Brush->AddressY = TA_Clamp;

	FTexture2DMipMap& Mip = Brush->GetPlatformData()->Mips[0];
	FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
	const float Centre = (Size - 1) * 0.5f;
	for (int32 Y = 0; Y < Size; ++Y)
	{
		for (int32 X = 0; X < Size; ++X)
		{
			const float Distance = FVector2f(X - Centre, Y - Centre).Size() / (Size * 0.5f);
			const float Falloff = FMath::SmoothStep(0.f, 1.f, 1.f - Distance);
			const uint8 Value = uint8(FMath::RoundToInt(Falloff * 255.f));
			Pixels[Y * Size + X] = FColor(Value, Value, Value, Value);
		}
	}
	Mip.BulkData.Unlock();
	Brush->UpdateResource();

	BrushTexture = Brush;
	return BrushTexture;
}

bool UGrungifyRuntimeWearSubsystem::PaintBrush(UTextureRenderTarget2D* Target, const FVector2D& UV, float DiameterTexels, int32 Channel, float Amount)
{
	if (Channel < 0 || Channel > 2)
	{
		return false;
	}
	UTexture2D* Brush = GetBrushTexture();
	if (!Target || !Brush)
	{
		return false;
	}

	UCanvas* Canvas = nullptr;
	FVector2D Size;
	FDrawToRenderTargetContext Context;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(this, Target, Canvas, Size, Context);
	if (Canvas)
	{
		const FVector2D Diameter(FMath::Max(DiameterTexels, 1.f));
		const FVector2D Position = UV * Size - Diameter * 0.5f;

		// Native Canvas additive uses CW_RGB and One/One: add only the selected RGB
		// channel, preserving alpha. Alpha-layer requests are rejected before mutation.
		FLinearColor Color(0.f, 0.f, 0.f, 0.f);
		switch (Channel)
		{
		case 0:
			Color.R = Amount;
			break;
		case 1:
			Color.G = Amount;
			break;
		case 2:
			Color.B = Amount;
			break;
		}
		Canvas->K2_DrawTexture(Brush, Position, Diameter, FVector2D::ZeroVector, FVector2D::UnitVector, Color, BLEND_Additive);
	}
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(this, Context);
	return Canvas != nullptr;
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
