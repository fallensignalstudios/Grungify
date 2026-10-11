// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyContentBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "GrungifySettings.h"
#include "GrungifyCompat.h"
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubstrate.h"
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
#include "Misc/SecureHash.h"
#include "RHIGlobals.h"
#include "RenderUtils.h"
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
	Lines.Add(bSaveAttempted
		? (bSaved ? TEXT("Saved    ok") : TEXT("ERROR    one or more packages failed to save"))
		: TEXT("Saved    not attempted"));
	Lines.Add(bReady ? TEXT("Ready    validated saved content") : TEXT("Ready    false; generated content requires repair"));
	return FString::Join(Lines, TEXT("\n"));
}

namespace
{
	const TCHAR* ZoneBlueprintPath = TEXT("/Grungify/BP_WeatheringZone");
	const TCHAR* FunctionPath = TEXT("/Grungify/Materials/MF_GrungifyWeathering");
	const TCHAR* SubstrateFunctionPath = TEXT("/Grungify/Materials/MF_GrungifySubstrate");
	const TCHAR* LandscapeFunctionPath = TEXT("/Grungify/Materials/MF_GrungifyLandscape");
	const TCHAR* DisplacementFunctionPath = TEXT("/Grungify/Materials/MF_GrungifyDisplacement");
	const TCHAR* DebugMaterialPath = TEXT("/Grungify/Materials/M_GrungifyMaskDebug");
	const TCHAR* ExampleMaterialPath = TEXT("/Grungify/Materials/M_GrungifyExample");
	const TCHAR* NoMaskTexturePath = TEXT("/Grungify/Materials/T_GrungifyNoMask");
	const TCHAR* IncludePath = TEXT("/Plugin/Grungify/Private/Grungify.ush");

	constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone | RF_Transactional;

	/** Changes whenever the generated graphs would change. */
	FString CurrentMarker()
	{
		const UGrungifySettings* Settings = UGrungifySettings::Get();
		return FString::Printf(TEXT("[GrungifyContent v%d cpd=%d uv=%d inst=%d]"), Grungify::ContentVersion, Settings->CustomPrimitiveDataIndex,
			Settings->MaskUVChannel, Settings->InstanceCustomDataIndex);
	}

	FString PendingDescription()
	{
		return TEXT("Applies Grungify weathering (chips, dust, runoff, handling) baked by a Grungify Weathering Zone. ")
			TEXT("Inactive owned-material gate passes artist inputs through unchanged. [GrungifyContent pending validation]");
	}

