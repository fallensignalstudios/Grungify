#include "GrungifyContentBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "FileHelpers.h"
#include "GrungifySettings.h"
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "GrungifyContentBuilder"

namespace
{
	const TCHAR* ZoneBlueprintPath = TEXT("/Grungify/BP_WeatheringZone");
	const TCHAR* FunctionPath = TEXT("/Grungify/Materials/MF_GrungifyWeathering");
	const TCHAR* DebugMaterialPath = TEXT("/Grungify/Materials/M_GrungifyMaskDebug");
	const TCHAR* ExampleMaterialPath = TEXT("/Grungify/Materials/M_GrungifyExample");
	const TCHAR* IncludePath = TEXT("/Plugin/Grungify/Private/Grungify.ush");

	constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone | RF_Transactional;

	bool AssetExists(const TCHAR* PackagePath)
	{
		return FindPackage(nullptr, PackagePath) != nullptr || FPackageName::DoesPackageExist(PackagePath);
	}

	template <typename T>
	T* AddToFunction(UMaterialFunction* Function, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, T::StaticClass(), X, Y));
	}

	template <typename T>
	T* AddToMaterial(UMaterial* Material, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
	}

	void Connect(UMaterialExpression* From, const TCHAR* FromOutput, UMaterialExpression* To, const TCHAR* ToInput)
	{
		if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput))
		{
			UE_LOG(LogGrungify, Warning, TEXT("Could not connect %s.%s -> %s.%s"),
				From ? *From->GetName() : TEXT("null"), FromOutput, To ? *To->GetName() : TEXT("null"), ToInput);
		}
	}

	void SetCustomInputs(UMaterialExpressionCustom* Custom, const TArray<const TCHAR*>& Names)
	{
		Custom->Inputs.Reset();
		for (const TCHAR* Name : Names)
		{
			FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
			Input.InputName = FName(Name);
		}
	}

	/** Shared inputs for the weathering custom nodes (everything except the base material values). */
	struct FWeatheringSources
	{
		UMaterialExpression* BaseColor = nullptr;
		UMaterialExpression* Roughness = nullptr;
		UMaterialExpression* Metallic = nullptr;
		UMaterialExpression* SurfaceOverride = nullptr;
		UMaterialExpressionVertexColor* VertexColor = nullptr;
		UMaterialExpression* WorldPos = nullptr;
		UMaterialExpression* WorldNormal = nullptr;
		UMaterialExpression* SurfaceType = nullptr;
		UMaterialExpression* FeatureScale = nullptr;
		UMaterialExpression* SeedOffset = nullptr;
		UMaterialExpression* DustColor = nullptr;
		UMaterialExpression* RunoffColor = nullptr;
		UMaterialExpression* ChipColor = nullptr;
		UMaterialExpression* Strength = nullptr;
	};

	const TArray<const TCHAR*> WeatheringInputNames = {
		TEXT("BaseColor"), TEXT("Roughness"), TEXT("Metallic"),
		TEXT("MaskR"), TEXT("MaskG"), TEXT("MaskB"), TEXT("MaskA"),
		TEXT("WorldPos"), TEXT("WorldNormal"),
		TEXT("SurfaceType"), TEXT("SurfaceOverride"), TEXT("FeatureScale"), TEXT("SeedOffset"),
		TEXT("DustColor"), TEXT("RunoffColor"), TEXT("ChipColor"), TEXT("Strength") };

	const TCHAR* EvaluateCall =
		TEXT("FGrungifyResult R = GrungifyEvaluate(BaseColor, Roughness, Metallic, float4(MaskR, MaskG, MaskB, MaskA), ")
		TEXT("WorldPos, WorldNormal, SurfaceType, SurfaceOverride, FeatureScale, SeedOffset, ")
		TEXT("DustColor.rgb, RunoffColor.rgb, ChipColor.rgb, Strength);\n");

	UMaterialExpressionCustom* AddWeatheringCustom(UMaterialFunction* Function, const FWeatheringSources& S, const TCHAR* Description, ECustomMaterialOutputType OutputType, const FString& ReturnLine, int32 X, int32 Y)
	{
		UMaterialExpressionCustom* Custom = AddToFunction<UMaterialExpressionCustom>(Function, X, Y);
		Custom->Description = Description;
		Custom->OutputType = OutputType;
		Custom->IncludeFilePaths.Add(IncludePath);
		Custom->Code = FString(EvaluateCall) + ReturnLine;
		SetCustomInputs(Custom, WeatheringInputNames);

		Connect(S.BaseColor, TEXT(""), Custom, TEXT("BaseColor"));
		Connect(S.Roughness, TEXT(""), Custom, TEXT("Roughness"));
		Connect(S.Metallic, TEXT(""), Custom, TEXT("Metallic"));
		Connect(S.VertexColor, TEXT("R"), Custom, TEXT("MaskR"));
		Connect(S.VertexColor, TEXT("G"), Custom, TEXT("MaskG"));
		Connect(S.VertexColor, TEXT("B"), Custom, TEXT("MaskB"));
		Connect(S.VertexColor, TEXT("A"), Custom, TEXT("MaskA"));
		Connect(S.WorldPos, TEXT(""), Custom, TEXT("WorldPos"));
		Connect(S.WorldNormal, TEXT(""), Custom, TEXT("WorldNormal"));
		Connect(S.SurfaceType, TEXT(""), Custom, TEXT("SurfaceType"));
		Connect(S.SurfaceOverride, TEXT(""), Custom, TEXT("SurfaceOverride"));
		Connect(S.FeatureScale, TEXT(""), Custom, TEXT("FeatureScale"));
		Connect(S.SeedOffset, TEXT(""), Custom, TEXT("SeedOffset"));
		Connect(S.DustColor, TEXT(""), Custom, TEXT("DustColor"));
		Connect(S.RunoffColor, TEXT(""), Custom, TEXT("RunoffColor"));
		Connect(S.ChipColor, TEXT(""), Custom, TEXT("ChipColor"));
		Connect(S.Strength, TEXT(""), Custom, TEXT("Strength"));
		return Custom;
	}

	UMaterialExpressionFunctionInput* AddFunctionInput(UMaterialFunction* Function, const TCHAR* Name, EFunctionInputType Type, int32 Sort, const FVector4f& Default, const TCHAR* Description, int32 Y)
	{
		UMaterialExpressionFunctionInput* Input = AddToFunction<UMaterialExpressionFunctionInput>(Function, -1200, Y);
		Input->InputName = FName(Name);
		Input->InputType = Type;
		Input->SortPriority = Sort;
		Input->Description = Description;
		Input->bUsePreviewValueAsDefault = true;
		Input->PreviewValue = Default;
		return Input;
	}

	UMaterialExpressionScalarParameter* AddCustomDataParameter(UMaterialFunction* Function, const TCHAR* Name, int32 Offset, int32 Y)
	{
		UMaterialExpressionScalarParameter* Param = AddToFunction<UMaterialExpressionScalarParameter>(Function, -1200, Y);
		Param->ParameterName = FName(Name);
		Param->Group = TEXT("Grungify");
		Param->DefaultValue = 0.f;
		Param->bUseCustomPrimitiveData = true;
		Param->PrimitiveDataIndex = uint8(UGrungifySettings::Get()->CustomPrimitiveDataIndex + Offset);
		return Param;
	}

	UMaterialExpressionVectorParameter* AddColorParameter(UMaterialFunction* Function, const TCHAR* Name, const FLinearColor& Default, int32 Y)
	{
		UMaterialExpressionVectorParameter* Param = AddToFunction<UMaterialExpressionVectorParameter>(Function, -1200, Y);
		Param->ParameterName = FName(Name);
		Param->Group = TEXT("Grungify");
		Param->DefaultValue = Default;
		return Param;
	}

	UMaterialFunction* BuildWeatheringFunction(UPackage* Package)
	{
		UMaterialFunction* Function = NewObject<UMaterialFunction>(Package, TEXT("MF_GrungifyWeathering"), AssetFlags);
		Function->Description = TEXT("Applies Grungify weathering (chips, dust, runoff, handling) baked by a Grungify Weathering Zone. Put it between your base values and the material outputs.");
		Function->bExposeToLibrary = true;
		Function->LibraryCategoriesText.Add(LOCTEXT("LibraryCategory", "Grungify"));

		FWeatheringSources S;
		S.BaseColor = AddFunctionInput(Function, TEXT("BaseColor"), FunctionInput_Vector3, 0, FVector4f(0.5f, 0.5f, 0.5f, 0.f), TEXT("Unweathered base colour."), 0);
		S.Roughness = AddFunctionInput(Function, TEXT("Roughness"), FunctionInput_Scalar, 1, FVector4f(0.6f, 0.f, 0.f, 0.f), TEXT("Unweathered roughness."), 150);
		S.Metallic = AddFunctionInput(Function, TEXT("Metallic"), FunctionInput_Scalar, 2, FVector4f(0.f, 0.f, 0.f, 0.f), TEXT("Unweathered metallic."), 300);
		S.SurfaceOverride = AddFunctionInput(Function, TEXT("SurfaceOverride"), FunctionInput_Scalar, 3, FVector4f(-1.f, 0.f, 0.f, 0.f),
			TEXT("-1 = use the type the zone detected for this mesh. 0 = stone, 1 = metal, 2 = trim (use on mixed meshes to set it per material)."), 450);

		S.VertexColor = AddToFunction<UMaterialExpressionVertexColor>(Function, -1200, 600);
		S.WorldPos = AddToFunction<UMaterialExpressionWorldPosition>(Function, -1200, 800);
		S.WorldNormal = AddToFunction<UMaterialExpressionVertexNormalWS>(Function, -1200, 900);

		S.SurfaceType = AddCustomDataParameter(Function, TEXT("Grungify Surface Type"), Grungify::CPD_SurfaceType, 1000);
		S.FeatureScale = AddCustomDataParameter(Function, TEXT("Grungify Feature Scale"), Grungify::CPD_FeatureScale, 1100);
		S.SeedOffset = AddCustomDataParameter(Function, TEXT("Grungify Seed Offset"), Grungify::CPD_SeedOffset, 1200);

		S.DustColor = AddColorParameter(Function, TEXT("Grungify Dust Color"), FLinearColor(0.55f, 0.50f, 0.42f), 1300);
		S.RunoffColor = AddColorParameter(Function, TEXT("Grungify Runoff Color"), FLinearColor(0.16f, 0.11f, 0.07f), 1500);
		S.ChipColor = AddColorParameter(Function, TEXT("Grungify Trim Chip Color"), FLinearColor(0.45f, 0.42f, 0.38f), 1700);

		UMaterialExpressionScalarParameter* Strength = AddToFunction<UMaterialExpressionScalarParameter>(Function, -1200, 1900);
		Strength->ParameterName = TEXT("Grungify Strength");
		Strength->Group = TEXT("Grungify");
		Strength->DefaultValue = 1.f;
		S.Strength = Strength;

		UMaterialExpressionCustom* ColorNode = AddWeatheringCustom(Function, S, TEXT("Grungify Base Color"), CMOT_Float3, TEXT("return R.BaseColor;"), -500, 0);
		UMaterialExpressionCustom* SurfaceNode = AddWeatheringCustom(Function, S, TEXT("Grungify Roughness Metallic"), CMOT_Float2, TEXT("return float2(R.Roughness, R.Metallic);"), -500, 500);

		UMaterialExpressionComponentMask* RoughnessMask = AddToFunction<UMaterialExpressionComponentMask>(Function, -150, 450);
		RoughnessMask->R = true;
		UMaterialExpressionComponentMask* MetallicMask = AddToFunction<UMaterialExpressionComponentMask>(Function, -150, 600);
		MetallicMask->G = true;
		Connect(SurfaceNode, TEXT(""), RoughnessMask, TEXT(""));
		Connect(SurfaceNode, TEXT(""), MetallicMask, TEXT(""));

		auto AddOutput = [Function](const TCHAR* Name, int32 Sort, int32 Y)
		{
			UMaterialExpressionFunctionOutput* Output = AddToFunction<UMaterialExpressionFunctionOutput>(Function, 200, Y);
			Output->OutputName = FName(Name);
			Output->SortPriority = Sort;
			return Output;
		};
		Connect(ColorNode, TEXT(""), AddOutput(TEXT("BaseColor"), 0, 0), TEXT(""));
		Connect(RoughnessMask, TEXT(""), AddOutput(TEXT("Roughness"), 1, 450), TEXT(""));
		Connect(MetallicMask, TEXT(""), AddOutput(TEXT("Metallic"), 2, 600), TEXT(""));

		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
		return Function;
	}

	UMaterial* BuildDebugMaterial(UPackage* Package)
	{
		UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_GrungifyMaskDebug"), AssetFlags);
		Material->SetShadingModel(MSM_Unlit);

		UMaterialExpressionVertexColor* VertexColor = AddToMaterial<UMaterialExpressionVertexColor>(Material, -800, 0);
		UMaterialExpressionScalarParameter* Channel = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -800, 250);
		Channel->ParameterName = TEXT("Grungify Debug Channel");
		Channel->bUseCustomPrimitiveData = true;
		Channel->PrimitiveDataIndex = uint8(UGrungifySettings::Get()->CustomPrimitiveDataIndex + Grungify::CPD_DebugChannel);

		UMaterialExpressionCustom* Custom = AddToMaterial<UMaterialExpressionCustom>(Material, -400, 0);
		Custom->Description = TEXT("Grungify Mask Debug");
		Custom->OutputType = CMOT_Float3;
		Custom->IncludeFilePaths.Add(IncludePath);
		Custom->Code = TEXT("return GrungifyMaskDebug(float4(MaskR, MaskG, MaskB, MaskA), Channel);");
		SetCustomInputs(Custom, { TEXT("MaskR"), TEXT("MaskG"), TEXT("MaskB"), TEXT("MaskA"), TEXT("Channel") });

		Connect(VertexColor, TEXT("R"), Custom, TEXT("MaskR"));
		Connect(VertexColor, TEXT("G"), Custom, TEXT("MaskG"));
		Connect(VertexColor, TEXT("B"), Custom, TEXT("MaskB"));
		Connect(VertexColor, TEXT("A"), Custom, TEXT("MaskA"));
		Connect(Channel, TEXT(""), Custom, TEXT("Channel"));

		UMaterialEditingLibrary::ConnectMaterialProperty(Custom, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
	}

	UMaterial* BuildExampleMaterial(UPackage* Package, UMaterialFunction* Function)
	{
		UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_GrungifyExample"), AssetFlags);

		UMaterialExpressionVectorParameter* BaseColor = AddToMaterial<UMaterialExpressionVectorParameter>(Material, -800, 0);
		BaseColor->ParameterName = TEXT("Base Color");
		BaseColor->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f);

		UMaterialExpressionScalarParameter* Roughness = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -800, 200);
		Roughness->ParameterName = TEXT("Roughness");
		Roughness->DefaultValue = 0.6f;

		UMaterialExpressionScalarParameter* Metallic = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -800, 300);
		Metallic->ParameterName = TEXT("Metallic");
		Metallic->DefaultValue = 0.f;

		UMaterialExpressionScalarParameter* SurfaceOverride = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -800, 400);
		SurfaceOverride->ParameterName = TEXT("Surface Override");
		SurfaceOverride->DefaultValue = -1.f;

		UMaterialExpressionMaterialFunctionCall* Call = AddToMaterial<UMaterialExpressionMaterialFunctionCall>(Material, -400, 0);
		Call->SetMaterialFunction(Function);

		Connect(BaseColor, TEXT(""), Call, TEXT("BaseColor"));
		Connect(Roughness, TEXT(""), Call, TEXT("Roughness"));
		Connect(Metallic, TEXT(""), Call, TEXT("Metallic"));
		Connect(SurfaceOverride, TEXT(""), Call, TEXT("SurfaceOverride"));

		UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("BaseColor"), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Roughness"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Metallic"), MP_Metallic);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
	}

	UBlueprint* BuildZoneBlueprint(UPackage* Package)
	{
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AGrungifyWeatheringZone::StaticClass(), Package, TEXT("BP_WeatheringZone"), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("GrungifyContentBuilder"));
		if (Blueprint)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
		return Blueprint;
	}

	void Register(UObject* Asset, TArray<UPackage*>& OutPackages)
	{
		if (Asset)
		{
			FAssetRegistryModule::AssetCreated(Asset);
			Asset->MarkPackageDirty();
			OutPackages.Add(Asset->GetPackage());
		}
	}
}

