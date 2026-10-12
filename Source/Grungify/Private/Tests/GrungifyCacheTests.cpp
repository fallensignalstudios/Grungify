// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyGenerator.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifySlotCacheIdentityTest, "Grungify.Generation.EffectiveSlotCacheIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifySlotCacheIdentityTest::RunTest(const FString& Parameters)
{
	FGrungifyComponentContext Original;
	Original.Slots.AddDefaulted();
	const uint32 OriginalHash = Grungify::HashSlotContext(Original);
	const TCHAR* Names[] = { TEXT("Dust"), TEXT("Runoff"), TEXT("Handling"), TEXT("Edge chips") };
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FGrungifyComponentContext Edited = Original;
		float* Values[] = { &Edited.Slots[0].Profile.Dust, &Edited.Slots[0].Profile.Runoff,
			&Edited.Slots[0].Profile.Handling, &Edited.Slots[0].Profile.EdgeChips };
		*Values[Index] = 0.f;
		TestTrue(FString(Names[Index]) + TEXT(" profile edit invalidates cached masks with the same report label"),
			Grungify::HashSlotContext(Edited) != OriginalHash);
	}

	FGrungifyGate Gate;
	Gate.bUseHeightGate = true;
	Gate.MaxHeightCm = 100.f;
	Gate.bUseFacingGate = true;
	Gate.MaxUpFacing = 0.9f;
	Original.Slots[0].Gate = &Gate;
	const uint32 GateHash = Grungify::HashSlotContext(Original);
	Gate.MaxHeightCm += 0.1f; // Same rounded diagnostic string, different effective gate.
	TestTrue(TEXT("Sub-centimetre height edits invalidate the cache"), Grungify::HashSlotContext(Original) != GateHash);
	Gate.MaxHeightCm = 100.f;
	Gate.MaxUpFacing += 0.001f;
	TestTrue(TEXT("Sub-percent facing edits invalidate the cache"), Grungify::HashSlotContext(Original) != GateHash);
	Gate.MaxUpFacing = 0.9f;

	UTexture2D* Texture = NewObject<UTexture2D>();
	uint8 Pixels[] = { 0, 255 };
	Texture->Source.Init(2, 1, 1, 1, TSF_G8, Pixels);
	Gate.GateTexture = Texture;
	FGrungifyGateSampler Captured, Reimported;
	FString Error;
	if (!TestTrue(TEXT("Original gate source readable"), Captured.Init(Texture, Error))) { return false; }
	Original.Slots[0].GateSampler = &Captured;
	const uint32 CapturedHash = Grungify::HashSlotContext(Original);
	Pixels[1] = 0;
	Texture->Source.Init(2, 1, 1, 1, TSF_G8, Pixels);
	if (!TestTrue(TEXT("Reimported gate source readable"), Reimported.Init(Texture, Error))) { return false; }
	Original.Slots[0].GateSampler = &Reimported;
	TestTrue(TEXT("Reimporting the same texture object invalidates cached masks"), Grungify::HashSlotContext(Original) != CapturedHash);
	TestEqual(TEXT("The running job retains its copied original gate pixels"), Captured.Sample(FVector2f(0.75f, 0.f), EGrungifyChannel::R), 1.f);
	TestEqual(TEXT("The new job sees the reimported pixels"), Reimported.Sample(FVector2f(0.75f, 0.f), EGrungifyChannel::R), 0.f);

	FGrungifySegmentField First, Second;
	First.Reset(50.0);
	Second.Reset(50.0);
	FGrungifySegmentField::FSegment Segment{ FVector::ZeroVector, FVector(100.0, 0.0, 0.0) };
	First.Add(Segment);
	Segment.AlongB = 0.2f;
	Second.Add(Segment);
	TestTrue(TEXT("Water-path falloff changes invalidate cached masks"), First.Hash() != Second.Hash());
	return !HasAnyErrors();
}

#endif