	FString ReadinessPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"), TEXT("ContentReady.txt"));
	}

	FString ShaderPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Grungify"));
		return Plugin ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"), TEXT("Private"), TEXT("Grungify.ush")) : FString();
	}

	// The label alone is not readiness: a partial SavePackages failure may have written some packages.
	// Authenticate only the shader and complete generated package set from a successful build.
	bool MakeReadinessToken(FString& Token)
	{
		Token = CurrentMarker();
		TArray<FString> Files{ ShaderPath() };
		for (const TCHAR* Path : { ZoneBlueprintPath, FunctionPath, SubstrateFunctionPath, LandscapeFunctionPath, DisplacementFunctionPath, DebugMaterialPath, ExampleMaterialPath, NoMaskTexturePath })
		{
			Files.Add(FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension()));
		}
		for (const FString& File : Files)
		{
			TArray<uint8> Bytes;
			if (File.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.IsEmpty())
			{
				return false;
			}
			Token += TEXT("\n") + File + TEXT(" ") + FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num()).ToString();
		}
		return true;
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

	// UE 5.8's DeleteAll helpers iterate the live collection while each deletion compacts it.
	// Iterate a stable snapshot instead, otherwise old parameters/outputs can survive a rebuild.
	bool ClearFunctionExpressions(UMaterialFunction* Function, FGrungifyContentReport& Report)
	{
		TArray<UMaterialExpression*> Expressions;
		for (UMaterialExpression* Expression : Function->GetExpressions()) { Expressions.Add(Expression); }
		for (UMaterialExpression* Expression : Expressions)
		{
			UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Expression);
		}
		if (Function->GetExpressions().Num() != 0)
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: expression deletion was incomplete; graph was not repopulated"), *Function->GetName()));
			return false;
		}
		return true;
	}

	bool ClearMaterialExpressions(UMaterial* Material, FGrungifyContentReport& Report)
	{
		TArray<UMaterialExpression*> Expressions;
		for (UMaterialExpression* Expression : Material->GetExpressions()) { Expressions.Add(Expression); }
		for (UMaterialExpression* Expression : Expressions)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
		}
		if (Material->GetExpressions().Num() != 0)
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: expression deletion was incomplete; graph was not repopulated"), *Material->GetName()));
			return false;
		}
		return true;
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
		TEXT("Relief"),
		TEXT("InstR"), TEXT("InstG"), TEXT("InstB"), TEXT("InstA"),
		TEXT("ExtR"), TEXT("ExtG"), TEXT("ExtB"), TEXT("ExtA"),
		TEXT("MossColor"), TEXT("SootColor"), TEXT("SaltColor"),
		TEXT("MoistureStrength"), TEXT("SootStrength"), TEXT("ScorchStrength"), TEXT("MossStrength"), TEXT("CrackStrength"),
		TEXT("Height"), TEXT("HeightBlend"), TEXT("Contrast") };

	const TCHAR* CommonCode =
		// Height blending reshapes the stored masks before anything reads them (exactly unchanged at blend 0).
		// Contrast then reshapes how boldly faint wear reads (also exactly unchanged at 1).
		TEXT("const float4 VM = GrungifyContrast(GrungifyHeightMasks(float4(MaskR, MaskG, MaskB, MaskA), Height, HeightBlend), Contrast);\n")
		TEXT("const float4 TM = GrungifyContrast(GrungifyHeightMasks(float4(TexR, TexG, TexB, TexA), Height, HeightBlend), Contrast);\n")
		TEXT("const float4 CS = float4(ChipStrength, DustStrength, RunoffStrength, HandlingStrength);\n")
		TEXT("const float4 IM = GrungifyContrast(GrungifyHeightMasks(float4(InstR, InstG, InstB, InstA), Height, HeightBlend), Contrast);\n")
		TEXT("const float4 EM = GrungifyContrast(GrungifyHeightExtended(float4(ExtR, ExtG, ExtB, ExtA), Height, HeightBlend), Contrast);\n")
		TEXT("const float4 ES = float4(MoistureStrength, SootStrength, ScorchStrength, MossStrength);\n");

	const TCHAR* EvaluateCode =
		TEXT("FGrungifyResult R = GrungifyEvaluateFull(BaseColor, Roughness, Metallic, MaskMode, VM, TM, IM, EM, WorldPos, WorldNormal, ")
		TEXT("SurfaceType, SurfaceOverride, FeatureScale, SeedOffset, DustColor.rgb, RunoffColor.rgb, ChipColor.rgb, Strength, CS, ")
		TEXT("MossColor.rgb, SootColor.rgb, SaltColor.rgb, ES, CrackStrength);\n");

	UMaterialExpressionCustom* AddWeatheringCustom(UMaterialFunction* Function, FBuildContext& Ctx, const FWeatheringSources& Sources,
		const TCHAR* Description, ECustomMaterialOutputType OutputType, const FString& Body, int32 X, int32 Y,
		const TArray<const TCHAR*>& InputNames = WeatheringInputNames)
	{
		UMaterialExpressionCustom* Custom = AddToFunction<UMaterialExpressionCustom>(Function, X, Y);
		Custom->Description = Description;
		Custom->OutputType = OutputType;
		Custom->IncludeFilePaths.Add(IncludePath);
		Custom->Code = FString(CommonCode) + Body;
		SetCustomInputs(Custom, InputNames);

		for (const TCHAR* Input : InputNames)
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

	/** Everything the weathering custom nodes read besides the artist's own inputs (both functions share these). */
	void AddSharedSources(UMaterialFunction* Function, FBuildContext& Ctx, UTexture2D* NoMask, FWeatheringSources& S, bool bLandscape = false)
	{
		// Landscapes have no vertex-colour masks, custom primitive data or instances: those read as zero or as plain
		// parameters the zone sets on the landscape's material instance, and the masks are projected by world XY.
		UMaterialExpressionConstant* Zero = nullptr;
		if (bLandscape)
		{
			Zero = AddToFunction<UMaterialExpressionConstant>(Function, -1400, 600);
			Zero->R = 0.f;
		}
		auto ModeParameter = [&](const TCHAR* Name, int32 Offset, float LandscapeDefault, int32 Y) -> UMaterialExpressionScalarParameter*
		{
			return bLandscape ? AddScalar(Function, Name, LandscapeDefault, Y) : AddCustomDataParameter(Function, Name, Offset, Y);
		};

		UMaterialExpressionFunctionInput* SurfaceInput = AddFunctionInput(Function, TEXT("SurfaceOverride"), FunctionInput_Scalar, 4, FVector4f(-1.f, 0.f, 0.f, 0.f),
			TEXT("-1 = use the type the zone detected for this mesh. 0 = stone, 1 = metal, 2 = trim. Unconnected: use the owned material instance's Grungify Surface Override."), 480);
		UMaterialExpressionScalarParameter* SurfaceFallback = AddScalar(Function, TEXT("Grungify Surface Override"), -1.f, 520);
		Ctx.Connect(SurfaceFallback, TEXT(""), SurfaceInput, TEXT("Preview"));
		S.Add(TEXT("SurfaceOverride"), SurfaceInput);

		if (bLandscape)
		{
			for (const TCHAR* Input : { TEXT("MaskR"), TEXT("MaskG"), TEXT("MaskB"), TEXT("MaskA") }) { S.Add(Input, Zero); }
		}
		else
		{
			UMaterialExpressionVertexColor* VertexColor = AddToFunction<UMaterialExpressionVertexColor>(Function, -1400, 600);
			S.Add(TEXT("MaskR"), VertexColor, TEXT("R"));
			S.Add(TEXT("MaskG"), VertexColor, TEXT("G"));
			S.Add(TEXT("MaskB"), VertexColor, TEXT("B"));
			S.Add(TEXT("MaskA"), VertexColor, TEXT("A"));
		}

		UMaterialExpression* MaskUV = nullptr;
		if (bLandscape)
		{
			// UV = (WorldPos.xy - Rect.xy) / Rect.zw, with Rect set by the zone.
			UMaterialExpressionWorldPosition* World = AddToFunction<UMaterialExpressionWorldPosition>(Function, -2300, 700);
			UMaterialExpressionComponentMask* WorldXY = AddToFunction<UMaterialExpressionComponentMask>(Function, -2100, 700);
			SetMask(WorldXY, true, true, false, false);
			UMaterialExpressionVectorParameter* Rect = AddColor(Function, *Grungify::LandscapeRectParameterName().ToString(), FLinearColor(0.f, 0.f, 1.f, 1.f), 820);
			UMaterialExpressionComponentMask* Origin = AddToFunction<UMaterialExpressionComponentMask>(Function, -2100, 820);
			SetMask(Origin, true, true, false, false);
			UMaterialExpressionComponentMask* Extent = AddToFunction<UMaterialExpressionComponentMask>(Function, -2100, 900);
			SetMask(Extent, false, false, true, true);
			UMaterialExpressionSubtract* Local = AddToFunction<UMaterialExpressionSubtract>(Function, -1900, 760);
			UMaterialExpressionDivide* Projected = AddToFunction<UMaterialExpressionDivide>(Function, -1700, 760);
			Ctx.Connect(World, TEXT(""), WorldXY, TEXT(""));
			Ctx.Connect(Rect, TEXT(""), Origin, TEXT(""));
			Ctx.Connect(Rect, TEXT(""), Extent, TEXT(""));
			Ctx.Connect(WorldXY, TEXT(""), Local, TEXT("A"));
			Ctx.Connect(Origin, TEXT(""), Local, TEXT("B"));
			Ctx.Connect(Local, TEXT(""), Projected, TEXT("A"));
			Ctx.Connect(Extent, TEXT(""), Projected, TEXT("B"));
			MaskUV = Projected;
		}
		else
		{
			UMaterialExpressionTextureCoordinate* MaskChannel = AddToFunction<UMaterialExpressionTextureCoordinate>(Function, -2000, 700);
			MaskChannel->CoordinateIndex = UGrungifySettings::Get()->MaskUVChannel;
			// Skeletal meshes often only have UV0; the zone sets Grungify Use UV0 on their owned instances when it baked there.
			UMaterialExpressionTextureCoordinate* FirstChannel = AddToFunction<UMaterialExpressionTextureCoordinate>(Function, -2000, 820);
			FirstChannel->CoordinateIndex = 0;
			UMaterialExpressionScalarParameter* UseFirst = AddScalar(Function, TEXT("Grungify Use UV0"), 0.f, 900);
			UMaterialExpressionLinearInterpolate* PickUV = AddToFunction<UMaterialExpressionLinearInterpolate>(Function, -1700, 760);
			Ctx.Connect(MaskChannel, TEXT(""), PickUV, TEXT("A"));
			Ctx.Connect(FirstChannel, TEXT(""), PickUV, TEXT("B"));
			Ctx.Connect(UseFirst, TEXT(""), PickUV, TEXT("Alpha"));
			MaskUV = PickUV;
		}
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

		UMaterialExpressionScalarParameter* Mode = ModeParameter(TEXT("Grungify Mask Mode"), Grungify::CPD_MaskMode, 0.f, 1200);
		UMaterialExpressionScalarParameter* Active = AddScalar(Function, TEXT("Grungify Active"), 0.f, 1160);
		UMaterialExpressionMultiply* OwnedMode = AddToFunction<UMaterialExpressionMultiply>(Function, -1050, 1200);
		Ctx.Connect(Mode, TEXT(""), OwnedMode, TEXT("A"));
		Ctx.Connect(Active, TEXT(""), OwnedMode, TEXT("B"));
		S.Add(TEXT("MaskMode"), OwnedMode);
		S.Add(TEXT("SurfaceType"), ModeParameter(TEXT("Grungify Surface Type"), Grungify::CPD_SurfaceType, 0.f, 1300));
		S.Add(TEXT("FeatureScale"), ModeParameter(TEXT("Grungify Feature Scale"), Grungify::CPD_FeatureScale, 25.f, 1400));
		S.Add(TEXT("SeedOffset"), ModeParameter(TEXT("Grungify Seed Offset"), Grungify::CPD_SeedOffset, 0.f, 1500));

		S.Add(TEXT("DustColor"), AddColor(Function, TEXT("Grungify Dust Color"), FLinearColor(0.55f, 0.50f, 0.42f), 1600));
		S.Add(TEXT("RunoffColor"), AddColor(Function, TEXT("Grungify Runoff Color"), FLinearColor(0.16f, 0.11f, 0.07f), 1800));
		S.Add(TEXT("ChipColor"), AddColor(Function, TEXT("Grungify Trim Chip Color"), FLinearColor(0.45f, 0.42f, 0.38f), 2000));

		S.Add(TEXT("Strength"), AddScalar(Function, TEXT("Grungify Strength"), 1.f, 2200));
		S.Add(TEXT("ChipStrength"), AddScalar(Function, TEXT("Grungify Chip Strength"), 1.f, 2300));
		S.Add(TEXT("DustStrength"), AddScalar(Function, TEXT("Grungify Dust Strength"), 1.f, 2400));
		S.Add(TEXT("RunoffStrength"), AddScalar(Function, TEXT("Grungify Runoff Strength"), 1.f, 2500));
		S.Add(TEXT("HandlingStrength"), AddScalar(Function, TEXT("Grungify Handling Strength"), 1.f, 2600));
		S.Add(TEXT("Relief"), AddScalar(Function, TEXT("Grungify Relief Strength"), 0.f, 2700));

		// Instanced meshes: per-instance custom data (returns the default 0 on ordinary meshes).
		const TCHAR* InstanceInputs[] = { TEXT("InstR"), TEXT("InstG"), TEXT("InstB"), TEXT("InstA") };
		for (int32 Index = 0; Index < Grungify::InstanceDataCount; ++Index)
		{
			if (bLandscape)
			{
				S.Add(InstanceInputs[Index], Zero);
				continue;
			}
			UMaterialExpressionPerInstanceCustomData* Instance = AddToFunction<UMaterialExpressionPerInstanceCustomData>(Function, -1400, 2800 + Index * 100);
			Instance->DataIndex = uint32(UGrungifySettings::Get()->InstanceCustomDataIndex + Index);
			S.Add(InstanceInputs[Index], Instance);
		}

		// Extended layers: moisture, soot, scorch, moss in a second mask texture on the same UVs.
		UMaterialExpressionTextureSampleParameter2D* ExtendedTexture = AddToFunction<UMaterialExpressionTextureSampleParameter2D>(Function, -1400, 3300);
		ExtendedTexture->ParameterName = Grungify::ExtendedMaskTextureParameterName();
		ExtendedTexture->Group = TEXT("Grungify");
		ExtendedTexture->Texture = NoMask;
		ExtendedTexture->SamplerType = SAMPLERTYPE_LinearColor;
		Ctx.Connect(MaskUV, TEXT(""), ExtendedTexture, TEXT("UVs"));
		S.Add(TEXT("ExtR"), ExtendedTexture, TEXT("R"));
		S.Add(TEXT("ExtG"), ExtendedTexture, TEXT("G"));
		S.Add(TEXT("ExtB"), ExtendedTexture, TEXT("B"));
		S.Add(TEXT("ExtA"), ExtendedTexture, TEXT("A"));

		S.Add(TEXT("MossColor"), AddColor(Function, TEXT("Grungify Moss Color"), FLinearColor(0.12f, 0.18f, 0.05f), 3600));
		S.Add(TEXT("SootColor"), AddColor(Function, TEXT("Grungify Soot Color"), FLinearColor(0.03f, 0.028f, 0.025f), 3800));
		S.Add(TEXT("SaltColor"), AddColor(Function, TEXT("Grungify Efflorescence Color"), FLinearColor(0.78f, 0.76f, 0.70f), 4000));
		S.Add(TEXT("MoistureStrength"), AddScalar(Function, TEXT("Grungify Moisture Strength"), 1.f, 4200));
		S.Add(TEXT("SootStrength"), AddScalar(Function, TEXT("Grungify Soot Strength"), 1.f, 4300));
		S.Add(TEXT("ScorchStrength"), AddScalar(Function, TEXT("Grungify Scorch Strength"), 1.f, 4400));
		S.Add(TEXT("MossStrength"), AddScalar(Function, TEXT("Grungify Moss Strength"), 1.f, 4500));
		S.Add(TEXT("CrackStrength"), AddScalar(Function, TEXT("Grungify Crack Strength"), 0.f, 4600));
		// Not read by the shader; runtime wear uses it to size brushes in texels.
		AddScalar(Function, TEXT("Grungify Mask Texel Size"), 4.f, 4700);
		S.Add(TEXT("HeightBlend"), AddScalar(Function, TEXT("Grungify Height Blend"), 0.f, 4900));
		S.Add(TEXT("Contrast"), AddScalar(Function, TEXT("Grungify Contrast"), 1.f, 5100));
	}

	UMaterialExpressionFunctionInput* AddHeightInput(UMaterialFunction* Function, int32 Sort)
	{
		return AddFunctionInput(Function, TEXT("Height"), FunctionInput_Scalar, Sort, FVector4f(0.5f, 0.f, 0.f, 0.f),
			TEXT("Optional height map (0..1). With Grungify Height Blend above 0, chips favour the high parts and dust, damp and moss the low ones."), 600);
	}

	void PopulateFunction(UMaterialFunction* Function, UTexture2D* NoMask, FGrungifyContentReport& Report, bool bLandscape = false)
	{
		FBuildContext Ctx{ Report, bLandscape ? TEXT("MF_GrungifyLandscape") : TEXT("MF_GrungifyWeathering") };

		Function->Description = PendingDescription();
		Function->bExposeToLibrary = true;
		Function->LibraryCategoriesText.Reset();
		Function->LibraryCategoriesText.Add(LOCTEXT("LibraryCategory", "Grungify"));

		FWeatheringSources S;
		S.Add(TEXT("BaseColor"), AddFunctionInput(Function, TEXT("BaseColor"), FunctionInput_Vector3, 0, FVector4f(0.5f, 0.5f, 0.5f, 0.f), TEXT("Unweathered base colour."), 0));
		S.Add(TEXT("Roughness"), AddFunctionInput(Function, TEXT("Roughness"), FunctionInput_Scalar, 1, FVector4f(0.6f, 0.f, 0.f, 0.f), TEXT("Unweathered roughness."), 120));
		S.Add(TEXT("Metallic"), AddFunctionInput(Function, TEXT("Metallic"), FunctionInput_Scalar, 2, FVector4f(0.f, 0.f, 0.f, 0.f), TEXT("Unweathered metallic."), 240));
		S.Add(TEXT("NormalTS"), AddFunctionInput(Function, TEXT("Normal"), FunctionInput_Vector3, 3, FVector4f(0.f, 0.f, 1.f, 0.f),
			TEXT("Your tangent-space normal. Passed through unchanged unless Grungify Relief Strength is above 0."), 360));
		S.Add(TEXT("Height"), AddHeightInput(Function, 5));
		AddSharedSources(Function, Ctx, NoMask, S, bLandscape);

		UMaterialExpressionCustom* ColorNode = AddWeatheringCustom(Function, Ctx, S, TEXT("Grungify Base Color"), CMOT_Float3,
			FString(EvaluateCode) + TEXT("return R.BaseColor;"), -600, 0);
		UMaterialExpressionCustom* SurfaceNode = AddWeatheringCustom(Function, Ctx, S, TEXT("Grungify Roughness Metallic"), CMOT_Float2,
			FString(EvaluateCode) + TEXT("return float2(R.Roughness, R.Metallic);"), -600, 500);
		UMaterialExpressionCustom* NormalNode = AddWeatheringCustom(Function, Ctx, S, TEXT("Grungify Relief Normal"), CMOT_Float3,
			TEXT("return GrungifyReliefNormalFull(NormalTS, Relief, MaskMode, VM, TM, IM, WorldPos, WorldNormal, ")
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
		UMaterialExpressionScalarParameter* Active = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -900, 750);
		Active->ParameterName = Grungify::ActiveParameterName();
		Active->Group = TEXT("Grungify");
		Active->DefaultValue = 0.f;
		UMaterialExpressionMultiply* OwnedMode = AddToMaterial<UMaterialExpressionMultiply>(Material, -650, 550);
		Ctx.Connect(Mode, TEXT(""), OwnedMode, TEXT("A"));
		Ctx.Connect(Active, TEXT(""), OwnedMode, TEXT("B"));

		UMaterialExpressionTextureSampleParameter2D* ExtendedTexture = AddToMaterial<UMaterialExpressionTextureSampleParameter2D>(Material, -900, 850);
		ExtendedTexture->ParameterName = Grungify::ExtendedMaskTextureParameterName();
		ExtendedTexture->Texture = NoMask;
		ExtendedTexture->SamplerType = SAMPLERTYPE_LinearColor;
		Ctx.Connect(MaskUV, TEXT(""), ExtendedTexture, TEXT("UVs"));

		UMaterialExpressionPerInstanceCustomData* Instance[Grungify::InstanceDataCount];
		for (int32 Index = 0; Index < Grungify::InstanceDataCount; ++Index)
		{
			Instance[Index] = AddToMaterial<UMaterialExpressionPerInstanceCustomData>(Material, -900, 1100 + Index * 100);
			Instance[Index]->DataIndex = uint32(UGrungifySettings::Get()->InstanceCustomDataIndex + Index);
		}

		UMaterialExpressionCustom* Custom = AddToMaterial<UMaterialExpressionCustom>(Material, -400, 0);
		Custom->Description = TEXT("Grungify Mask Debug");
		Custom->OutputType = CMOT_Float3;
		Custom->IncludeFilePaths.Add(IncludePath);
		Custom->Code = TEXT("return GrungifyMaskDebugFull(MaskMode, float4(MaskR, MaskG, MaskB, MaskA), float4(TexR, TexG, TexB, TexA), ")
			TEXT("float4(InstR, InstG, InstB, InstA), float4(ExtR, ExtG, ExtB, ExtA), Channel);");
		SetCustomInputs(Custom, { TEXT("MaskR"), TEXT("MaskG"), TEXT("MaskB"), TEXT("MaskA"), TEXT("TexR"), TEXT("TexG"), TEXT("TexB"), TEXT("TexA"),
			TEXT("InstR"), TEXT("InstG"), TEXT("InstB"), TEXT("InstA"), TEXT("ExtR"), TEXT("ExtG"), TEXT("ExtB"), TEXT("ExtA"), TEXT("MaskMode"), TEXT("Channel") });
		Ctx.Connect(Instance[0], TEXT(""), Custom, TEXT("InstR"));
		Ctx.Connect(Instance[1], TEXT(""), Custom, TEXT("InstG"));
		Ctx.Connect(Instance[2], TEXT(""), Custom, TEXT("InstB"));
		Ctx.Connect(Instance[3], TEXT(""), Custom, TEXT("InstA"));
		Ctx.Connect(ExtendedTexture, TEXT("R"), Custom, TEXT("ExtR"));
		Ctx.Connect(ExtendedTexture, TEXT("G"), Custom, TEXT("ExtG"));
		Ctx.Connect(ExtendedTexture, TEXT("B"), Custom, TEXT("ExtB"));
		Ctx.Connect(ExtendedTexture, TEXT("A"), Custom, TEXT("ExtA"));

		Ctx.Connect(VertexColor, TEXT("R"), Custom, TEXT("MaskR"));
		Ctx.Connect(VertexColor, TEXT("G"), Custom, TEXT("MaskG"));
		Ctx.Connect(VertexColor, TEXT("B"), Custom, TEXT("MaskB"));
		Ctx.Connect(VertexColor, TEXT("A"), Custom, TEXT("MaskA"));
		Ctx.Connect(MaskTexture, TEXT("R"), Custom, TEXT("TexR"));
		Ctx.Connect(MaskTexture, TEXT("G"), Custom, TEXT("TexG"));
		Ctx.Connect(MaskTexture, TEXT("B"), Custom, TEXT("TexB"));
		Ctx.Connect(MaskTexture, TEXT("A"), Custom, TEXT("TexA"));
		Ctx.Connect(OwnedMode, TEXT(""), Custom, TEXT("MaskMode"));
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
		SurfaceOverride->ParameterName = Grungify::SurfaceOverrideParameterName();
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
		if (FMaterialResource* Resource = GRUNGIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material))
		{
			Resource->FinishCompilation();
			for (const FString& Error : Resource->GetCompileErrors())
			{
				Report.Errors.Add(FString::Printf(TEXT("%s: %s"), *Material->GetName(), *Error));
			}
		}
		else
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: no material resource; compilation could not be validated"), *Material->GetName()));
		}
	}

	bool ValidateOwnedMode(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions, const TCHAR* Asset, FGrungifyContentReport& Report, bool bModeUsesCPD = true)
	{
		UMaterialExpressionScalarParameter* Active = nullptr;
		UMaterialExpressionScalarParameter* Mode = nullptr;
		int32 ActiveCount = 0;
		int32 ModeCount = 0;
		for (UMaterialExpression* Expression : Expressions)
		{
			if (UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				if (Parameter->ParameterName == Grungify::ActiveParameterName()) { Active = Parameter; ++ActiveCount; }
				if (Parameter->ParameterName == TEXT("Grungify Mask Mode")) { Mode = Parameter; ++ModeCount; }
			}
		}
		if (ActiveCount != 1 || ModeCount != 1 || Active->bUseCustomPrimitiveData || Active->DefaultValue != 0.f || Mode->DefaultValue != 0.f
			|| Mode->bUseCustomPrimitiveData != bModeUsesCPD || (bModeUsesCPD && Mode->PrimitiveDataIndex != CustomDataIndex(Grungify::CPD_MaskMode)))
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: missing or invalid default-inactive ownership/mode parameters"), Asset));
			return false;
		}
		UMaterialExpressionMultiply* Gate = nullptr;
		for (UMaterialExpression* Expression : Expressions)
		{
			if (UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				if (Multiply->A.Expression == Mode && Multiply->B.Expression == Active) { Gate = Multiply; }
			}
		}
		int32 CustomCount = 0;
		for (UMaterialExpression* Expression : Expressions)
		{
			if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression))
			{
				++CustomCount;
				bool bHasOwnedMode = false;
				for (const FCustomInput& Input : Custom->Inputs)
				{
					if (!Input.Input.Expression)
					{
						Report.Errors.Add(FString::Printf(TEXT("%s: %s has an unconnected custom input %s"), Asset, *Custom->Description, *Input.InputName.ToString()));
					}
					if (Input.InputName == TEXT("MaskMode")) { bHasOwnedMode = Gate && Input.Input.Expression == Gate; }
				}
				if (!bHasOwnedMode || !Custom->IncludeFilePaths.Contains(IncludePath))
				{
					Report.Errors.Add(FString::Printf(TEXT("%s: %s bypasses the ownership gate or shader include"), Asset, *Custom->Description));
				}
			}
		}
		if (!Gate || CustomCount == 0)
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: incomplete ownership gate/custom graph"), Asset));
			return false;
		}
		return true;
	}

	void ValidateGeneratedGraphs(UMaterialFunction* Function, UMaterial* Debug, UMaterial* Example, UTexture2D* NoMask, FGrungifyContentReport& Report)
	{
		if (!Function || !Debug || !Example || !NoMask)
		{
			Report.Errors.Add(TEXT("One or more generated material/function/texture assets could not be loaded"));
			return;
		}
		ValidateOwnedMode(Function->GetExpressions(), TEXT("MF_GrungifyWeathering"), Report);
		ValidateOwnedMode(Debug->GetExpressions(), TEXT("M_GrungifyMaskDebug"), Report);
		TSet<FName> Outputs;
		UMaterialExpressionFunctionOutput* RoughnessOutput = nullptr;
		UMaterialExpressionFunctionOutput* MetallicOutput = nullptr;
		int32 ValidChannelMasks = 0;
		UMaterialExpressionFunctionInput* SurfaceInput = nullptr;
		UMaterialExpressionScalarParameter* SurfaceFallback = nullptr;
		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				if (Output->A.Expression) { Outputs.Add(Output->OutputName); }
				if (Output->OutputName == TEXT("Roughness")) { RoughnessOutput = Output; }
				if (Output->OutputName == TEXT("Metallic")) { MetallicOutput = Output; }
			}
			if (UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
			{
				if (Mask->Input.Expression && !Mask->B && !Mask->A && ((Mask->R && !Mask->G) || (!Mask->R && Mask->G))) { ++ValidChannelMasks; }
			}
			if (UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				if (Input->InputName == TEXT("SurfaceOverride")) { SurfaceInput = Input; }
			}
			if (UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				if (Parameter->ParameterName == Grungify::SurfaceOverrideParameterName()) { SurfaceFallback = Parameter; }
			}
		}
		if (Outputs.Num() != 4 || !Outputs.Contains(TEXT("BaseColor")) || !Outputs.Contains(TEXT("Roughness"))
			|| !Outputs.Contains(TEXT("Metallic")) || !Outputs.Contains(TEXT("Normal")) || ValidChannelMasks != 2)
		{
			Report.Errors.Add(TEXT("MF_GrungifyWeathering: invalid connected outputs or R/G-only channel masks"));
		}
		const UMaterialExpressionComponentMask* RoughnessMask = RoughnessOutput ? Cast<UMaterialExpressionComponentMask>(RoughnessOutput->A.Expression) : nullptr;
		const UMaterialExpressionComponentMask* MetallicMask = MetallicOutput ? Cast<UMaterialExpressionComponentMask>(MetallicOutput->A.Expression) : nullptr;
		const UMaterialExpressionCustom* SurfaceResult = RoughnessMask ? Cast<UMaterialExpressionCustom>(RoughnessMask->Input.Expression) : nullptr;
		if (!RoughnessMask || !MetallicMask || !SurfaceResult || SurfaceResult->Description != TEXT("Grungify Roughness Metallic")
			|| SurfaceResult->OutputType != CMOT_Float2 || RoughnessMask->Input.OutputIndex != 0
			|| MetallicMask->Input.Expression != SurfaceResult || MetallicMask->Input.OutputIndex != 0
			|| RoughnessOutput->A.OutputIndex != 0 || MetallicOutput->A.OutputIndex != 0
			|| !RoughnessMask->R || RoughnessMask->G || RoughnessMask->B || RoughnessMask->A
			|| MetallicMask->R || !MetallicMask->G || MetallicMask->B || MetallicMask->A)
		{
			Report.Errors.Add(TEXT("MF_GrungifyWeathering: Roughness must use R-only and Metallic G-only from the same float2 surface result"));
		}
		if (!SurfaceInput || !SurfaceFallback || SurfaceFallback->bUseCustomPrimitiveData || SurfaceFallback->DefaultValue != -1.f
			|| !SurfaceInput->bUsePreviewValueAsDefault || SurfaceInput->Preview.Expression != SurfaceFallback)
		{
			Report.Errors.Add(TEXT("MF_GrungifyWeathering: invalid per-material surface override fallback"));
		}
		TArray64<uint8> NoMaskPixels;
		if (NoMask->SRGB || NoMask->CompressionSettings != TC_Default || NoMask->Source.GetSizeX() != 4 || NoMask->Source.GetSizeY() != 4
			|| NoMask->Source.GetFormat() != TSF_BGRA8 || !NoMask->Source.GetMipData(NoMaskPixels, 0) || NoMaskPixels.Num() != 64)
		{
			Report.Errors.Add(TEXT("T_GrungifyNoMask: invalid linear RGBA fallback texture"));
		}
		else
		{
			for (uint8 Byte : NoMaskPixels)
			{
				if (Byte != 0) { Report.Errors.Add(TEXT("T_GrungifyNoMask: fallback mask is not transparent black")); break; }
			}
		}
	}

	/** The weathering custom inputs minus the artist's legacy inputs, plus the stone chip colour Substrate needs. */
	TArray<const TCHAR*> SubstrateInputNames()
	{
		TArray<const TCHAR*> Names;
		for (const TCHAR* Name : WeatheringInputNames)
		{
			if (FCString::Strcmp(Name, TEXT("BaseColor")) != 0 && FCString::Strcmp(Name, TEXT("Roughness")) != 0 && FCString::Strcmp(Name, TEXT("Metallic")) != 0)
			{
				Names.Add(Name);
			}
		}
		Names.Add(TEXT("StoneChipColor"));
		return Names;
	}

	/**
	 * MF_GrungifySubstrate: Material (Substrate) in, Material out. One grime slab, whose albedo, F0, roughness and coverage
	 * come from GrungifyEvaluateGrime, is mixed over the input. Inactive meshes get zero coverage.
	 */
	void PopulateSubstrateFunction(UMaterialFunction* Function, UTexture2D* NoMask, FGrungifyContentReport& Report)
	{
		FBuildContext Ctx{ Report, TEXT("MF_GrungifySubstrate") };

		Function->Description = PendingDescription();
		Function->bExposeToLibrary = true;
		Function->LibraryCategoriesText.Reset();
		Function->LibraryCategoriesText.Add(LOCTEXT("LibraryCategory", "Grungify"));

		UMaterialExpressionFunctionInput* MaterialInput = AddFunctionInput(Function, TEXT("Material"), FunctionInput_Substrate, 0, FVector4f(0.f, 0.f, 0.f, 0.f),
			TEXT("Your Substrate material (what normally goes into Front Material)."), 0);
		FWeatheringSources S;
		S.Add(TEXT("NormalTS"), AddFunctionInput(Function, TEXT("Normal"), FunctionInput_Vector3, 1, FVector4f(0.f, 0.f, 1.f, 0.f),
			TEXT("Tangent-space normal for the grime layer, usually your slab's normal. Relief is added on top when Grungify Relief Strength is above 0."), 360));
		S.Add(TEXT("Height"), AddHeightInput(Function, 2));
		AddSharedSources(Function, Ctx, NoMask, S);
		S.Add(TEXT("StoneChipColor"), AddColor(Function, TEXT("Grungify Stone Chip Color"), FLinearColor(0.62f, 0.60f, 0.56f), 4800));

		UMaterialExpressionCustom* Grime = AddWeatheringCustom(Function, Ctx, S, TEXT("Grungify Substrate Grime"), CMOT_Float1,
			TEXT("FGrungifyGrime G = GrungifyEvaluateGrime(MaskMode, VM, TM, IM, EM, WorldPos, WorldNormal, SurfaceType, SurfaceOverride, ")
			TEXT("FeatureScale, SeedOffset, DustColor.rgb, RunoffColor.rgb, ChipColor.rgb, StoneChipColor.rgb, Strength, CS, ")
			TEXT("MossColor.rgb, SootColor.rgb, SaltColor.rgb, ES, CrackStrength);\n")
			TEXT("Albedo = G.Albedo;\nF0 = G.F0;\nGrimeRoughness = G.Roughness;\nreturn G.Coverage;"),
			-600, 0, SubstrateInputNames());
		// Output 0 is the coverage; 1..3 are the slab parameters.
		Grime->AdditionalOutputs.Reset();
		Grime->AdditionalOutputs.Add({ TEXT("Albedo"), CMOT_Float3 });
		Grime->AdditionalOutputs.Add({ TEXT("F0"), CMOT_Float3 });
		Grime->AdditionalOutputs.Add({ TEXT("GrimeRoughness"), CMOT_Float1 });
		if (FProperty* Property = FindFProperty<FProperty>(UMaterialExpressionCustom::StaticClass(), GET_MEMBER_NAME_CHECKED(UMaterialExpressionCustom, AdditionalOutputs)))
		{
			FPropertyChangedEvent Event(Property);
			Grime->PostEditChangeProperty(Event);
		}
		if (Grime->GetOutputs().Num() != 4)
		{
			Report.Errors.Add(TEXT("MF_GrungifySubstrate: the grime node did not expose its four outputs"));
		}

		UMaterialExpressionCustom* NormalNode = AddWeatheringCustom(Function, Ctx, S, TEXT("Grungify Relief Normal"), CMOT_Float3,
			TEXT("return GrungifyReliefNormalFull(NormalTS, Relief, MaskMode, VM, TM, IM, WorldPos, WorldNormal, ")
			TEXT("Parameters.TangentToWorld[0], Parameters.TangentToWorld[1], FeatureScale, SeedOffset, Strength, CS);"), -600, 1000,
			SubstrateInputNames());

		UMaterialExpressionSubstrateSlabBSDF* Slab = AddToFunction<UMaterialExpressionSubstrateSlabBSDF>(Function, -200, 200);
		Slab->DiffuseAlbedo.Connect(1, Grime);
		Slab->F0.Connect(2, Grime);
		Slab->Roughness.Connect(3, Grime);
		Slab->Normal.Connect(0, NormalNode);

		UMaterialExpressionSubstrateHorizontalMixing* Mix = AddToFunction<UMaterialExpressionSubstrateHorizontalMixing>(Function, 0, 0);
		Mix->Background.Connect(0, MaterialInput);
		Mix->Foreground.Connect(0, Slab);
		Mix->Mix.Connect(0, Grime);

		UMaterialExpressionFunctionOutput* Output = AddToFunction<UMaterialExpressionFunctionOutput>(Function, 250, 0);
		Output->OutputName = TEXT("Material");
		Output->SortPriority = 0;
		Output->A.Connect(0, Mix);

		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
	}

	void ValidateSubstrateGraph(UMaterialFunction* Function, FGrungifyContentReport& Report)
	{
		if (!Function)
		{
			Report.Errors.Add(TEXT("MF_GrungifySubstrate could not be loaded"));
			return;
		}
		ValidateOwnedMode(Function->GetExpressions(), TEXT("MF_GrungifySubstrate"), Report);
		int32 Outputs = 0;
		const UMaterialExpressionSubstrateHorizontalMixing* Mix = nullptr;
		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				++Outputs;
				if (Output->OutputName == TEXT("Material"))
				{
					Mix = Cast<UMaterialExpressionSubstrateHorizontalMixing>(Output->A.Expression);
				}
			}
		}
		const UMaterialExpressionFunctionInput* Background = Mix ? Cast<UMaterialExpressionFunctionInput>(Mix->Background.Expression) : nullptr;
		const UMaterialExpressionCustom* Coverage = Mix ? Cast<UMaterialExpressionCustom>(Mix->Mix.Expression) : nullptr;
		const UMaterialExpressionSubstrateSlabBSDF* Slab = Mix ? Cast<UMaterialExpressionSubstrateSlabBSDF>(Mix->Foreground.Expression) : nullptr;
		if (Outputs != 1 || !Background || Background->InputType != FunctionInput_Substrate || !Coverage
			|| Coverage->Description != TEXT("Grungify Substrate Grime") || Mix->Mix.OutputIndex != 0 || !Slab
			|| Slab->DiffuseAlbedo.Expression != Coverage || Slab->F0.Expression != Coverage || Slab->Roughness.Expression != Coverage)
		{
			Report.Errors.Add(TEXT("MF_GrungifySubstrate: the input material must be mixed with the grime slab by the grime coverage"));
		}
	}

	/** Substrate graphs only compile in Substrate projects; there, compile a throwaway material that uses the function. */
	void CompileCheckSubstrate(UMaterialFunction* Function, FGrungifyContentReport& Report)
	{
		if (!Substrate::IsSubstrateEnabled())
		{
			Report.Notes.Add(TEXT("Substrate is off in this project: MF_GrungifySubstrate was built and validated but not compiled"));
			return;
		}
		UMaterial* Probe = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		FBuildContext Ctx{ Report, TEXT("MF_GrungifySubstrate (compile check)") };
		UMaterialExpressionSubstrateSlabBSDF* Slab = AddToMaterial<UMaterialExpressionSubstrateSlabBSDF>(Probe, -600, 0);
		UMaterialExpressionMaterialFunctionCall* Call = AddToMaterial<UMaterialExpressionMaterialFunctionCall>(Probe, -300, 0);
		if (!Call || !Call->SetMaterialFunction(Function))
		{
			Report.Errors.Add(TEXT("MF_GrungifySubstrate: could not call the function from a test material"));
			return;
		}
		Ctx.Connect(Slab, TEXT(""), Call, TEXT("Material"));
		Ctx.ConnectProperty(Call, TEXT("Material"), MP_FrontMaterial, TEXT("Front Material"));
		CompileAndCheck(Probe, Report);
	}

	/** MF_GrungifyLandscape: MF_GrungifyWeathering for landscape materials (world-projected masks, plain parameters). */
	void PopulateLandscapeFunction(UMaterialFunction* Function, UTexture2D* NoMask, FGrungifyContentReport& Report)
	{
		PopulateFunction(Function, NoMask, Report, /*bLandscape*/ true);
	}

	void ValidateLandscapeGraph(UMaterialFunction* Function, FGrungifyContentReport& Report)
	{
		if (!Function)
		{
			Report.Errors.Add(TEXT("MF_GrungifyLandscape could not be loaded"));
			return;
		}
		ValidateOwnedMode(Function->GetExpressions(), TEXT("MF_GrungifyLandscape"), Report, /*bModeUsesCPD*/ false);
		TSet<FName> Outputs;
		bool bRect = false;
		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				if (Output->A.Expression) { Outputs.Add(Output->OutputName); }
			}
			if (const UMaterialExpressionVectorParameter* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression))
			{
				bRect |= Parameter->ParameterName == Grungify::LandscapeRectParameterName();
			}
			if (const UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				if (Parameter->bUseCustomPrimitiveData)
				{
					Report.Errors.Add(TEXT("MF_GrungifyLandscape: landscapes have no custom primitive data, but a parameter reads it"));
				}
			}
		}
		if (Outputs.Num() != 4 || !Outputs.Contains(TEXT("BaseColor")) || !Outputs.Contains(TEXT("Normal")) || !bRect)
		{
			Report.Errors.Add(TEXT("MF_GrungifyLandscape: expected four connected outputs and the landscape rectangle parameter"));
		}
	}

	/** MF_GrungifyDisplacement: your displacement in, plus chips and cracks cut in and dust built up. */
	void PopulateDisplacementFunction(UMaterialFunction* Function, UTexture2D* NoMask, FGrungifyContentReport& Report)
	{
		FBuildContext Ctx{ Report, TEXT("MF_GrungifyDisplacement") };
		Function->Description = PendingDescription();
		Function->bExposeToLibrary = true;
		Function->LibraryCategoriesText.Reset();
		Function->LibraryCategoriesText.Add(LOCTEXT("LibraryCategory", "Grungify"));

		FWeatheringSources S;
		S.Add(TEXT("Displacement"), AddFunctionInput(Function, TEXT("Displacement"), FunctionInput_Scalar, 0, FVector4f(0.f, 0.f, 0.f, 0.f),
			TEXT("Your material's own displacement (0 when it has none)."), 0));
		S.Add(TEXT("Height"), AddHeightInput(Function, 1));
		AddSharedSources(Function, Ctx, NoMask, S);
		S.Add(TEXT("DisplacementDepth"), AddScalar(Function, TEXT("Grungify Displacement Depth"), 0.f, 5000));

		TArray<const TCHAR*> Names;
		for (const TCHAR* Name : WeatheringInputNames)
		{
			const FString Text(Name);
			if (Text != TEXT("BaseColor") && Text != TEXT("Roughness") && Text != TEXT("Metallic") && Text != TEXT("NormalTS") && Text != TEXT("Relief"))
			{
				Names.Add(Name);
			}
		}
		Names.Add(TEXT("Displacement"));
		Names.Add(TEXT("DisplacementDepth"));
		UMaterialExpressionCustom* Node = AddWeatheringCustom(Function, Ctx, S, TEXT("Grungify Displacement"), CMOT_Float1,
			TEXT("return Displacement + GrungifyDisplacementOffset(MaskMode, VM, TM, IM, WorldPos, WorldNormal, SurfaceType, SurfaceOverride, ")
			TEXT("FeatureScale, SeedOffset, Strength, CS, CrackStrength, DisplacementDepth);"), -600, 0, Names);

		UMaterialExpressionFunctionOutput* Output = AddToFunction<UMaterialExpressionFunctionOutput>(Function, 200, 0);
		Output->OutputName = TEXT("Displacement");
		Output->SortPriority = 0;
		Ctx.Connect(Node, TEXT(""), Output, TEXT(""));
		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
	}

	void ValidateDisplacementGraph(UMaterialFunction* Function, FGrungifyContentReport& Report)
	{
		if (!Function)
		{
			Report.Errors.Add(TEXT("MF_GrungifyDisplacement could not be loaded"));
			return;
		}
		ValidateOwnedMode(Function->GetExpressions(), TEXT("MF_GrungifyDisplacement"), Report);
		int32 Outputs = 0;
		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				Outputs += Output->A.Expression && Output->OutputName == TEXT("Displacement") ? 1 : 100;
			}
		}
		if (Outputs != 1)
		{
			Report.Errors.Add(TEXT("MF_GrungifyDisplacement: expected exactly one connected Displacement output"));
		}
	}

	/** Generated functions besides MF_GrungifyWeathering, built and validated the same way. */
	struct FExtraFunction
	{
		const TCHAR* Path;
		void (*Populate)(UMaterialFunction*, UTexture2D*, FGrungifyContentReport&);
		void (*Validate)(UMaterialFunction*, FGrungifyContentReport&);
		void (*CompileCheck)(UMaterialFunction*, FGrungifyContentReport&);
		const TCHAR* Description;
	};

	void NoCompileCheck(UMaterialFunction*, FGrungifyContentReport&) {}

	TArray<FExtraFunction> ExtraFunctions()
	{
		return {
			{ SubstrateFunctionPath, &PopulateSubstrateFunction, &ValidateSubstrateGraph, &CompileCheckSubstrate,
				TEXT("Substrate version of MF_GrungifyWeathering: mixes one grime slab over your Substrate material. Inactive owned-material gate gives zero coverage. ") },
			{ LandscapeFunctionPath, &PopulateLandscapeFunction, &ValidateLandscapeGraph, &NoCompileCheck,
				TEXT("Landscape version of MF_GrungifyWeathering: masks projected by world XY from the zone's landscape mask. Inactive gate passes inputs through. ") },
			{ DisplacementFunctionPath, &PopulateDisplacementFunction, &ValidateDisplacementGraph, &NoCompileCheck,
				TEXT("Adds Grungify depth to Nanite displacement: chips and cracks cut in, dust builds up (Grungify Displacement Depth, 0 = off). ") },
		};
	}

	/**
	 * A plugin installed in the engine (Fab / Marketplace) is shared and read-only: its prebuilt content must never be
	 * rebuilt on a buyer's machine. Copying the plugin into the project's Plugins folder makes it writable again.
	 */
	bool CanWritePluginContent(FString& OutWhy)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Grungify"));
		if (!Plugin)
		{
			OutWhy = TEXT("plugin not found");
			return false;
		}
		if (Plugin->GetType() == EPluginType::Engine || Plugin->GetDescriptor().bInstalled)
		{
			OutWhy = TEXT("Grungify is installed in the engine (for example from Fab), so its content is read-only");
			return false;
		}
		const FString Function = FPackageName::LongPackageNameToFilename(FunctionPath, FPackageName::GetAssetPackageExtension());
		if (FPaths::FileExists(Function) && IFileManager::Get().IsReadOnly(*Function))
		{
			OutWhy = TEXT("the plugin's content files are read-only (check them out of source control to rebuild)");
			return false;
		}
		return true;
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
	UMaterialFunction* Function = FindExisting<UMaterialFunction>(FunctionPath);
	UMaterial* Debug = FindExisting<UMaterial>(DebugMaterialPath);
	UMaterial* Example = FindExisting<UMaterial>(ExampleMaterialPath);
	UTexture2D* NoMask = FindExisting<UTexture2D>(NoMaskTexturePath);
	UBlueprint* Blueprint = FindExisting<UBlueprint>(ZoneBlueprintPath);
	TArray<UMaterialFunction*> Extras;
	for (const FExtraFunction& Extra : ExtraFunctions())
	{
		UMaterialFunction* Existing = FindExisting<UMaterialFunction>(Extra.Path);
		if (!Existing || !Existing->Description.Contains(CurrentMarker()))
		{
			return true;
		}
		Extras.Add(Existing);
	}
	if (!Function || !Function->Description.Contains(CurrentMarker()) || !Debug || !Example || !NoMask || !Blueprint
		|| !Blueprint->ParentClass || !Blueprint->ParentClass->IsChildOf(AGrungifyWeatheringZone::StaticClass())
		|| !Blueprint->GeneratedClass || Blueprint->Status == BS_Error)
	{
		return true;
	}
	TArray<UObject*> Assets{ Function, Debug, Example, NoMask, Blueprint };
	Assets.Append(Extras);
	for (UObject* Asset : Assets)
	{
		if (Asset->GetPackage()->IsDirty()) { return true; }
	}
	FGrungifyContentReport Validation;
	ValidateGeneratedGraphs(Function, Debug, Example, NoMask, Validation);
	for (int32 Index = 0; Index < Extras.Num(); ++Index)
	{
		ExtraFunctions()[Index].Validate(Extras[Index], Validation);
	}
	FString ExpectedToken;
	FString SavedToken;
	return Validation.Errors.Num() > 0 || !MakeReadinessToken(ExpectedToken)
		|| !FFileHelper::LoadFileToString(SavedToken, *ReadinessPath()) || SavedToken != ExpectedToken;
}

