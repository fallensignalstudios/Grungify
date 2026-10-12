// Copyright Fallen Signal Studios. All Rights Reserved.
// Admission, material reset, and selected-channel accumulation for all eight layers proven by GPU readback.
#include "GrungifyRuntimeWear.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Engine/World.h"
#include "GrungifySettings.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"

struct FGrungifyRuntimeWearTestAccess
{
	static bool Supports(EGrungifyWearLayer Layer) { return UGrungifyRuntimeWearSubsystem::SupportsRuntimeLayer(Layer); }
	static bool Eligible(const UMaterialInterface* Material) { return UGrungifyRuntimeWearSubsystem::IsEligibleMaterial(Material); }
	static int32 StateCount(const UGrungifyRuntimeWearSubsystem& Subsystem) { return Subsystem.States.Num(); }
	static bool HasTargets(const UGrungifyRuntimeWearSubsystem& Subsystem)
	{
		return Subsystem.States.ContainsByPredicate([](const FGrungifyRuntimeWearState& State) { return State.Targets.Num() > 0; });
	}
	static bool WarnedMissingUV(const UGrungifyRuntimeWearSubsystem& Subsystem) { return Subsystem.bWarnedMissingUV; }
	static void AddState(UGrungifyRuntimeWearSubsystem& Subsystem, FGrungifyRuntimeWearState&& State)
	{
		Subsystem.States.Add(MoveTemp(State));
	}
	static FGrungifyRuntimeWearState* CreateState(UGrungifyRuntimeWearSubsystem& Subsystem, UStaticMeshComponent* Component)
	{
		return Subsystem.FindOrCreateState(Component);
	}
	static bool Paint(UGrungifyRuntimeWearSubsystem& Subsystem, UStaticMeshComponent* Component, const FVector2D& UV, EGrungifyWearLayer Layer, float RadiusCm, float Amount)
	{
		const int32 Index = Subsystem.FindStateIndex(Component);
		bool bMissingExtended = false;
		return Index != INDEX_NONE && Subsystem.PaintSlot(Subsystem.States[Index], 0, UV, Layer, RadiusCm, Amount, bMissingExtended);
	}
	static uint8 Increment(float DX, float DY, float RadiusTexels, float Amount)
	{
		return UGrungifyRuntimeWearSubsystem::StrokeIncrement(DX, DY, RadiusTexels, Amount);
	}
	static const TArray<FColor>* CpuPixels(const UGrungifyRuntimeWearSubsystem& Subsystem, const UStaticMeshComponent* Component, bool bExtended)
	{
		const int32 Index = Subsystem.FindStateIndex(Component);
		if (Index == INDEX_NONE || Subsystem.States[Index].Slots.Num() == 0)
		{
			return nullptr;
		}
		const FGrungifyRuntimeWearState& State = Subsystem.States[Index];
		const int32 Target = bExtended ? State.Slots[0].ExtendedTarget : State.Slots[0].MaskTarget;
		return State.Targets.IsValidIndex(Target) ? &State.Targets[Target].Pixels : nullptr;
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyRuntimeLayerAdmissionTest, "Grungify.RuntimeWear.UnsupportedLayerNoMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyRuntimeLayerAdmissionTest::RunTest(const FString& Parameters)
{
	UWorld* World = NewObject<UWorld>();
	UGrungifyRuntimeWearSubsystem* Subsystem = NewObject<UGrungifyRuntimeWearSubsystem>(World);
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>();
	Component->OverrideMaterials = { nullptr, UMaterial::GetDefaultMaterial(MD_Surface), nullptr };
	Component->SetDefaultCustomPrimitiveDataFloat(3, .27f);
	Component->SetCustomPrimitiveDataFloat(7, .63f);
	const auto OverridesBefore = Component->OverrideMaterials;
	const auto DefaultDataBefore = Component->GetDefaultCustomPrimitiveData().Data;
	const auto LiveDataBefore = Component->GetCustomPrimitiveData().Data;
	FHitResult Hit;
	Hit.Component = Component;

	AddExpectedError(TEXT("Runtime wear: unsupported layer"), EAutomationExpectedErrorFlags::Contains, 1);
	for (EGrungifyWearLayer Layer : { static_cast<EGrungifyWearLayer>(8), static_cast<EGrungifyWearLayer>(255) })
	{
		TestFalse(TEXT("Unsupported layer is refused"), Subsystem->AddWearAtHit(Hit, Layer));
		TestTrue(TEXT("Raw material overrides unchanged"), Component->OverrideMaterials == OverridesBefore);
		TestTrue(TEXT("Default CPD unchanged"), Component->GetDefaultCustomPrimitiveData().Data == DefaultDataBefore);
		TestTrue(TEXT("Live CPD unchanged"), Component->GetCustomPrimitiveData().Data == LiveDataBefore);
		TestEqual(TEXT("No runtime state created"), FGrungifyRuntimeWearTestAccess::StateCount(*Subsystem), 0);
		TestFalse(TEXT("No live mask created"), FGrungifyRuntimeWearTestAccess::HasTargets(*Subsystem));
	}
	// Every channel, alpha included, is written exactly on the CPU, so all eight layers are admitted.
	for (int32 LayerIndex = 0; LayerIndex < 8; ++LayerIndex)
	{
		TestTrue(FString::Printf(TEXT("Layer %d passes layer admission"), LayerIndex), FGrungifyRuntimeWearTestAccess::Supports(static_cast<EGrungifyWearLayer>(LayerIndex)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyRuntimeMaterialAdmissionTest, "Grungify.RuntimeWear.ActiveMasksAndExcludedFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyRuntimeMaterialAdmissionTest::RunTest(const FString& Parameters)
{
	UMaterialInterface* Parent = UMaterial::GetDefaultMaterial(MD_Surface);
	UTexture2D* Mask = NewObject<UTexture2D>();
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>();
	UMaterialInstanceDynamic* Owned = UMaterialInstanceDynamic::Create(Parent, Component);
	Owned->SetScalarParameterValue(Grungify::ActiveParameterName(), 1.f);
	Owned->SetTextureParameterValue(Grungify::MaskTextureParameterName(), Mask);
	TestTrue(TEXT("Component-owned active explicit mask admitted"), FGrungifyRuntimeWearTestAccess::Eligible(Owned));

	UMaterialInstanceConstant* Baked = NewObject<UMaterialInstanceConstant>();
	Baked->SetParentEditorOnly(Parent, false);
	Baked->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Grungify::ActiveParameterName()), 1.f);
	Baked->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Grungify::MaskTextureParameterName()), Mask);
	TestTrue(TEXT("Standalone baked active explicit mask admitted"), FGrungifyRuntimeWearTestAccess::Eligible(Baked));
	Owned->SetScalarParameterValue(Grungify::ActiveParameterName(), 0.f);
	TestFalse(TEXT("Inactive explicit mask refused"), FGrungifyRuntimeWearTestAccess::Eligible(Owned));
	UMaterialInstanceDynamic* MissingMask = UMaterialInstanceDynamic::Create(Parent, Component);
	MissingMask->SetScalarParameterValue(Grungify::ActiveParameterName(), 1.f);
	TestFalse(TEXT("Active without an instance mask override refused"), FGrungifyRuntimeWearTestAccess::Eligible(MissingMask));
	TestFalse(TEXT("Plain shared material refused"), FGrungifyRuntimeWearTestAccess::Eligible(Parent));

	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Read-only cube fixture"), Mesh))
	{
		return false;
	}
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, Owned);
	Component->SetCustomPrimitiveDataFloat(UGrungifySettings::Get()->CustomPrimitiveDataIndex + Grungify::CPD_MaskMode, Grungify::MaskModeTexture);
	const auto OverridesBefore = Component->OverrideMaterials;
	const auto LiveDataBefore = Component->GetCustomPrimitiveData().Data;
	int32 Section = INDEX_NONE;
	if (!TestTrue(TEXT("Face zero resolves to excluded material"), Component->GetMaterialFromCollisionFaceIndex(0, Section) == Owned))
	{
		return false;
	}
	UWorld* World = NewObject<UWorld>();
	UGrungifyRuntimeWearSubsystem* Subsystem = NewObject<UGrungifyRuntimeWearSubsystem>(World);
	FHitResult Hit;
	Hit.Component = Component;
	Hit.FaceIndex = 0;
	TestFalse(TEXT("Known excluded face refused before runtime state creation"), Subsystem->AddWearAtHit(Hit, EGrungifyWearLayer::Dust));
	TestTrue(TEXT("Excluded face preserves raw overrides"), Component->OverrideMaterials == OverridesBefore);
	TestTrue(TEXT("Excluded face preserves live CPD"), Component->GetCustomPrimitiveData().Data == LiveDataBefore);
	TestEqual(TEXT("Excluded face creates no state"), FGrungifyRuntimeWearTestAccess::StateCount(*Subsystem), 0);
	TestFalse(TEXT("Excluded face creates no live mask"), FGrungifyRuntimeWearTestAccess::HasTargets(*Subsystem));
	TestFalse(TEXT("Excluded face is refused before UV lookup"), FGrungifyRuntimeWearTestAccess::WarnedMissingUV(*Subsystem));
	Hit.FaceIndex = INDEX_NONE;
	TestNull(TEXT("Unresolved face fixture returns no material"), Component->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, Section));
	TestFalse(TEXT("Unresolved face cannot fall back to painting active bindings"), Subsystem->AddWearAtHit(Hit, EGrungifyWearLayer::Dust));
	TestTrue(TEXT("Unresolved face preserves raw overrides"), Component->OverrideMaterials == OverridesBefore);
	TestTrue(TEXT("Unresolved face preserves live CPD"), Component->GetCustomPrimitiveData().Data == LiveDataBefore);
	TestEqual(TEXT("Unresolved face creates no state"), FGrungifyRuntimeWearTestAccess::StateCount(*Subsystem), 0);
	TestFalse(TEXT("Unresolved face creates no live mask"), FGrungifyRuntimeWearTestAccess::HasTargets(*Subsystem));
	TestFalse(TEXT("Unresolved face is refused before UV lookup"), FGrungifyRuntimeWearTestAccess::WarnedMissingUV(*Subsystem));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyRuntimeOverrideResetTest, "Grungify.RuntimeWear.ExactOverrideResetAndGameplayBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyRuntimeOverrideResetTest::RunTest(const FString& Parameters)
{
	UWorld* World = NewObject<UWorld>();
	UGrungifyRuntimeWearSubsystem* Subsystem = NewObject<UGrungifyRuntimeWearSubsystem>(World);
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>();
	UMaterialInterface* Original = UMaterial::GetDefaultMaterial(MD_Surface);
	UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(Original, Component);
	UMaterialInstanceDynamic* Gameplay = UMaterialInstanceDynamic::Create(Original, Component);
	const TArray<TArray<TObjectPtr<UMaterialInterface>>> Originals = { {}, { nullptr }, { nullptr, Original, nullptr } };
	for (const auto& OriginalOverrides : Originals)
	{
		FGrungifyRuntimeWearState State;
		State.Component = Component;
		State.OriginalOverrideMaterials = OriginalOverrides;
		Component->OverrideMaterials = OriginalOverrides;
		Component->SetMaterial(0, Dynamic);
		State.RuntimeOverrideMaterials = Component->OverrideMaterials;
		FGrungifyRuntimeWearSlot Slot;
		Slot.MaterialIndex = 0;
		Slot.OriginalMaterial = Original;
		Slot.Dynamic = Dynamic;
		State.Slots.Add(Slot);
		FGrungifyRuntimeWearTestAccess::AddState(*Subsystem, MoveTemp(State));
		Subsystem->ResetRuntimeWear(Component);
		TestTrue(TEXT("Unchanged runtime bindings restore exact original length and null holes"), Component->OverrideMaterials == OriginalOverrides);
		TestEqual(TEXT("Reset retires runtime state"), FGrungifyRuntimeWearTestAccess::StateCount(*Subsystem), 0);
	}

	for (bool bReassignPaintedSlot : { false, true })
	{
		FGrungifyRuntimeWearState State;
		State.Component = Component;
		State.OriginalOverrideMaterials = { nullptr, Original, nullptr };
		Component->OverrideMaterials = State.OriginalOverrideMaterials;
		Component->SetMaterial(0, Dynamic);
		State.RuntimeOverrideMaterials = Component->OverrideMaterials;
		FGrungifyRuntimeWearSlot Slot;
		Slot.MaterialIndex = 0;
		Slot.OriginalMaterial = Original;
		Slot.Dynamic = Dynamic;
		State.Slots.Add(Slot);
		FGrungifyRuntimeWearTestAccess::AddState(*Subsystem, MoveTemp(State));
		Component->SetMaterial(bReassignPaintedSlot ? 0 : 2, Gameplay);
		Subsystem->ResetRuntimeWear(Component);
		TestEqual(TEXT("Gameplay-owned binding survives reset"), Component->GetMaterial(bReassignPaintedSlot ? 0 : 2), static_cast<UMaterialInterface*>(Gameplay));
		TestEqual(TEXT("Raw array shape with gameplay reassignment preserved"), Component->OverrideMaterials.Num(), 3);
		if (!bReassignPaintedSlot)
		{
			TestNull(TEXT("Still-owned painted slot returns to raw null override"), Component->OverrideMaterials[0].Get());
		}
	}
	return true;
}

namespace GrungifyRuntimeWearTests
{
	constexpr int32 MaskSize = 64;

