// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyLibraryBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "FileHelpers.h"
#include "GrungifyPreset.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* LibraryFolder = TEXT("/Game/Grungify/Library");
	const TCHAR* ExampleMaterialPath = TEXT("/Grungify/Materials/M_GrungifyExample.M_GrungifyExample");

	/** One tuned look: a preset and base colours for its three material families. */
	struct FLook
	{
		const TCHAR* Name;
		const TCHAR* Notes;
		TFunction<void(UGrungifyPreset&)> Configure;
		FLinearColor Stone;
		FLinearColor Metal;
		float MetalMetallic;
		FLinearColor Trim;
	};

	template <typename T>
	T* FindOrCreate(const FString& Name, bool bOverwrite, bool& bOutCreated)
	{
		const FString PackageName = FString(LibraryFolder) / Name;
		const FString ObjectPath = PackageName + TEXT(".") + Name;
		bOutCreated = false;
		if (T* Existing = LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return bOverwrite ? Existing : nullptr;
		}
		UPackage* Package = CreatePackage(*PackageName);
		T* Asset = NewObject<T>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		bOutCreated = true;
		return Asset;
	}

	TArray<FLook> Looks()
	{
		TArray<FLook> Result;

		Result.Add({ TEXT("AbandonedIndustrial"),
			TEXT("Rust-streaked metal, heavy dust, chipped paint, cracked concrete. Long runoff below vents and fasteners."),
			[](UGrungifyPreset& P)
			{
				P.Intensity = 1.15f;
				P.DustIntensity = 1.2f;
				P.DustHeightCm = 80.f;
				P.LedgeDust = 0.8f;
				P.RunoffIntensity = 1.3f;
				P.RunoffLengthCm = 350.f;
				P.AmbientStreaks = 0.3f;
				P.HandlingIntensity = 0.6f;
				P.ChipIntensity = 1.1f;
				P.MetalProfile = FGrungifySurfaceProfile(0.8f, 1.4f, 0.8f, 0.9f);
				P.Extended.RustBloomIntensity = 0.8f;
				P.Extended.SootIntensity = 0.3f;
				P.Palette.bOverrideDustColor = true;
				P.Palette.DustColor = FLinearColor(0.45f, 0.42f, 0.36f);
				P.Palette.bOverrideRunoffColor = true;
				P.Palette.RunoffColor = FLinearColor(0.22f, 0.10f, 0.04f);
				P.Palette.bOverrideCrackStrength = true;
				P.Palette.CrackStrength = 0.35f;
				P.Palette.bOverrideContrast = true;
				P.Palette.Contrast = 1.25f;
			},
			FLinearColor(0.42f, 0.41f, 0.39f), FLinearColor(0.16f, 0.22f, 0.20f), 0.2f, FLinearColor(0.55f, 0.47f, 0.18f) });

		Result.Add({ TEXT("OldTemple"),
			TEXT("Worn stone: crisp chipped arrises, dust on ledges, polished hand-height edges, cracks, moss on damp sheltered faces."),
			[](UGrungifyPreset& P)
			{
				P.Intensity = 1.f;
				P.LedgeDust = 0.8f;
				P.CreviceDust = 0.8f;
				P.ChipIntensity = 1.3f;
				P.EdgeAngleDegrees = 45.f;
				P.RunoffIntensity = 0.7f;
				P.HandlingIntensity = 0.9f;
				P.GeneralHandling = 0.4f;
				P.StoneProfile = FGrungifySurfaceProfile(1.0f, 0.7f, 0.8f, 1.1f);
				P.bUseSkyExposure = true;
				P.SkyExposureInfluence = 0.5f;
				P.Extended.MoistureIntensity = 0.3f;
				P.Extended.MossIntensity = 0.6f;
				P.Palette.bOverrideDustColor = true;
				P.Palette.DustColor = FLinearColor(0.60f, 0.55f, 0.45f);
				P.Palette.bOverrideCrackStrength = true;
				P.Palette.CrackStrength = 0.6f;
				P.Palette.bOverrideMossColor = true;
				P.Palette.MossColor = FLinearColor(0.14f, 0.20f, 0.06f);
			},
			FLinearColor(0.58f, 0.53f, 0.45f), FLinearColor(0.35f, 0.24f, 0.12f), 0.8f, FLinearColor(0.66f, 0.60f, 0.50f) });

		Result.Add({ TEXT("WetSewer"),
			TEXT("Standing water and a flood line, dark streaks along traced water paths, salt bloom on brick, moss everywhere damp."),
			[](UGrungifyPreset& P)
			{
				P.Intensity = 1.f;
				P.DustIntensity = 0.3f;
				P.RunoffIntensity = 1.5f;
				P.RunoffLengthCm = 500.f;
				P.AmbientStreaks = 0.45f;
				P.ChipIntensity = 0.8f;
				P.bTraceWaterFlow = true;
				P.Extended.MoistureIntensity = 1.f;
				P.Extended.WaterLineHeightCm = 40.f;
				P.Extended.MossIntensity = 0.8f;
				P.Extended.MossNorthBias = 0.f;
				P.Extended.RustBloomIntensity = 0.6f;
				P.Palette.bOverrideRunoffColor = true;
				P.Palette.RunoffColor = FLinearColor(0.08f, 0.07f, 0.05f);
				P.Palette.bOverrideContrast = true;
				P.Palette.Contrast = 1.15f;
				P.Palette.bOverrideEfflorescenceColor = true;
				P.Palette.EfflorescenceColor = FLinearColor(0.82f, 0.80f, 0.74f);
			},
			FLinearColor(0.36f, 0.22f, 0.16f), FLinearColor(0.20f, 0.20f, 0.19f), 0.6f, FLinearColor(0.40f, 0.38f, 0.34f) });

		Result.Add({ TEXT("DryCoatedCeramic"),
			TEXT("Dry glazed or coated surfaces (coffers, panels, ceramic trim): fine broken chips on a narrow worn edge, intact faces, restrained local grime. ")
			TEXT("A starting point, not an approved look: tune Chip Scale, Coverage, Width and Breakup on the real asset."),
			[](UGrungifyPreset& P)
			{
				P.Intensity = 1.f;
				P.FeatureScaleCm = 6.f;
				P.DustIntensity = 0.45f;
				P.LedgeDust = 0.35f;
				P.CreviceDust = 0.4f;
				P.FloorDust = 0.2f;
				P.RunoffIntensity = 0.25f;
				P.AmbientStreaks = 0.05f;
				P.HandlingIntensity = 0.45f;
				P.ChipIntensity = 1.f;
				P.EdgeWearWidthCm = 4.f;
				P.Palette.bOverrideChipStrength = true;
				P.Palette.ChipStrength = 0.75f;
				P.Palette.bOverrideDustStrength = true;
				P.Palette.DustStrength = 0.45f;
				P.Palette.bOverrideHandlingStrength = true;
				P.Palette.HandlingStrength = 0.45f;
				P.Palette.bOverrideContrast = true;
				P.Palette.Contrast = 1.1f;
				P.Palette.bOverrideChipScaleCm = true;
				P.Palette.ChipScaleCm = 1.5f;
				P.Palette.bOverrideChipCoverage = true;
				P.Palette.ChipCoverage = 0.35f;
				P.Palette.bOverrideChipWidth = true;
				P.Palette.ChipWidth = 0.4f;
				P.Palette.bOverrideChipBreakup = true;
				P.Palette.ChipBreakup = 0.7f;
				P.Palette.bOverrideSurfaceProtection = true;
				P.Palette.SurfaceProtection = 0.25f;
				P.Palette.bOverrideGrimeBreakup = true;
				P.Palette.GrimeBreakup = 0.6f;
				P.Palette.bOverrideRoughnessVariation = true;
				P.Palette.RoughnessVariation = 0.2f;
			},
			FLinearColor(0.72f, 0.70f, 0.66f), FLinearColor(0.30f, 0.30f, 0.31f), 0.9f, FLinearColor(0.78f, 0.75f, 0.68f) });

		return Result;
	}

	/** Shared by Build and ConfigureLook so a preset made elsewhere matches the library's GP_<Look> field for field. */
	void WriteLook(const FLook& Look, UGrungifyPreset& Preset)
	{
		// Start from defaults so overwriting a preset doesn't keep stale tweaks.
		const UGrungifyPreset* Defaults = GetDefault<UGrungifyPreset>();
#define GRUNGIFY_RESET_FIELD(Field) Preset.Field = Defaults->Field;
		GRUNGIFY_PRESET_FIELDS(GRUNGIFY_RESET_FIELD)
#undef GRUNGIFY_RESET_FIELD
		Preset.Notes = Look.Notes;
		Preset.bIncludeSeed = false;
		Look.Configure(Preset);
	}
}

