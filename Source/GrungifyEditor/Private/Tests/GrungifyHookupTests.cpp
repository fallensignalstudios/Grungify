#include "GrungifyMaterialHookup.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyHookupRoundTripTest, "Grungify.Content.MaterialHookupRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyHookupRoundTripTest::RunTest(const FString& Parameters)
{
	// An unsaved project-path material, so the tool treats it as editable; nothing is written to disk.
	UPackage* Package = CreatePackage(TEXT("/Game/GrungifyAutomation/M_HookupRoundTrip"));
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_HookupRoundTrip"), RF_Transient);
	UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
	if (!TestNotNull(TEXT("Editor data"), Data)) { return false; }

	// Base colour from a parameter (with a component mask), roughness as Use Constant, metallic and normal unconnected.
	UMaterialExpressionVectorParameter* Color = Cast<UMaterialExpressionVectorParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -500, 0));
	Color->ParameterName = TEXT("Tint");
	Data->BaseColor.Connect(0, Color);
	Data->BaseColor.SetMask(1, 1, 1, 1, 0);
	Data->Roughness.UseConstant = 1;
	Data->Roughness.Constant = 0.37f;
	const int32 OriginalCount = Material->GetExpressions().Num();

	const FGrungifyHookupResult Added = UGrungifyMaterialHookup::AddGrungifyToMaterial(Material, /*bBackup*/ false);
	if (Added.Outcome != TEXT("added"))
	{
		AddError(FString::Printf(TEXT("Add failed: %s %s"), *Added.Outcome, *Added.Notes));
		return false;
	}
	TestEqual(TEXT("Legacy path"), Added.Path, FString(TEXT("legacy")));
	TestTrue(TEXT("Material now uses Grungify"), UGrungifyMaterialHookup::UsesGrungify(Material));
	const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Data->BaseColor.Expression);
	TestNotNull(TEXT("Base colour goes through the function"), Call);
	TestTrue(TEXT("Roughness goes through the function"), Data->Roughness.Expression == Call && !Data->Roughness.UseConstant);
	TestTrue(TEXT("Metallic goes through the function"), Data->Metallic.Expression == Call);
	TestEqual(TEXT("Adding twice is a no-op"), UGrungifyMaterialHookup::AddGrungifyToMaterial(Material, false).Outcome, FString(TEXT("already")));

	const FGrungifyHookupResult Removed = UGrungifyMaterialHookup::RemoveGrungifyFromMaterial(Material);
	TestEqual(TEXT("Removed"), Removed.Outcome, FString(TEXT("removed")));
	TestFalse(TEXT("Material no longer uses Grungify"), UGrungifyMaterialHookup::UsesGrungify(Material));
	TestTrue(TEXT("Base colour wiring and mask restored"), Data->BaseColor.Expression == Color && Data->BaseColor.Mask == 1
		&& Data->BaseColor.MaskR && Data->BaseColor.MaskG && Data->BaseColor.MaskB && !Data->BaseColor.MaskA);
	TestTrue(TEXT("Roughness constant restored"), Data->Roughness.UseConstant && !Data->Roughness.Expression && Data->Roughness.Constant == 0.37f);
	TestNull(TEXT("Metallic unconnected again"), Data->Metallic.Expression.Get());
	TestNull(TEXT("Normal unconnected again"), Data->Normal.Expression.Get());
	TestEqual(TEXT("No inserted nodes left behind"), Material->GetExpressions().Num(), OriginalCount);

	Material->MarkAsGarbage();
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}

#endif