	/** Nonzero, varied values in all four channels, different for the mask and the extended mask. */
	TArray<FColor> MakeBasePixels(int32 Salt)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(MaskSize * MaskSize);
		for (int32 Y = 0; Y < MaskSize; ++Y)
		{
			for (int32 X = 0; X < MaskSize; ++X)
			{
				Pixels[Y * MaskSize + X] = FColor(
					uint8(20 + (X * 7 + Y * 3 + Salt) % 150),
					uint8(30 + (X * 5 + Y * 11 + Salt * 3) % 140),
					uint8(25 + (X * 13 + Y * 2 + Salt * 5) % 160),
					uint8(40 + (X * 3 + Y * 9 + Salt * 7) % 130));
			}
		}
		return Pixels;
	}

	UTexture2D* MakeMaskTexture(const TArray<FColor>& Pixels)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(MaskSize, MaskSize, PF_B8G8R8A8);
		if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
		{
			return nullptr;
		}
		Texture->SRGB = false;
		Texture->Filter = TF_Nearest;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Wrap;
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		FMemory::Memcpy(Mip.BulkData.Lock(LOCK_READ_WRITE), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		return Texture;
	}

	uint8 ChannelValue(const FColor& Color, int32 Channel)
	{
		return Channel == 0 ? Color.R : Channel == 1 ? Color.G : Channel == 2 ? Color.B : Color.A;
	}