int32 FGrungifyContentBuilder::BuildMissingContent()
{
	TArray<UPackage*> Created;

	UMaterialFunction* Function = nullptr;
	if (!AssetExists(FunctionPath))
	{
		Function = BuildWeatheringFunction(CreatePackage(FunctionPath));
		Register(Function, Created);
	}
	else
	{
		Function = LoadObject<UMaterialFunction>(nullptr, *FString::Printf(TEXT("%s.MF_GrungifyWeathering"), FunctionPath));
	}

	if (!AssetExists(DebugMaterialPath))
	{
		Register(BuildDebugMaterial(CreatePackage(DebugMaterialPath)), Created);
	}

	if (!AssetExists(ExampleMaterialPath) && Function)
	{
		Register(BuildExampleMaterial(CreatePackage(ExampleMaterialPath), Function), Created);
	}

	if (!AssetExists(ZoneBlueprintPath))
	{
		Register(BuildZoneBlueprint(CreatePackage(ZoneBlueprintPath)), Created);
	}

	if (Created.Num() > 0)
	{
		UEditorLoadingAndSavingUtils::SavePackages(Created, /*bOnlyDirty*/ false);
		UE_LOG(LogGrungify, Log, TEXT("Grungify: created %d asset(s) under /Grungify."), Created.Num());
	}
	return Created.Num();
}

bool FGrungifyContentBuilder::IsContentMissing()
{
	return !AssetExists(ZoneBlueprintPath) || !AssetExists(FunctionPath) || !AssetExists(DebugMaterialPath) || !AssetExists(ExampleMaterialPath);
}

#undef LOCTEXT_NAMESPACE
