#include "GrungifyContentBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "GrungifySettings.h"
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "GrungifyContentBuilder"

FString FGrungifyContentReport::ToString() const
{
	TArray<FString> Lines;
	for (const FString& Item : Created)
	{
		Lines.Add(TEXT("Created  ") + Item);
	}
	for (const FString& Item : Rebuilt)
	{
		Lines.Add(TEXT("Rebuilt  ") + Item);
	}
	for (const FString& Item : Notes)
	{
		Lines.Add(TEXT("Note     ") + Item);
	}
	for (const FString& Item : Errors)
	{
		Lines.Add(TEXT("ERROR    ") + Item);
	}
	Lines.Add(bSaved ? TEXT("Saved    ok") : TEXT("ERROR    one or more packages failed to save"));
	return FString::Join(Lines, TEXT("\n"));
}

namespace
{
	const TCHAR* ZoneBlueprintPath = TEXT("/Grungify/BP_WeatheringZone");
	const TCHAR* FunctionPath = TEXT("/Grungify/Materials/MF_GrungifyWeathering");
	const TCHAR* DebugMaterialPath = TEXT("/Grungify/Materials/M_GrungifyMaskDebug");
	const TCHAR* ExampleMaterialPath = TEXT("/Grungify/Materials/M_GrungifyExample");
	const TCHAR* NoMaskTexturePath = TEXT("/Grungify/Materials/T_GrungifyNoMask");
	const TCHAR* IncludePath = TEXT("/Plugin/Grungify/Private/Grungify.ush");

	constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone | RF_Transactional;

	/** Changes whenever the generated graphs would change. */
	FString CurrentMarker()
	{
		const UGrungifySettings* Settings = UGrungifySettings::Get();
		return FString::Printf(TEXT("[GrungifyContent v%d cpd=%d uv=%d]"), Grungify::ContentVersion, Settings->CustomPrimitiveDataIndex, Settings->MaskUVChannel);
	}

	FString AssetName(const TCHAR* PackagePath)
	{
		return FPackageName::GetShortName(PackagePath);
	}