	uint8& ChannelRef(FColor& Color, int32 Channel)
	{
		return Channel == 0 ? Color.R : Channel == 1 ? Color.G : Channel == 2 ? Color.B : Color.A;
	}

	struct FStroke
	{
		FVector2D UV;
		float Radius;
		float Amount;
	};

	/**
	 * Expected result of one stroke, computed independently of the subsystem's traversal: every texel of the texture,
	 * with the wrapped copy nearest the stroke centre (the masks wrap). Only the falloff value itself is shared.
	 */
	void ApplyExpected(TArray<FColor>& Pixels, const FStroke& Stroke, int32 Channel)
	{
		const float CentreX = float(Stroke.UV.X) * MaskSize;
		const float CentreY = float(Stroke.UV.Y) * MaskSize;
		for (int32 Y = 0; Y < MaskSize; ++Y)
		{
			for (int32 X = 0; X < MaskSize; ++X)
			{
				float BestDX = 0.f, BestDY = 0.f;
				float Best = TNumericLimits<float>::Max();
				for (int32 OffsetY : { -MaskSize, 0, MaskSize })
				{
					for (int32 OffsetX : { -MaskSize, 0, MaskSize })
					{
						const float DX = (X + OffsetX) + 0.5f - CentreX;
						const float DY = (Y + OffsetY) + 0.5f - CentreY;
						if (DX * DX + DY * DY < Best)
						{
							Best = DX * DX + DY * DY;
							BestDX = DX;
							BestDY = DY;
						}
					}
				}
				const uint8 Increment = FGrungifyRuntimeWearTestAccess::Increment(BestDX, BestDY, Stroke.Radius, Stroke.Amount);
				uint8& Value = ChannelRef(Pixels[Y * MaskSize + X], Channel);
				Value = uint8(FMath::Min(int32(Value) + int32(Increment), 255));
			}
		}
	}

