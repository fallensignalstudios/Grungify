// Copyright Fallen Signal Studios. All Rights Reserved.
// Admission and material-reset coverage only; these tests do not paint or prove GPU channel output.
#include "GrungifyRuntimeWear.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GrungifySettings.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"

struct FGrungifyRuntimeWearTestAccess
{
	static bool Supports(EGrungifyWearLayer Layer) { return UGrungifyRuntimeWearSubsystem::SupportsRuntimeLayer(Layer); }
	static bool Eligible(const UMaterialInterface* Material) { return UGrungifyRuntimeWearSubsystem::IsEligibleMaterial(Material); }
	static int32 StateCount(const UGrungifyRuntimeWearSubsystem& Subsystem) { return Subsystem.States.Num(); }
	static bool HasBrush(const UGrungifyRuntimeWearSubsystem& Subsystem) { return Subsystem.BrushTexture != nullptr; }
	static bool WarnedMissingUV(const UGrungifyRuntimeWearSubsystem& Subsystem) { return Subsystem.bWarnedMissingUV; }
	static void AddState(UGrungifyRuntimeWearSubsystem& Subsystem, FGrungifyRuntimeWearState&& State)
	{
		Subsystem.States.Add(MoveTemp(State));
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
	for (EGrungifyWearLayer Layer : { EGrungifyWearLayer::Handling, EGrungifyWearLayer::Moss,
		static_cast<EGrungifyWearLayer>(8), static_cast<EGrungifyWearLayer>(255) })
	{
		TestFalse(TEXT("Unsupported layer is refused"), Subsystem->AddWearAtHit(Hit, Layer));
		TestTrue(TEXT("Raw material overrides unchanged"), Component->OverrideMaterials == OverridesBefore);
		TestTrue(TEXT("Default CPD unchanged"), Component->GetDefaultCustomPrimitiveData().Data == DefaultDataBefore);
		TestTrue(TEXT("Live CPD unchanged"), Component->GetCustomPrimitiveData().Data == LiveDataBefore);
		TestEqual(TEXT("No runtime state created"), FGrungifyRuntimeWearTestAccess::StateCount(*Subsystem), 0);
		TestFalse(TEXT("No brush created"), FGrungifyRuntimeWearTestAccess::HasBrush(*Subsystem));
	}
	for (EGrungifyWearLayer Layer : { EGrungifyWearLayer::EdgeChips, EGrungifyWearLayer::Dust,
		EGrungifyWearLayer::Runoff, EGrungifyWearLayer::Moisture, EGrungifyWearLayer::Soot, EGrungifyWearLayer::Scorch })
	{
		TestTrue(TEXT("RGB layer passes layer admission"), FGrungifyRuntimeWearTestAccess::Supports(Layer));
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
	TestFalse(TEXT("Excluded face creates no brush"), FGrungifyRuntimeWearTestAccess::HasBrush(*Subsystem));
	TestFalse(TEXT("Excluded face is refused before UV lookup"), FGrungifyRuntimeWearTestAccess::WarnedMissingUV(*Subsystem));
	Hit.FaceIndex = INDEX_NONE;
	TestNull(TEXT("Unresolved face fixture returns no material"), Component->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, Section));
	TestFalse(TEXT("Unresolved face cannot fall back to painting active bindings"), Subsystem->AddWearAtHit(Hit, EGrungifyWearLayer::Dust));
	TestTrue(TEXT("Unresolved face preserves raw overrides"), Component->OverrideMaterials == OverridesBefore);
	TestTrue(TEXT("Unresolved face preserves live CPD"), Component->GetCustomPrimitiveData().Data == LiveDataBefore);
	TestEqual(TEXT("Unresolved face creates no state"), FGrungifyRuntimeWearTestAccess::StateCount(*Subsystem), 0);
	TestFalse(TEXT("Unresolved face creates no brush"), FGrungifyRuntimeWearTestAccess::HasBrush(*Subsystem));
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

#endif
