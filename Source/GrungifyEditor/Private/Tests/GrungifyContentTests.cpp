// This regression deliberately rebuilds generated plugin content in an isolated BuildPlugin HostProject.
#include "GrungifyContentBuilder.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "GrungifySettings.h"
#include "GrungifyTypes.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyContentRegenerationTest, "Grungify.Content.InPlaceRegeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyContentRegenerationTest::RunTest(const FString& Parameters)
{
	if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("HostProject"))
	{
		AddError(TEXT("Generated-content regeneration must run in the isolated BuildPlugin HostProject, not a production project."));
		return false;
	}

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

		const TArray<int32> Counts{ Function->GetExpressions().Num(), Debug->GetExpressions().Num(), Example->GetExpressions().Num() };
		if (OriginalExpressionCounts.Num() == 0) { OriginalExpressionCounts = Counts; }
		for (int32 Index = 0; Index < Counts.Num(); ++Index)
		{
			TestEqual(TEXT("Rebuild retains exact expression count; no stale nodes"), Counts[Index], OriginalExpressionCounts[Index]);
		}

		auto CheckOwnership = [&](TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions)
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
						TestTrue(TEXT("Mode uses CPD"), Parameter->bUseCustomPrimitiveData);
						TestEqual(TEXT("Mode defaults to zero"), Parameter->DefaultValue, 0.f);
						TestEqual(TEXT("Mode follows current CPD base"), int32(Parameter->PrimitiveDataIndex), Base + Grungify::CPD_MaskMode);
					}
				}
			}
			TestEqual(TEXT("Exactly one Active parameter"), ActiveCount, 1);
			TestEqual(TEXT("Exactly one Mode parameter"), ModeCount, 1);
		};
		CheckOwnership(Function->GetExpressions());
		CheckOwnership(Debug->GetExpressions());

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