	template <typename T>
	T* FindExisting(const TCHAR* PackagePath)
	{
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), PackagePath, *AssetName(PackagePath));
		if (T* Loaded = FindObject<T>(nullptr, *ObjectPath))
		{
			return Loaded;
		}
		if (!FPackageName::DoesPackageExist(PackagePath))
		{
			return nullptr;
		}
		return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** Collects connection failures so a build can't silently produce a half-wired graph. */
	struct FBuildContext
	{
		FGrungifyContentReport& Report;
		FString Asset;

		void Connect(UMaterialExpression* From, const TCHAR* FromOutput, UMaterialExpression* To, const TCHAR* ToInput)
		{
			if (!From || !To || !UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput))
			{
				Report.Errors.Add(FString::Printf(TEXT("%s: could not connect %s.%s -> %s.%s"), *Asset,
					From ? *From->GetName() : TEXT("null"), FromOutput, To ? *To->GetName() : TEXT("null"), ToInput));
			}
		}

		void ConnectProperty(UMaterialExpression* From, const TCHAR* FromOutput, EMaterialProperty Property, const TCHAR* PropertyName)
		{
			if (!From || !UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOutput, Property))
			{
				Report.Errors.Add(FString::Printf(TEXT("%s: could not connect %s to %s"), *Asset, FromOutput, PropertyName));
			}
		}
	};

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

	void SetCustomInputs(UMaterialExpressionCustom* Custom, const TArray<const TCHAR*>& Names)
	{
		Custom->Inputs.Reset();
		for (const TCHAR* Name : Names)
		{
			FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
			Input.InputName = FName(Name);
		}
	}

	void SetMask(UMaterialExpressionComponentMask* Mask, bool bR, bool bG, bool bB, bool bA)
	{
		// The defaults are not all-off, so always set every channel.
		Mask->R = bR;
		Mask->G = bG;
		Mask->B = bB;
		Mask->A = bA;
	}

	int32 CustomDataIndex(int32 Offset)
	{
		return UGrungifySettings::Get()->CustomPrimitiveDataIndex + Offset;
	}

	/** Expressions shared by every custom node in the function. */
	struct FWeatheringSources
	{
		TMap<FString, TPair<UMaterialExpression*, FString>> ByInput; // custom input -> (expression, output name)

		void Add(const TCHAR* Input, UMaterialExpression* Expression, const TCHAR* Output = TEXT(""))
		{
			ByInput.Add(Input, TPair<UMaterialExpression*, FString>(Expression, Output));
		}
	};

	const TArray<const TCHAR*> WeatheringInputNames = {
		TEXT("BaseColor"), TEXT("Roughness"), TEXT("Metallic"), TEXT("NormalTS"),
		TEXT("MaskR"), TEXT("MaskG"), TEXT("MaskB"), TEXT("MaskA"),
		TEXT("TexR"), TEXT("TexG"), TEXT("TexB"), TEXT("TexA"), TEXT("MaskMode"),
		TEXT("WorldPos"), TEXT("WorldNormal"),
		TEXT("SurfaceType"), TEXT("SurfaceOverride"), TEXT("FeatureScale"), TEXT("SeedOffset"),
		TEXT("DustColor"), TEXT("RunoffColor"), TEXT("ChipColor"),
		TEXT("Strength"), TEXT("ChipStrength"), TEXT("DustStrength"), TEXT("RunoffStrength"), TEXT("HandlingStrength"),
		TEXT("Relief") };

	const TCHAR* CommonCode =
		TEXT("const float4 VM = float4(MaskR, MaskG, MaskB, MaskA);\n")
		TEXT("const float4 TM = float4(TexR, TexG, TexB, TexA);\n")
		TEXT("const float4 CS = float4(ChipStrength, DustStrength, RunoffStrength, HandlingStrength);\n");

	const TCHAR* EvaluateCode =
		TEXT("FGrungifyResult R = GrungifyEvaluate(BaseColor, Roughness, Metallic, MaskMode, VM, TM, WorldPos, WorldNormal, ")
		TEXT("SurfaceType, SurfaceOverride, FeatureScale, SeedOffset, DustColor.rgb, RunoffColor.rgb, ChipColor.rgb, Strength, CS);\n");

	UMaterialExpressionCustom* AddWeatheringCustom(UMaterialFunction* Function, FBuildContext& Ctx, const FWeatheringSources& Sources,
		const TCHAR* Description, ECustomMaterialOutputType OutputType, const FString& Body, int32 X, int32 Y)
	{
		UMaterialExpressionCustom* Custom = AddToFunction<UMaterialExpressionCustom>(Function, X, Y);
		Custom->Description = Description;
		Custom->OutputType = OutputType;
		Custom->IncludeFilePaths.Add(IncludePath);
		Custom->Code = FString(CommonCode) + Body;
		SetCustomInputs(Custom, WeatheringInputNames);

		for (const TCHAR* Input : WeatheringInputNames)
		{
			if (const TPair<UMaterialExpression*, FString>* Source = Sources.ByInput.Find(Input))
			{
				Ctx.Connect(Source->Key, *Source->Value, Custom, Input);
			}
			else
			{
				Ctx.Report.Errors.Add(FString::Printf(TEXT("%s: no source for custom input %s"), *Ctx.Asset, Input));
			}
		}
		return Custom;
	}

	UMaterialExpressionFunctionInput* AddFunctionInput(UMaterialFunction* Function, const TCHAR* Name, EFunctionInputType Type, int32 Sort, const FVector4f& Default, const TCHAR* Description, int32 Y)
	{
		UMaterialExpressionFunctionInput* Input = AddToFunction<UMaterialExpressionFunctionInput>(Function, -1400, Y);
		Input->InputName = FName(Name);
		Input->InputType = Type;
		Input->SortPriority = Sort;
		Input->Description = Description;
		Input->bUsePreviewValueAsDefault = true;
		Input->PreviewValue = Default;
		return Input;
	}

	UMaterialExpressionScalarParameter* AddScalar(UMaterialFunction* Function, const TCHAR* Name, float Default, int32 Y)
	{
		UMaterialExpressionScalarParameter* Param = AddToFunction<UMaterialExpressionScalarParameter>(Function, -1400, Y);
		Param->ParameterName = FName(Name);
		Param->Group = TEXT("Grungify");
		Param->DefaultValue = Default;
		return Param;
	}

	UMaterialExpressionScalarParameter* AddCustomDataParameter(UMaterialFunction* Function, const TCHAR* Name, int32 Offset, int32 Y)
	{
		UMaterialExpressionScalarParameter* Param = AddScalar(Function, Name, 0.f, Y);
		Param->bUseCustomPrimitiveData = true;
		Param->PrimitiveDataIndex = uint8(CustomDataIndex(Offset));
		return Param;
	}

	UMaterialExpressionVectorParameter* AddColor(UMaterialFunction* Function, const TCHAR* Name, const FLinearColor& Default, int32 Y)
	{
		UMaterialExpressionVectorParameter* Param = AddToFunction<UMaterialExpressionVectorParameter>(Function, -1400, Y);
		Param->ParameterName = FName(Name);
		Param->Group = TEXT("Grungify");
		Param->DefaultValue = Default;
		return Param;
	}

	void PopulateFunction(UMaterialFunction* Function, UTexture2D* NoMask, FGrungifyContentReport& Report)
	{
		FBuildContext Ctx{ Report, TEXT("MF_GrungifyWeathering") };

		Function->Description = FString::Printf(
			TEXT("Applies Grungify weathering (chips, dust, runoff, handling) baked by a Grungify Weathering Zone. ")
			TEXT("Meshes no zone has touched pass through unchanged. %s"), *CurrentMarker());
		Function->bExposeToLibrary = true;
		Function->LibraryCategoriesText.Reset();
		Function->LibraryCategoriesText.Add(LOCTEXT("LibraryCategory", "Grungify"));

		FWeatheringSources S;
		S.Add(TEXT("BaseColor"), AddFunctionInput(Function, TEXT("BaseColor"), FunctionInput_Vector3, 0, FVector4f(0.5f, 0.5f, 0.5f, 0.f), TEXT("Unweathered base colour."), 0));
		S.Add(TEXT("Roughness"), AddFunctionInput(Function, TEXT("Roughness"), FunctionInput_Scalar, 1, FVector4f(0.6f, 0.f, 0.f, 0.f), TEXT("Unweathered roughness."), 120));
		S.Add(TEXT("Metallic"), AddFunctionInput(Function, TEXT("Metallic"), FunctionInput_Scalar, 2, FVector4f(0.f, 0.f, 0.f, 0.f), TEXT("Unweathered metallic."), 240));
		S.Add(TEXT("NormalTS"), AddFunctionInput(Function, TEXT("Normal"), FunctionInput_Vector3, 3, FVector4f(0.f, 0.f, 1.f, 0.f),
			TEXT("Your tangent-space normal. Passed through unchanged unless Grungify Relief Strength is above 0."), 360));
		S.Add(TEXT("SurfaceOverride"), AddFunctionInput(Function, TEXT("SurfaceOverride"), FunctionInput_Scalar, 4, FVector4f(-1.f, 0.f, 0.f, 0.f),
			TEXT("-1 = use the type the zone detected for this mesh. 0 = stone, 1 = metal, 2 = trim (use on mixed meshes to set it per material)."), 480));

		UMaterialExpressionVertexColor* VertexColor = AddToFunction<UMaterialExpressionVertexColor>(Function, -1400, 600);
		S.Add(TEXT("MaskR"), VertexColor, TEXT("R"));
		S.Add(TEXT("MaskG"), VertexColor, TEXT("G"));
		S.Add(TEXT("MaskB"), VertexColor, TEXT("B"));
		S.Add(TEXT("MaskA"), VertexColor, TEXT("A"));

		UMaterialExpressionTextureCoordinate* MaskUV = AddToFunction<UMaterialExpressionTextureCoordinate>(Function, -1700, 760);
		MaskUV->CoordinateIndex = UGrungifySettings::Get()->MaskUVChannel;
		UMaterialExpressionTextureSampleParameter2D* MaskTexture = AddToFunction<UMaterialExpressionTextureSampleParameter2D>(Function, -1400, 760);
		MaskTexture->ParameterName = Grungify::MaskTextureParameterName();
		MaskTexture->Group = TEXT("Grungify");
		MaskTexture->Texture = NoMask;
		MaskTexture->SamplerType = SAMPLERTYPE_LinearColor;
		Ctx.Connect(MaskUV, TEXT(""), MaskTexture, TEXT("UVs"));
		S.Add(TEXT("TexR"), MaskTexture, TEXT("R"));
		S.Add(TEXT("TexG"), MaskTexture, TEXT("G"));
		S.Add(TEXT("TexB"), MaskTexture, TEXT("B"));
		S.Add(TEXT("TexA"), MaskTexture, TEXT("A"));

		S.Add(TEXT("WorldPos"), AddToFunction<UMaterialExpressionWorldPosition>(Function, -1400, 1000));
		S.Add(TEXT("WorldNormal"), AddToFunction<UMaterialExpressionVertexNormalWS>(Function, -1400, 1100));

		S.Add(TEXT("MaskMode"), AddCustomDataParameter(Function, TEXT("Grungify Mask Mode"), Grungify::CPD_MaskMode, 1200));
		S.Add(TEXT("SurfaceType"), AddCustomDataParameter(Function, TEXT("Grungify Surface Type"), Grungify::CPD_SurfaceType, 1300));
		S.Add(TEXT("FeatureScale"), AddCustomDataParameter(Function, TEXT("Grungify Feature Scale"), Grungify::CPD_FeatureScale, 1400));
		S.Add(TEXT("SeedOffset"), AddCustomDataParameter(Function, TEXT("Grungify Seed Offset"), Grungify::CPD_SeedOffset, 1500));

		S.Add(TEXT("DustColor"), AddColor(Function, TEXT("Grungify Dust Color"), FLinearColor(0.55f, 0.50f, 0.42f), 1600));
		S.Add(TEXT("RunoffColor"), AddColor(Function, TEXT("Grungify Runoff Color"), FLinearColor(0.16f, 0.11f, 0.07f), 1800));
		S.Add(TEXT("ChipColor"), AddColor(Function, TEXT("Grungify Trim Chip Color"), FLinearColor(0.45f, 0.42f, 0.38f), 2000));

		S.Add(TEXT("Strength"), AddScalar(Function, TEXT("Grungify Strength"), 1.f, 2200));
		S.Add(TEXT("ChipStrength"), AddScalar(Function, TEXT("Grungify Chip Strength"), 1.f, 2300));
		S.Add(TEXT("DustStrength"), AddScalar(Function, TEXT("Grungify Dust Strength"), 1.f, 2400));
		S.Add(TEXT("RunoffStrength"), AddScalar(Function, TEXT("Grungify Runoff Strength"), 1.f, 2500));
		S.Add(TEXT("HandlingStrength"), AddScalar(Function, TEXT("Grungify Handling Strength"), 1.f, 2600));
		S.Add(TEXT("Relief"), AddScalar(Function, TEXT("Grungify Relief Strength"), 0.f, 2700));

		UMaterialExpressionCustom* ColorNode = AddWeatheringCustom(Function, Ctx, S, TEXT("Grungify Base Color"), CMOT_Float3,
			FString(EvaluateCode) + TEXT("return R.BaseColor;"), -600, 0);
		UMaterialExpressionCustom* SurfaceNode = AddWeatheringCustom(Function, Ctx, S, TEXT("Grungify Roughness Metallic"), CMOT_Float2,
			FString(EvaluateCode) + TEXT("return float2(R.Roughness, R.Metallic);"), -600, 500);
		UMaterialExpressionCustom* NormalNode = AddWeatheringCustom(Function, Ctx, S, TEXT("Grungify Relief Normal"), CMOT_Float3,
			TEXT("return GrungifyReliefNormal(NormalTS, Relief, MaskMode, VM, TM, WorldPos, WorldNormal, ")
			TEXT("Parameters.TangentToWorld[0], Parameters.TangentToWorld[1], FeatureScale, SeedOffset, Strength, CS);"), -600, 1000);

		UMaterialExpressionComponentMask* RoughnessMask = AddToFunction<UMaterialExpressionComponentMask>(Function, -200, 450);
		SetMask(RoughnessMask, true, false, false, false);
		UMaterialExpressionComponentMask* MetallicMask = AddToFunction<UMaterialExpressionComponentMask>(Function, -200, 600);
		SetMask(MetallicMask, false, true, false, false);
		Ctx.Connect(SurfaceNode, TEXT(""), RoughnessMask, TEXT(""));
		Ctx.Connect(SurfaceNode, TEXT(""), MetallicMask, TEXT(""));

		auto AddOutput = [Function](const TCHAR* Name, int32 Sort, int32 Y)
		{
			UMaterialExpressionFunctionOutput* Output = AddToFunction<UMaterialExpressionFunctionOutput>(Function, 200, Y);
			Output->OutputName = FName(Name);
			Output->SortPriority = Sort;
			return Output;
		};
		Ctx.Connect(ColorNode, TEXT(""), AddOutput(TEXT("BaseColor"), 0, 0), TEXT(""));
		Ctx.Connect(RoughnessMask, TEXT(""), AddOutput(TEXT("Roughness"), 1, 450), TEXT(""));
		Ctx.Connect(MetallicMask, TEXT(""), AddOutput(TEXT("Metallic"), 2, 600), TEXT(""));
		Ctx.Connect(NormalNode, TEXT(""), AddOutput(TEXT("Normal"), 3, 1000), TEXT(""));

		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
	}

	void PopulateDebugMaterial(UMaterial* Material, UTexture2D* NoMask, FGrungifyContentReport& Report)
	{
		FBuildContext Ctx{ Report, TEXT("M_GrungifyMaskDebug") };
		Material->SetShadingModel(MSM_Unlit);

		UMaterialExpressionVertexColor* VertexColor = AddToMaterial<UMaterialExpressionVertexColor>(Material, -900, 0);

		UMaterialExpressionTextureCoordinate* MaskUV = AddToMaterial<UMaterialExpressionTextureCoordinate>(Material, -1200, 250);
		MaskUV->CoordinateIndex = UGrungifySettings::Get()->MaskUVChannel;
		UMaterialExpressionTextureSampleParameter2D* MaskTexture = AddToMaterial<UMaterialExpressionTextureSampleParameter2D>(Material, -900, 250);
		MaskTexture->ParameterName = Grungify::MaskTextureParameterName();
		MaskTexture->Texture = NoMask;
		MaskTexture->SamplerType = SAMPLERTYPE_LinearColor;
		Ctx.Connect(MaskUV, TEXT(""), MaskTexture, TEXT("UVs"));

		auto AddCustomData = [Material](const TCHAR* Name, int32 Offset, int32 Y)
		{
			UMaterialExpressionScalarParameter* Param = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -900, Y);
			Param->ParameterName = FName(Name);
			Param->bUseCustomPrimitiveData = true;
			Param->PrimitiveDataIndex = uint8(CustomDataIndex(Offset));
			return Param;
		};
		UMaterialExpressionScalarParameter* Mode = AddCustomData(TEXT("Grungify Mask Mode"), Grungify::CPD_MaskMode, 550);
		UMaterialExpressionScalarParameter* Channel = AddCustomData(TEXT("Grungify Debug Channel"), Grungify::CPD_DebugChannel, 650);

		UMaterialExpressionCustom* Custom = AddToMaterial<UMaterialExpressionCustom>(Material, -400, 0);
		Custom->Description = TEXT("Grungify Mask Debug");
		Custom->OutputType = CMOT_Float3;
		Custom->IncludeFilePaths.Add(IncludePath);
		Custom->Code = TEXT("return GrungifyMaskDebug(MaskMode, float4(MaskR, MaskG, MaskB, MaskA), float4(TexR, TexG, TexB, TexA), Channel);");
		SetCustomInputs(Custom, { TEXT("MaskR"), TEXT("MaskG"), TEXT("MaskB"), TEXT("MaskA"), TEXT("TexR"), TEXT("TexG"), TEXT("TexB"), TEXT("TexA"), TEXT("MaskMode"), TEXT("Channel") });

		Ctx.Connect(VertexColor, TEXT("R"), Custom, TEXT("MaskR"));
		Ctx.Connect(VertexColor, TEXT("G"), Custom, TEXT("MaskG"));
		Ctx.Connect(VertexColor, TEXT("B"), Custom, TEXT("MaskB"));
		Ctx.Connect(VertexColor, TEXT("A"), Custom, TEXT("MaskA"));
		Ctx.Connect(MaskTexture, TEXT("R"), Custom, TEXT("TexR"));
		Ctx.Connect(MaskTexture, TEXT("G"), Custom, TEXT("TexG"));
		Ctx.Connect(MaskTexture, TEXT("B"), Custom, TEXT("TexB"));
		Ctx.Connect(MaskTexture, TEXT("A"), Custom, TEXT("TexA"));
		Ctx.Connect(Mode, TEXT(""), Custom, TEXT("MaskMode"));
		Ctx.Connect(Channel, TEXT(""), Custom, TEXT("Channel"));

		Ctx.ConnectProperty(Custom, TEXT(""), MP_EmissiveColor, TEXT("Emissive Color"));
	}

	void PopulateExampleMaterial(UMaterial* Material, UMaterialFunction* Function, FGrungifyContentReport& Report)
	{
		FBuildContext Ctx{ Report, TEXT("M_GrungifyExample") };

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
		if (!Call->SetMaterialFunction(Function))
		{
			Report.Errors.Add(TEXT("M_GrungifyExample: could not set MF_GrungifyWeathering on the function call"));
		}

		Ctx.Connect(BaseColor, TEXT(""), Call, TEXT("BaseColor"));
		Ctx.Connect(Roughness, TEXT(""), Call, TEXT("Roughness"));
		Ctx.Connect(Metallic, TEXT(""), Call, TEXT("Metallic"));
		Ctx.Connect(SurfaceOverride, TEXT(""), Call, TEXT("SurfaceOverride"));

		Ctx.ConnectProperty(Call, TEXT("BaseColor"), MP_BaseColor, TEXT("Base Color"));
		Ctx.ConnectProperty(Call, TEXT("Roughness"), MP_Roughness, TEXT("Roughness"));
		Ctx.ConnectProperty(Call, TEXT("Metallic"), MP_Metallic, TEXT("Metallic"));
		Ctx.ConnectProperty(Call, TEXT("Normal"), MP_Normal, TEXT("Normal"));
	}

	void CompileAndCheck(UMaterial* Material, FGrungifyContentReport& Report)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
		if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel))
		{
			Resource->FinishCompilation();
			for (const FString& Error : Resource->GetCompileErrors())
			{
				Report.Errors.Add(FString::Printf(TEXT("%s: %s"), *Material->GetName(), *Error));
			}
		}
		else
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: no material resource to check compile errors on"), *Material->GetName()));
		}
	}

	UTexture2D* BuildNoMaskTexture(UPackage* Package)
	{
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName(NoMaskTexturePath), AssetFlags);
		TArray<FColor> Pixels;
		Pixels.Init(FColor(0, 0, 0, 0), 16);
		Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		// Must match the baked mask textures so the Linear Color sampler accepts both.
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Default;
		Texture->PostEditChange();
		return Texture;
	}

	void Register(UObject* Asset, TArray<UPackage*>& Packages)
	{
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		Packages.AddUnique(Asset->GetPackage());
	}

	void Touch(UObject* Asset, TArray<UPackage*>& Packages)
	{
		Asset->MarkPackageDirty();
		Packages.AddUnique(Asset->GetPackage());
	}
}

