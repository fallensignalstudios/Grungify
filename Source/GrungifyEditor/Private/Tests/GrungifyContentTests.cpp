// Copyright Fallen Signal Studios. All Rights Reserved.
// This regression deliberately rebuilds generated plugin content in an isolated BuildPlugin HostProject.
#include "GrungifyContentBuilder.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "GrungifySettings.h"
#include "GrungifyTypes.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyContentRegenerationTest, "Grungify.Content.InPlaceRegeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyContentRegenerationTest::RunTest(const FString& Parameters)
{
	if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("HostProject"))
	{
		AddError(TEXT("Generated-content regeneration must run in the isolated BuildPlugin HostProject, not a production project."));
		return false;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Grungify"));
	if (!TestTrue(TEXT("Isolated Grungify plugin exists"), Plugin.IsValid())) { return false; }
	if (!TestTrue(TEXT("Regeneration fixture is a project plugin"), Plugin->GetType() == EPluginType::Project)
		|| !TestTrue(TEXT("Regeneration fixture retains BuildPlugin Installed metadata"), Plugin->GetDescriptor().bInstalled)) { return false; }

	UGrungifySettings* Settings = GetMutableDefault<UGrungifySettings>();
	const int32 OriginalBase = Settings->CustomPrimitiveDataIndex;
	TGuardValue<int32> RestoreBase(Settings->CustomPrimitiveDataIndex, OriginalBase);
	TArray<int32> OriginalExpressionCounts;
	for (const int32 Base : { 0, 12, 0 })
	{
		Settings->CustomPrimitiveDataIndex = Base;
		const FGrungifyContentReport Report = FGrungifyContentBuilder::Run(true);
		TestTrue(TEXT("Rebuilt content is ready"), Report.bReady);
		TestTrue(TEXT("All generated packages saved"), Report.bSaveAttempted && Report.bSaved);
		TestEqual(TEXT("No rebuild errors"), Report.Errors.Num(), 0);
		for (const FString& Error : Report.Errors) { AddError(Error); }
		if (Report.HasErrors()) { return false; }

		UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr,
			TEXT("/Grungify/Materials/MF_GrungifyWeathering.MF_GrungifyWeathering"));
		UMaterial* Debug = LoadObject<UMaterial>(nullptr, TEXT("/Grungify/Materials/M_GrungifyMaskDebug.M_GrungifyMaskDebug"));
		UMaterial* Example = LoadObject<UMaterial>(nullptr, TEXT("/Grungify/Materials/M_GrungifyExample.M_GrungifyExample"));
		if (!TestNotNull(TEXT("Generated function"), Function) || !TestNotNull(TEXT("Generated debug material"), Debug)
			|| !TestNotNull(TEXT("Generated example material"), Example)) { return false; }

		UMaterialFunction* SubstrateFunction = LoadObject<UMaterialFunction>(nullptr,
			TEXT("/Grungify/Materials/MF_GrungifySubstrate.MF_GrungifySubstrate"));
		if (!TestNotNull(TEXT("Generated Substrate function"), SubstrateFunction)) { return false; }
		UMaterialFunction* LandscapeFunction = LoadObject<UMaterialFunction>(nullptr,
			TEXT("/Grungify/Materials/MF_GrungifyLandscape.MF_GrungifyLandscape"));
		UMaterialFunction* DisplacementFunction = LoadObject<UMaterialFunction>(nullptr,
			TEXT("/Grungify/Materials/MF_GrungifyDisplacement.MF_GrungifyDisplacement"));
		if (!TestNotNull(TEXT("Generated landscape function"), LandscapeFunction)
			|| !TestNotNull(TEXT("Generated displacement function"), DisplacementFunction)) { return false; }

		const TArray<int32> Counts{ Function->GetExpressions().Num(), Debug->GetExpressions().Num(), Example->GetExpressions().Num(),
			SubstrateFunction->GetExpressions().Num(), LandscapeFunction->GetExpressions().Num(), DisplacementFunction->GetExpressions().Num() };
		if (OriginalExpressionCounts.Num() == 0) { OriginalExpressionCounts = Counts; }
		for (int32 Index = 0; Index < Counts.Num(); ++Index)
		{
			TestEqual(TEXT("Rebuild retains exact expression count; no stale nodes"), Counts[Index], OriginalExpressionCounts[Index]);
		}

		auto CheckOwnership = [&](TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions, bool bModeUsesCPD = true)
		{
			int32 ActiveCount = 0;
			int32 ModeCount = 0;
			for (UMaterialExpression* Expression : Expressions)
			{
				if (const UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
				{
					if (Parameter->ParameterName == Grungify::ActiveParameterName())
					{
						++ActiveCount;
						TestFalse(TEXT("Active is not CPD"), Parameter->bUseCustomPrimitiveData);
						TestEqual(TEXT("Active remains inactive by default"), Parameter->DefaultValue, 0.f);
					}
					if (Parameter->ParameterName == TEXT("Grungify Mask Mode"))
					{
						++ModeCount;
						TestEqual(TEXT("Mode follows mesh or landscape data contract"), Parameter->bUseCustomPrimitiveData, bModeUsesCPD);
						TestEqual(TEXT("Mode defaults to zero"), Parameter->DefaultValue, 0.f);
						if (bModeUsesCPD)
						{
							TestEqual(TEXT("Mode follows current CPD base"), int32(Parameter->PrimitiveDataIndex), Base + Grungify::CPD_MaskMode);
						}
					}
				}
			}
			TestEqual(TEXT("Exactly one Active parameter"), ActiveCount, 1);
			TestEqual(TEXT("Exactly one Mode parameter"), ModeCount, 1);
		};
		CheckOwnership(Function->GetExpressions());
		CheckOwnership(Debug->GetExpressions());
		CheckOwnership(SubstrateFunction->GetExpressions());
		CheckOwnership(LandscapeFunction->GetExpressions(), false);
		CheckOwnership(DisplacementFunction->GetExpressions());
		TSet<FName> LandscapeOutputs;
		for (UMaterialExpression* Expression : LandscapeFunction->GetExpressions())
		{
			if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				LandscapeOutputs.Add(Output->OutputName);
				TestNotNull(TEXT("Landscape output is connected"), Output->A.Expression);
			}
			if (const UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				TestFalse(TEXT("Landscape parameters never read CPD"), Parameter->bUseCustomPrimitiveData);
			}
		}
		TestTrue(TEXT("Exact landscape outputs"), LandscapeOutputs.Num() == 4 && LandscapeOutputs.Contains(TEXT("BaseColor"))
			&& LandscapeOutputs.Contains(TEXT("Roughness")) && LandscapeOutputs.Contains(TEXT("Metallic")) && LandscapeOutputs.Contains(TEXT("Normal")));
		int32 DisplacementOutputs = 0;
		int32 DisplacementDepths = 0;
		for (UMaterialExpression* Expression : DisplacementFunction->GetExpressions())
		{
			if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				++DisplacementOutputs;
				TestEqual(TEXT("Displacement output name"), Output->OutputName, FName(TEXT("Displacement")));
				TestNotNull(TEXT("Displacement output is connected"), Output->A.Expression);
			}
			if (const UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				if (Parameter->ParameterName == TEXT("Grungify Displacement Depth"))
				{
					++DisplacementDepths;
					TestEqual(TEXT("Displacement defaults to off"), Parameter->DefaultValue, 0.f);
				}
			}
		}
		TestEqual(TEXT("Exactly one displacement output"), DisplacementOutputs, 1);
		TestEqual(TEXT("Exactly one displacement depth parameter"), DisplacementDepths, 1);
		int32 SubstrateOutputs = 0;
		for (UMaterialExpression* Expression : SubstrateFunction->GetExpressions())
		{
			if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				++SubstrateOutputs;
				TestEqual(TEXT("Substrate function outputs Material"), Output->OutputName, FName(TEXT("Material")));
				TestNotNull(TEXT("Substrate output is connected"), Output->A.Expression);
			}
		}
		TestEqual(TEXT("Exactly one Substrate function output"), SubstrateOutputs, 1);

		TSet<FName> OutputNames;
		int32 OutputCount = 0;
		const UMaterialExpressionCustom* RoughnessResult = nullptr;
		const UMaterialExpressionCustom* MetallicResult = nullptr;
		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				++OutputCount;
				OutputNames.Add(Output->OutputName);
				TestNotNull(TEXT("Every function output is connected"), Output->A.Expression);
				if (Output->OutputName == TEXT("Roughness") || Output->OutputName == TEXT("Metallic"))
				{
					const bool bRoughness = Output->OutputName == TEXT("Roughness");
					const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Output->A.Expression);
					if (!TestNotNull(TEXT("Surface output uses component mask"), Mask)) { return false; }
					TestTrue(TEXT("Roughness R-only; Metallic G-only"), Mask->R == bRoughness && Mask->G != bRoughness && !Mask->B && !Mask->A);
					const UMaterialExpressionCustom* Result = Cast<UMaterialExpressionCustom>(Mask->Input.Expression);
					if (!TestNotNull(TEXT("Surface mask reads custom result"), Result)) { return false; }
					TestTrue(TEXT("Surface custom returns float2"), Result->OutputType == CMOT_Float2);
					TestEqual(TEXT("Mask selects primary custom output"), Mask->Input.OutputIndex, 0);
					if (bRoughness) { RoughnessResult = Result; } else { MetallicResult = Result; }
				}
			}
		}
		TestEqual(TEXT("Exactly four function outputs"), OutputCount, 4);
		TestTrue(TEXT("Exact named function outputs"), OutputNames.Num() == 4 && OutputNames.Contains(TEXT("BaseColor"))
			&& OutputNames.Contains(TEXT("Roughness")) && OutputNames.Contains(TEXT("Metallic")) && OutputNames.Contains(TEXT("Normal")));
		TestTrue(TEXT("Roughness and metallic share the surface result"), RoughnessResult && RoughnessResult == MetallicResult);
		TestFalse(TEXT("Saved graph and token need no further rebuild"), FGrungifyContentBuilder::NeedsWork());

		// Readiness must authenticate the real saved bytes of all eight outputs, not merely their in-memory graphs.
		FString Token;
		const FString TokenPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"), TEXT("ContentReady.txt"));
		if (!TestTrue(TEXT("Saved readiness token is readable"), FFileHelper::LoadFileToString(Token, *TokenPath))) { return false; }
		TArray<FString> TokenLines;
		Token.ParseIntoArrayLines(TokenLines);
		TestEqual(TEXT("Token has marker plus shader and eight generated files"), TokenLines.Num(), 10);
		const FString Marker = FString::Printf(TEXT("[GrungifyContent v%d cpd=%d uv=%d inst=%d]"), Grungify::ContentVersion, Base,
			Settings->MaskUVChannel, Settings->InstanceCustomDataIndex);
		if (TokenLines.Num() == 0) { return false; }
		TestEqual(TEXT("Token uses the current generated-content settings"), TokenLines[0], Marker);
		TArray<FString> SavedFiles{ FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"), TEXT("Private"), TEXT("Grungify.ush")) };
		for (const TCHAR* Package : { TEXT("/Grungify/BP_WeatheringZone"), TEXT("/Grungify/Materials/MF_GrungifyWeathering"),
			TEXT("/Grungify/Materials/MF_GrungifySubstrate"), TEXT("/Grungify/Materials/MF_GrungifyLandscape"),
			TEXT("/Grungify/Materials/MF_GrungifyDisplacement"), TEXT("/Grungify/Materials/M_GrungifyMaskDebug"),
			TEXT("/Grungify/Materials/M_GrungifyExample"), TEXT("/Grungify/Materials/T_GrungifyNoMask") })
		{
			SavedFiles.Add(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension()));
		}
		for (const FString& File : SavedFiles)
		{
			TArray<uint8> Bytes;
			if (!TestTrue(TEXT("Every readiness file is saved and readable"), FFileHelper::LoadFileToArray(Bytes, *File) && Bytes.Num() > 0)) { return false; }
			const FString ExpectedLine = File + TEXT(" ") + FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num()).ToString();
			TestTrue(TEXT("Readiness hash matches each actual saved file"), TokenLines.Contains(ExpectedLine));
		}
	}

	// Restore both the setting and generated graphs if the caller began with a different CPD base.
	Settings->CustomPrimitiveDataIndex = OriginalBase;
	if (OriginalBase != 0)
	{
		const FGrungifyContentReport RestoreReport = FGrungifyContentBuilder::Run(true);
		TestFalse(TEXT("Original generated-content setting restored"), RestoreReport.HasErrors());
	}
	return !HasAnyErrors();
}

#endif