TArray<FString> FGrungifyLibraryBuilder::LookNames()
{
	TArray<FString> Names;
	for (const FLook& Look : Looks())
	{
		Names.Add(Look.Name);
	}
	return Names;
}

bool FGrungifyLibraryBuilder::ConfigureLook(const FString& LookName, UGrungifyPreset& Preset)
{
	for (const FLook& Look : Looks())
	{
		if (LookName.Equals(Look.Name, ESearchCase::IgnoreCase))
		{
			WriteLook(Look, Preset);
			return true;
		}
	}
	return false;
}

bool FGrungifyLibraryBuilder::GetLookColors(const FString& LookName, FGrungifyLookColors& OutColors)
{
	for (const FLook& Look : Looks())
	{
		if (LookName.Equals(Look.Name, ESearchCase::IgnoreCase))
		{
			OutColors.Stone = Look.Stone;
			OutColors.Metal = Look.Metal;
			OutColors.MetalMetallic = Look.MetalMetallic;
			OutColors.Trim = Look.Trim;
			return true;
		}
	}
	return false;
}

bool FGrungifyLibraryBuilder::Build(bool bOverwrite, FString& OutMessage)
{
	UMaterialInterface* Example = LoadObject<UMaterialInterface>(nullptr, ExampleMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Example)
	{
		OutMessage = TEXT("M_GrungifyExample is missing; run Tools > Grungify > Update Plugin Content first.");
		return false;
	}

	TArray<UPackage*> Packages;
	int32 Created = 0;
	int32 Updated = 0;
	for (const FLook& Look : Looks())
	{
		bool bCreated = false;
		if (UGrungifyPreset* Preset = FindOrCreate<UGrungifyPreset>(FString::Printf(TEXT("GP_%s"), Look.Name), bOverwrite, bCreated))
		{
			Preset->Modify();
			WriteLook(Look, *Preset);
			Preset->MarkPackageDirty();
			Packages.Add(Preset->GetPackage());
			(bCreated ? Created : Updated) += 1;
		}

		struct FFamily { const TCHAR* Suffix; FLinearColor Color; float Metallic; float Roughness; float Surface; };
		const FFamily Families[] = {
			{ TEXT("Stone"), Look.Stone, 0.f, 0.75f, 0.f },
			{ TEXT("Metal"), Look.Metal, Look.MetalMetallic, 0.45f, 1.f },
			{ TEXT("Trim"), Look.Trim, 0.f, 0.6f, 2.f } };
		for (const FFamily& Family : Families)
		{
			UMaterialInstanceConstant* Instance = FindOrCreate<UMaterialInstanceConstant>(
				FString::Printf(TEXT("MI_Grungify_%s_%s"), Look.Name, Family.Suffix), bOverwrite, bCreated);
			if (!Instance)
			{
				continue;
			}
			Instance->Modify();
			Instance->SetParentEditorOnly(Example);
			Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Base Color")), Family.Color);
			Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), Family.Roughness);
			Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Metallic")), Family.Metallic);
			Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Grungify::SurfaceOverrideParameterName()), Family.Surface);
			Instance->PostEditChange();
			Instance->MarkPackageDirty();
			Packages.Add(Instance->GetPackage());
			(bCreated ? Created : Updated) += 1;
		}
	}

	if (Packages.Num() == 0)
	{
		OutMessage = TEXT("Library already exists in /Game/Grungify/Library (use Rebuild to overwrite).");
		return true;
	}
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false);
	OutMessage = FString::Printf(TEXT("Material library: %d created, %d updated in /Game/Grungify/Library%s."),
		Created, Updated, bSaved ? TEXT("") : TEXT(" (some packages failed to save)"));
	return bSaved;
}