bool FGrungifyContentBuilder::NeedsWork()
{
	const UMaterialFunction* Function = FindExisting<UMaterialFunction>(FunctionPath);
	return !Function || !Function->Description.Contains(CurrentMarker())
		|| !FPackageName::DoesPackageExist(ZoneBlueprintPath)
		|| !FPackageName::DoesPackageExist(DebugMaterialPath)
		|| !FPackageName::DoesPackageExist(ExampleMaterialPath)
		|| !FPackageName::DoesPackageExist(NoMaskTexturePath);
}

FGrungifyContentReport FGrungifyContentBuilder::Run(bool bForceRebuild)
{
	FGrungifyContentReport Report;
	TArray<UPackage*> Packages;

	// The custom nodes include this file; nothing compiles without it.
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Grungify")))
	{
		const FString Shader = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"), TEXT("Private"), TEXT("Grungify.ush"));
		if (!FPaths::FileExists(Shader))
		{
			Report.Errors.Add(FString::Printf(TEXT("Shader include missing: %s"), *Shader));
		}
	}

	UTexture2D* NoMask = FindExisting<UTexture2D>(NoMaskTexturePath);
	if (!NoMask)
	{
		NoMask = BuildNoMaskTexture(CreatePackage(NoMaskTexturePath));
		Register(NoMask, Packages);
		Report.Created.Add(AssetName(NoMaskTexturePath));
	}

	// The three generated material assets are rebuilt together whenever the function's marker is stale.
	UMaterialFunction* Function = FindExisting<UMaterialFunction>(FunctionPath);
	const bool bRebuild = bForceRebuild || !Function || !Function->Description.Contains(CurrentMarker());

	if (!Function)
	{
		Function = NewObject<UMaterialFunction>(CreatePackage(FunctionPath), *AssetName(FunctionPath), AssetFlags);
		Register(Function, Packages);
		Report.Created.Add(AssetName(FunctionPath));
	}
	else if (bRebuild)
	{
		Function->Modify();
		UMaterialEditingLibrary::DeleteAllMaterialExpressionsInFunction(Function);
		Touch(Function, Packages);
		Report.Rebuilt.Add(AssetName(FunctionPath));
	}
	if (bRebuild)
	{
		PopulateFunction(Function, NoMask, Report);
	}

	auto PrepareMaterial = [&](const TCHAR* Path) -> UMaterial*
	{
		UMaterial* Material = FindExisting<UMaterial>(Path);
		if (!Material)
		{
			Material = NewObject<UMaterial>(CreatePackage(Path), *AssetName(Path), AssetFlags);
			Register(Material, Packages);
			Report.Created.Add(AssetName(Path));
			return Material;
		}
		if (!bRebuild)
		{
			return nullptr;
		}
		Material->Modify();
		UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
		Touch(Material, Packages);
		Report.Rebuilt.Add(AssetName(Path));
		return Material;
	};

	if (UMaterial* Debug = PrepareMaterial(DebugMaterialPath))
	{
		PopulateDebugMaterial(Debug, NoMask, Report);
		CompileAndCheck(Debug, Report);
	}
	if (UMaterial* Example = PrepareMaterial(ExampleMaterialPath))
	{
		PopulateExampleMaterial(Example, Function, Report);
		CompileAndCheck(Example, Report);
	}

	if (UBlueprint* Existing = FindExisting<UBlueprint>(ZoneBlueprintPath))
	{
		if (!Existing->ParentClass || !Existing->ParentClass->IsChildOf(AGrungifyWeatheringZone::StaticClass()))
		{
			Report.Errors.Add(TEXT("BP_WeatheringZone exists but does not derive from Grungify Weathering Zone"));
		}
	}
	else
	{
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AGrungifyWeatheringZone::StaticClass(), CreatePackage(ZoneBlueprintPath), *AssetName(ZoneBlueprintPath), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("GrungifyContentBuilder"));
		if (Blueprint)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			Register(Blueprint, Packages);
			Report.Created.Add(AssetName(ZoneBlueprintPath));
		}
		else
		{
			Report.Errors.Add(TEXT("Could not create BP_WeatheringZone"));
		}
	}

	if (Packages.Num() > 0)
	{
		Report.bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false);
	}

	const FString Text = Report.ToString();
	FFileHelper::SaveStringToFile(Text, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"), TEXT("ContentReport.txt")));
	if (Report.HasErrors())
	{
		UE_LOG(LogGrungify, Error, TEXT("Grungify content build had problems:\n%s"), *Text);
	}
	else if (Packages.Num() > 0)
	{
		UE_LOG(LogGrungify, Log, TEXT("Grungify content build:\n%s"), *Text);
	}
	return Report;
}

#undef LOCTEXT_NAMESPACE