FGrungifyContentReport FGrungifyContentBuilder::Run(bool bForceRebuild)
{
	FGrungifyContentReport Report;
	TArray<UPackage*> Packages;
	const bool bRebuild = bForceRebuild || NeedsWork();
	const FString SavedDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"));

	auto Finish = [&]()
	{
		const FString Text = Report.ToString();
		if (!FFileHelper::SaveStringToFile(Text, *FPaths::Combine(SavedDirectory, TEXT("ContentReport.txt"))))
		{
			Report.Errors.Add(TEXT("Could not write Saved/Grungify/ContentReport.txt"));
		}
		if (Report.HasErrors())
		{
			UE_LOG(LogGrungify, Error, TEXT("Grungify content build had problems:\n%s"), *Report.ToString());
		}
		else if (Packages.Num() > 0)
		{
			UE_LOG(LogGrungify, Log, TEXT("Grungify content build:\n%s"), *Report.ToString());
		}
		return Report;
	};

	// Installed (Fab) copies ship prebuilt content and must never be rebuilt in place: validate it and stop.
	FString ReadOnlyReason;
	if (!CanWritePluginContent(ReadOnlyReason))
	{
		Report.Notes.Add(ReadOnlyReason + TEXT("; validating the shipped content instead of rebuilding it"));
		UMaterialFunction* Shipped = FindExisting<UMaterialFunction>(FunctionPath);
		if (!Shipped || !Shipped->Description.Contains(CurrentMarker()))
		{
			Report.Errors.Add(FString::Printf(TEXT("The shipped content was built for different settings than this project's (expected %s). ")
				TEXT("Reset Custom Primitive Data Index, Mask UV Channel and Instance Custom Data Index in Project Settings > Plugins > Grungify ")
				TEXT("to the shipped values, or copy the Grungify plugin into this project's Plugins folder so it can be rebuilt."), *CurrentMarker()));
			return Finish();
		}
		ValidateGeneratedGraphs(Shipped, FindExisting<UMaterial>(DebugMaterialPath), FindExisting<UMaterial>(ExampleMaterialPath), FindExisting<UTexture2D>(NoMaskTexturePath), Report);
		for (const FExtraFunction& Extra : ExtraFunctions())
		{
			Extra.Validate(FindExisting<UMaterialFunction>(Extra.Path), Report);
		}
		FString Token;
		if (Report.Errors.Num() == 0 && MakeReadinessToken(Token) && IFileManager::Get().MakeDirectory(*SavedDirectory, true)
			&& FFileHelper::SaveStringToFile(Token, *ReadinessPath()))
		{
			Report.bReady = true;
		}
		return Finish();
	}

	// Invalidate before touching any asset. An interrupted or failed rebuild cannot reuse a prior token.
	if (!IFileManager::Get().MakeDirectory(*SavedDirectory, true)
		|| !FFileHelper::SaveStringToFile(TEXT("unvalidated"), *ReadinessPath()))
	{
		Report.Errors.Add(TEXT("Could not invalidate content readiness; no generated assets were changed"));
		return Finish();
	}

	// The custom nodes include this file; nothing compiles without it.
	const FString Shader = ShaderPath();
	if (Shader.IsEmpty() || !FPaths::FileExists(Shader))
	{
		Report.Errors.Add(FString::Printf(TEXT("Shader include or plugin missing: %s"), *Shader));
		return Finish();
	}

	UTexture2D* NoMask = FindExisting<UTexture2D>(NoMaskTexturePath);
	if (!NoMask)
	{
		NoMask = BuildNoMaskTexture(CreatePackage(NoMaskTexturePath));
		Register(NoMask, Packages);
		Report.Created.Add(AssetName(NoMaskTexturePath));
	}
	else if (bRebuild)
	{
		NoMask->Modify();
		TArray<FColor> Pixels;
		Pixels.Init(FColor(0, 0, 0, 0), 16);
		NoMask->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		NoMask->SRGB = false;
		NoMask->CompressionSettings = TC_Default;
		NoMask->PostEditChange();
		Touch(NoMask, Packages);
		Report.Rebuilt.Add(AssetName(NoMaskTexturePath));
	}

	// The three generated material assets are rebuilt together whenever the function's marker is stale.
	UMaterialFunction* Function = FindExisting<UMaterialFunction>(FunctionPath);
	if (!Function)
	{
		Function = NewObject<UMaterialFunction>(CreatePackage(FunctionPath), *AssetName(FunctionPath), AssetFlags);
		Register(Function, Packages);
		Report.Created.Add(AssetName(FunctionPath));
	}
	else if (bRebuild)
	{
		Function->Modify();
		if (!ClearFunctionExpressions(Function, Report)) { return Finish(); }
		Touch(Function, Packages);
		Report.Rebuilt.Add(AssetName(FunctionPath));
	}
	if (bRebuild)
	{
		PopulateFunction(Function, NoMask, Report);
	}

	// The other generated functions follow the same create / rebuild-in-place rules.
	const TArray<FExtraFunction> Extras = ExtraFunctions();
	TArray<UMaterialFunction*> ExtraAssets;
	TArray<bool> ExtraBuilt;
	for (const FExtraFunction& Extra : Extras)
	{
		UMaterialFunction* Asset = FindExisting<UMaterialFunction>(Extra.Path);
		bool bBuild = bRebuild;
		if (!Asset)
		{
			Asset = NewObject<UMaterialFunction>(CreatePackage(Extra.Path), *AssetName(Extra.Path), AssetFlags);
			Register(Asset, Packages);
			Report.Created.Add(AssetName(Extra.Path));
			bBuild = true;
		}
		else if (bRebuild)
		{
			Asset->Modify();
			if (!ClearFunctionExpressions(Asset, Report)) { return Finish(); }
			Touch(Asset, Packages);
			Report.Rebuilt.Add(AssetName(Extra.Path));
		}
		if (bBuild)
		{
			Extra.Populate(Asset, NoMask, Report);
		}
		ExtraAssets.Add(Asset);
		ExtraBuilt.Add(bBuild);
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
		if (!ClearMaterialExpressions(Material, Report)) { return nullptr; }
		Touch(Material, Packages);
		Report.Rebuilt.Add(AssetName(Path));
		return Material;
	};

	if (UMaterial* Debug = PrepareMaterial(DebugMaterialPath))
	{
		PopulateDebugMaterial(Debug, NoMask, Report);
	}
	if (UMaterial* Example = PrepareMaterial(ExampleMaterialPath))
	{
		PopulateExampleMaterial(Example, Function, Report);
	}

	UBlueprint* Blueprint = FindExisting<UBlueprint>(ZoneBlueprintPath);
	if (Blueprint)
	{
		if (!Blueprint->ParentClass || !Blueprint->ParentClass->IsChildOf(AGrungifyWeatheringZone::StaticClass()))
		{
			Report.Errors.Add(TEXT("BP_WeatheringZone exists but does not derive from Grungify Weathering Zone"));
		}
	}
	else
	{
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AGrungifyWeatheringZone::StaticClass(), CreatePackage(ZoneBlueprintPath), *AssetName(ZoneBlueprintPath), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("GrungifyContentBuilder"));
		if (Blueprint)
		{
			Register(Blueprint, Packages);
			Report.Created.Add(AssetName(ZoneBlueprintPath));
		}
		else
		{
			Report.Errors.Add(TEXT("Could not create BP_WeatheringZone"));
		}
	}
	if (Blueprint && Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(AGrungifyWeatheringZone::StaticClass()))
	{
		if (bRebuild)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			Touch(Blueprint, Packages);
		}
		if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass)
		{
			Report.Errors.Add(TEXT("BP_WeatheringZone did not compile successfully"));
		}
	}

	UMaterial* Debug = FindExisting<UMaterial>(DebugMaterialPath);
	UMaterial* Example = FindExisting<UMaterial>(ExampleMaterialPath);
	ValidateGeneratedGraphs(Function, Debug, Example, NoMask, Report);
	for (int32 Index = 0; Index < Extras.Num(); ++Index)
	{
		Extras[Index].Validate(ExtraAssets[Index], Report);
		if (ExtraBuilt[Index]) { Extras[Index].CompileCheck(ExtraAssets[Index], Report); }
	}
	// RecompileMaterial marks the package dirty even if the graph is unchanged. A no-work run
	// already authenticated the saved graphs/shader; keep its packages and readiness genuinely clean.
	if (bRebuild && Debug) { CompileAndCheck(Debug, Report); }
	if (bRebuild && Example) { CompileAndCheck(Example, Report); }
	if (Report.Errors.Num() > 0)
	{
		Function->Description = PendingDescription();
		Touch(Function, Packages);
		for (UMaterialFunction* Asset : ExtraAssets) { Asset->Description = PendingDescription(); Touch(Asset, Packages); }
		Report.Notes.Add(TEXT("Validation failed: no generated packages were saved and readiness remains false"));
		return Finish();
	}

	// A current marker is granted only to a validated graph. The disk-byte token is granted only
	// after every requested save succeeds, so even a partial save cannot make NeedsWork return false.
	if (bRebuild)
	{
		Function->Description = PendingDescription().Replace(TEXT("[GrungifyContent pending validation]"), *CurrentMarker());
		Touch(Function, Packages);
	}
	for (int32 Index = 0; Index < Extras.Num(); ++Index)
	{
		if (ExtraBuilt[Index])
		{
			ExtraAssets[Index]->Description = FString(Extras[Index].Description) + CurrentMarker();
			Touch(ExtraAssets[Index], Packages);
		}
	}
	if (Packages.Num() > 0)
	{
		Report.bSaveAttempted = true;
		Report.bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false);
	}
	if (!Report.bSaved)
	{
		Function->Description = PendingDescription();
		Touch(Function, Packages);
		for (UMaterialFunction* Asset : ExtraAssets) { Asset->Description = PendingDescription(); Touch(Asset, Packages); }
		Report.Errors.Add(TEXT("Generated package save failed; readiness remains false"));
		return Finish();
	}
	FString Token;
	if (!MakeReadinessToken(Token) || !FFileHelper::SaveStringToFile(Token, *ReadinessPath()))
	{
		Report.Errors.Add(TEXT("Could not publish saved content readiness token; readiness remains false"));
		return Finish();
	}
	Report.bReady = true;
	return Finish();
}

#undef LOCTEXT_NAMESPACE
