// Copyright Fallen Signal Studios. All Rights Reserved.
// Runs in packaged Development builds too (no editor APIs), to prove per-instance mask textures and owned
// material instances survive cooking: -ExecCmds="Automation RunTests Grungify.Runtime"
#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GrungifySettings.h"
#include "GrungifyTypes.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstance.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Tests/AutomationCommon.h"

namespace
{
	// Built by FGrungifyReferenceScene in the editor module; keep the two in sync.
	const TCHAR* ReferenceMapPackage = TEXT("/Game/Grungify/Reference/L_GrungifyReference");
	constexpr double MapLoadTimeoutSeconds = 60.0;

	bool HasGrungifyTag(const AActor& Actor)
	{
		for (const FName& Tag : Actor.Tags)
		{
			if (Tag.ToString().StartsWith(TEXT("Grungify.")))
			{
				return true;
			}
		}
		return false;
	}

	/** The reference map if it is already loaded in any world context (game, PIE or editor). */
	UWorld* FindReferenceWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World && World->PersistentLevel && UWorld::RemovePIEPrefix(World->GetPackage()->GetName()) == ReferenceMapPackage)
			{
				return World;
			}
		}
		return nullptr;
	}

	void CheckReferenceWorld(FAutomationTestBase& Test, const UWorld& World)
	{
		const int32 ModeIndex = UGrungifySettings::Get()->CustomPrimitiveDataIndex + Grungify::CPD_MaskMode;
		int32 Checked = 0;
		for (const AActor* Actor : World.PersistentLevel->Actors)
		{
			if (!Actor || !HasGrungifyTag(*Actor))
			{
				continue;
			}
			TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
			for (const UStaticMeshComponent* Component : Components)
			{
				++Checked;
				const FString What = FString::Printf(TEXT("%s.%s"), *Actor->GetActorNameOrLabel(), *Component->GetName());

				// Live data is filled from the saved defaults on registration; a world loaded without
				// registering its components (editor, map not open) only has the defaults.
				const TArray<float>& Live = Component->GetCustomPrimitiveData().Data;
				const TArray<float>& Data = Live.IsValidIndex(ModeIndex) ? Live : Component->GetDefaultCustomPrimitiveData().Data;
				const float Mode = Data.IsValidIndex(ModeIndex) ? Data[ModeIndex] : 0.f;
				const bool bVertex = FMath::IsNearlyEqual(Mode, Grungify::MaskModeVertex);
				const bool bTexture = FMath::IsNearlyEqual(Mode, Grungify::MaskModeTexture);
				const bool bInstanced = FMath::IsNearlyEqual(Mode, Grungify::MaskModeInstanced);
				Test.TestTrue(FString::Printf(TEXT("%s: custom primitive data %d is a Grungify mask mode (got %g)"), *What, ModeIndex, Mode),
					bVertex || bTexture || bInstanced);

				bool bActiveSlot = false;
				bool bMaskTexture = false;
				for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
				{
					// Only instances the zone created inside the component count; a shared asset instance would weather every user.
					const UMaterialInterface* Material = Component->GetMaterial(Slot);
					if (!Material || !Material->IsA<UMaterialInstance>() || Material->GetOuter() != Component)
					{
						continue;
					}
					float Active = 0.f;
					if (!Material->GetScalarParameterValue(FMaterialParameterInfo(Grungify::ActiveParameterName()), Active)
						|| !FMath::IsNearlyEqual(Active, 1.f))
					{
						continue;
					}
					bActiveSlot = true;
					// Overridden only: the master material's default is the black no-mask texture, which would hide a lost bake.
					UTexture* Mask = nullptr;
					if (Material->GetTextureParameterValue(FMaterialParameterInfo(Grungify::MaskTextureParameterName()), Mask, /*bOveriddenOnly*/ true) && Mask)
					{
						bMaskTexture = true;
					}
				}
				Test.TestTrue(FString::Printf(TEXT("%s: an owned material instance has Grungify Active = 1"), *What), bActiveSlot);
				if (bTexture)
				{
					Test.TestTrue(FString::Printf(TEXT("%s: texture mode binds a per-instance Grungify Mask Texture"), *What), bMaskTexture);
				}
			}
		}
		if (Checked == 0)
		{
			Test.AddError(FString::Printf(TEXT("%s has no static meshes on actors with a Grungify.* tag; rebuild the reference scene"), ReferenceMapPackage));
		}
		else
		{
			Test.AddInfo(FString::Printf(TEXT("Checked %d weathered component(s) in %s"), Checked, ReferenceMapPackage));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyReferenceSceneCookedTest, "Grungify.Runtime.ReferenceSceneCooked",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGrungifyReferenceSceneCookedTest::RunTest(const FString& Parameters)
{
	if (!FPackageName::DoesPackageExist(ReferenceMapPackage))
	{
		AddWarning(FString::Printf(TEXT("Skipped: %s is not present. Build it in the editor (Grungify reference scene) and, for packaged builds, add it to the maps to cook."),
			ReferenceMapPackage));
		return true;
	}

	if (GIsEditor)
	{
		// The editor can inspect the saved map directly; opening it would replace the user's level.
		const UWorld* World = FindReferenceWorld();
		if (!World)
		{
			const FString ObjectPath = FString::Printf(TEXT("%s.%s"), ReferenceMapPackage, *FPackageName::GetShortName(ReferenceMapPackage));
			World = LoadObject<UWorld>(nullptr, *ObjectPath);
		}
		if (!World || !World->PersistentLevel)
		{
			AddError(FString::Printf(TEXT("Could not load %s"), ReferenceMapPackage));
			return false;
		}
		CheckReferenceWorld(*this, *World);
		return true;
	}

	AutomationOpenMap(ReferenceMapPackage);
	const double StartSeconds = FPlatformTime::Seconds();
	TSharedRef<int32> FramesSinceLoad = MakeShared<int32>(0);
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FFunctionLatentCommand>([this, StartSeconds, FramesSinceLoad]()
	{
		const UWorld* World = FindReferenceWorld();
		if (!World)
		{
			if (FPlatformTime::Seconds() - StartSeconds > MapLoadTimeoutSeconds)
			{
				AddError(FString::Printf(TEXT("Timed out opening %s"), ReferenceMapPackage));
				return true;
			}
			return false;
		}
		// One rendered frame after the load, so components are registered and their render state exists.
		if ((*FramesSinceLoad)++ < 1)
		{
			return false;
		}
		CheckReferenceWorld(*this, *World);
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