	struct FChannelDiff
	{
		int32 Mismatches[8] = {};
	};

	/** Per-channel mismatch counts of the mask (channels 0-3) and extended mask (4-7). */
	FChannelDiff Compare(const TArray<FColor>& Mask, const TArray<FColor>& ExpectedMask, const TArray<FColor>& Extended, const TArray<FColor>& ExpectedExtended)
	{
		FChannelDiff Diff;
		for (int32 Index = 0; Index < MaskSize * MaskSize; ++Index)
		{
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				Diff.Mismatches[Channel] += ChannelValue(Mask[Index], Channel) != ChannelValue(ExpectedMask[Index], Channel);
				Diff.Mismatches[4 + Channel] += ChannelValue(Extended[Index], Channel) != ChannelValue(ExpectedExtended[Index], Channel);
			}
		}
		return Diff;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyRuntimeChannelAccumulationTest, "Grungify.RuntimeWear.SelectedChannelAccumulationGPU",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyRuntimeChannelAccumulationTest::RunTest(const FString& Parameters)
{
	using namespace GrungifyRuntimeWearTests;
	if (!FApp::CanEverRender())
	{
		AddError(TEXT("This test reads the live masks back from the GPU; run it with a rendering RHI (not -nullrhi)."));
		return false;
	}

	// Falloff: full Amount at the centre, smooth and non-increasing outwards, nothing at or beyond the radius.
	TestEqual(TEXT("Centre adds the full amount"), int32(FGrungifyRuntimeWearTestAccess::Increment(0.f, 0.f, 10.f, .5f)), FMath::RoundToInt(.5f * 255.f));
	TestEqual(TEXT("Radius adds nothing"), int32(FGrungifyRuntimeWearTestAccess::Increment(10.f, 0.f, 10.f, 1.f)), 0);
	TestEqual(TEXT("Beyond the radius adds nothing"), int32(FGrungifyRuntimeWearTestAccess::Increment(0.f, 12.f, 10.f, 1.f)), 0);
	for (float Distance = 0.f; Distance < 10.f; Distance += .5f)
	{
		TestTrue(TEXT("Falloff is non-increasing"), FGrungifyRuntimeWearTestAccess::Increment(Distance + .5f, 0.f, 10.f, 1.f)
			<= FGrungifyRuntimeWearTestAccess::Increment(Distance, 0.f, 10.f, 1.f));
	}

	const TArray<FColor> BaseMask = MakeBasePixels(0);
	const TArray<FColor> BaseExtended = MakeBasePixels(17);
	UTexture2D* MaskTexture = MakeMaskTexture(BaseMask);
	UTexture2D* ExtendedTexture = MakeMaskTexture(BaseExtended);
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Mask fixture"), MaskTexture) || !TestNotNull(TEXT("Extended fixture"), ExtendedTexture)
		|| !TestNotNull(TEXT("Read-only cube fixture"), Mesh))
	{
		return false;
	}

	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>();
	Component->SetStaticMesh(Mesh);
	// Runtime creates an MID from this baked binding; UE permits Material/MIC parents, not MID parents.
	UMaterialInstanceConstant* Owned = NewObject<UMaterialInstanceConstant>(Component);
	Owned->SetParentEditorOnly(UMaterial::GetDefaultMaterial(MD_Surface), false);
	Owned->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Grungify::ActiveParameterName()), 1.f);
	Owned->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Grungify::MaskTexelSizeParameterName()), 1.f);
	Owned->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Grungify::MaskTextureParameterName()), MaskTexture);
	Owned->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Grungify::ExtendedMaskTextureParameterName()), ExtendedTexture);
	Component->SetMaterial(0, Owned);
	if (!TestTrue(TEXT("Baked MIC fixture is eligible before runtime state creation"), FGrungifyRuntimeWearTestAccess::Eligible(Owned)))
	{
		return false;
	}

	UWorld* World = NewObject<UWorld>();
	UGrungifyRuntimeWearSubsystem* Subsystem = NewObject<UGrungifyRuntimeWearSubsystem>(World);

	// Texel size 1 cm, so radii are in texels. Overlapping strokes, a stroke repeated to saturation, and one that wraps.
	const TArray<FStroke> Strokes = {
		{ FVector2D(.40, .40), 9.f, .35f },
		{ FVector2D(.47, .44), 11.f, .6f },
		{ FVector2D(.44, .42), 6.f, 1.f },
		{ FVector2D(.44, .42), 6.f, 1.f },
		{ FVector2D(.44, .42), 6.f, 1.f },
		{ FVector2D(.985, .70), 5.f, .5f },
	};

	auto ReadBoth = [&](TArray<FColor>& OutMask, TArray<FColor>& OutExtended)
	{
		int32 Width = 0, Height = 0, ExtendedWidth = 0, ExtendedHeight = 0;
		return Subsystem->ReadRuntimeWearMask(Component, 0, false, OutMask, Width, Height)
			&& Subsystem->ReadRuntimeWearMask(Component, 0, true, OutExtended, ExtendedWidth, ExtendedHeight)
			&& Width == MaskSize && Height == MaskSize && ExtendedWidth == MaskSize && ExtendedHeight == MaskSize;
	};

	for (int32 LayerIndex = 0; LayerIndex < 8; ++LayerIndex)
	{
		const EGrungifyWearLayer Layer = static_cast<EGrungifyWearLayer>(LayerIndex);
		const FString Name = UEnum::GetValueAsString(Layer);
		const bool bExtended = LayerIndex >= 4;
		const int32 Channel = LayerIndex % 4;

		if (!TestNotNull(*FString::Printf(TEXT("%s: runtime state created"), *Name), FGrungifyRuntimeWearTestAccess::CreateState(*Subsystem, Component)))
		{
			return false;
		}
		TestTrue(*FString::Printf(TEXT("%s: slot switched to a runtime instance"), *Name), Component->GetMaterial(0) != Owned);
		if (!TestTrue(*FString::Printf(TEXT("%s: runtime instance inherits active baked mask admission"), *Name),
			FGrungifyRuntimeWearTestAccess::Eligible(Component->GetMaterial(0))))
		{
			return false;
		}

		TArray<FColor> GpuMask, GpuExtended;
		if (!TestTrue(*FString::Printf(TEXT("%s: GPU readback before painting"), *Name), ReadBoth(GpuMask, GpuExtended)))
		{
			return false;
		}
		TestTrue(*FString::Printf(TEXT("%s: live copy matches the baked masks in all eight channels"), *Name),
			GpuMask == BaseMask && GpuExtended == BaseExtended);

		TArray<FColor> ExpectedMask = BaseMask;
		TArray<FColor> ExpectedExtended = BaseExtended;
		for (const FStroke& Stroke : Strokes)
		{
			TestTrue(*FString::Printf(TEXT("%s: stroke lands"), *Name),
				FGrungifyRuntimeWearTestAccess::Paint(*Subsystem, Component, Stroke.UV, Layer, Stroke.Radius, Stroke.Amount));
			ApplyExpected(bExtended ? ExpectedExtended : ExpectedMask, Stroke, Channel);
		}

		if (!TestTrue(*FString::Printf(TEXT("%s: GPU readback after painting"), *Name), ReadBoth(GpuMask, GpuExtended)))
		{
			return false;
		}
		const FChannelDiff Diff = Compare(GpuMask, ExpectedMask, GpuExtended, ExpectedExtended);
		for (int32 Other = 0; Other < 8; ++Other)
		{
			TestEqual(*FString::Printf(TEXT("%s: GPU channel %d texels differing from expected"), *Name, Other), Diff.Mismatches[Other], 0);
		}
		const TArray<FColor>* CpuMask = FGrungifyRuntimeWearTestAccess::CpuPixels(*Subsystem, Component, false);
		const TArray<FColor>* CpuExtended = FGrungifyRuntimeWearTestAccess::CpuPixels(*Subsystem, Component, true);
		TestTrue(*FString::Printf(TEXT("%s: GPU matches the CPU copy"), *Name), CpuMask && CpuExtended && *CpuMask == GpuMask && *CpuExtended == GpuExtended);

		// The strokes must actually exercise accumulation, falloff, saturation and wrapping.
		const TArray<FColor>& Base = bExtended ? BaseExtended : BaseMask;
		const TArray<FColor>& Painted = bExtended ? GpuExtended : GpuMask;
		int32 Saturated = 0, Partial = 0, Wrapped = 0;
		for (int32 Index = 0; Index < MaskSize * MaskSize; ++Index)
		{
			const uint8 Before = ChannelValue(Base[Index], Channel);
			const uint8 After = ChannelValue(Painted[Index], Channel);
			Saturated += After == 255 && Before < 255;
			Partial += After > Before && After < 255;
			Wrapped += After != Before && Index % MaskSize < 3;
		}
		TestTrue(*FString::Printf(TEXT("%s: repeated strokes saturate at 255"), *Name), Saturated > 0);
		TestTrue(*FString::Printf(TEXT("%s: falloff leaves partial texels"), *Name), Partial > 0);
		TestTrue(*FString::Printf(TEXT("%s: a stroke at the edge wraps to the other side"), *Name), Wrapped > 0);

		// Reset restores the baked binding and discards paint; the baked source itself was never written.
		Subsystem->ResetRuntimeWear(Component);
		TestEqual(*FString::Printf(TEXT("%s: Reset restores the baked material"), *Name), Component->GetMaterial(0), static_cast<UMaterialInterface*>(Owned));
		TestEqual(*FString::Printf(TEXT("%s: Reset retires runtime state"), *Name), FGrungifyRuntimeWearTestAccess::StateCount(*Subsystem), 0);
		if (TestNotNull(*FString::Printf(TEXT("%s: state recreated after Reset"), *Name), FGrungifyRuntimeWearTestAccess::CreateState(*Subsystem, Component))
			&& TestTrue(*FString::Printf(TEXT("%s: GPU readback after Reset"), *Name), ReadBoth(GpuMask, GpuExtended)))
		{
			TestTrue(*FString::Printf(TEXT("%s: after Reset all eight channels are the baked values"), *Name),
				GpuMask == BaseMask && GpuExtended == BaseExtended);
		}
		Subsystem->ResetRuntimeWear(Component);
	}
	return true;
}

#endif
