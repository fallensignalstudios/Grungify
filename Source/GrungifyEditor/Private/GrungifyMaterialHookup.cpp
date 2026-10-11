#include "GrungifyMaterialHookup.h"

#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GrungifySettings.h"
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "HAL/FileManager.h"
#include "IContentBrowserSingleton.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "RHIGlobals.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "GrungifyMaterialHookup"

namespace
{
	const TCHAR* WeatheringFunctionPath = TEXT("/Grungify/Materials/MF_GrungifyWeathering.MF_GrungifyWeathering");
	const TCHAR* SubstrateFunctionPath = TEXT("/Grungify/Materials/MF_GrungifySubstrate.MF_GrungifySubstrate");
	const TCHAR* BackupRoot = TEXT("/Game/Grungify/Backups");

	// Tags in the Desc of every node the tool inserts; Remove only ever touches tagged nodes.
	const TCHAR* CallTag = TEXT("GrungifyHookup call");
	const TCHAR* ConstantTag = TEXT("GrungifyHookup constant");
	const TCHAR* UsedConstantTag = TEXT("GrungifyHookup constant (was Use Constant)");
	const TCHAR* AttributesTag = TEXT("GrungifyHookup attributes");

	enum class EHookupPath : uint8 { Legacy, Attributes, Substrate };

	const TCHAR* PathName(EHookupPath Path)
	{
		return Path == EHookupPath::Legacy ? TEXT("legacy") : Path == EHookupPath::Attributes ? TEXT("attributes") : TEXT("substrate");
	}

	UMaterialFunction* LoadFunction(const TCHAR* Path)
	{
		return LoadObject<UMaterialFunction>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	bool IsGrungifyFunction(const UMaterialFunctionInterface* Function)
	{
		if (!Function)
		{
			return false;
		}
		const FString Path = Function->GetPathName();
		return Path == WeatheringFunctionPath || Path == SubstrateFunctionPath;
	}

	bool IsTagged(const UMaterialExpression* Expression, const TCHAR* Tag)
	{
		return Expression && Expression->Desc.StartsWith(Tag);
	}

	UMaterialExpressionMaterialFunctionCall* FindInsertedCall(UMaterial* Material)
	{
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			if (Call && IsTagged(Call, CallTag) && IsGrungifyFunction(Call->MaterialFunction))
			{
				return Call;
			}
		}
		return nullptr;
	}

	FExpressionInput* FindCallInput(UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name)
	{
		for (FFunctionExpressionInput& Input : Call->FunctionInputs)
		{
			if (Input.ExpressionInput && Input.ExpressionInput->InputName == FName(Name))
			{
				return &Input.Input;
			}
		}
		return nullptr;
	}

	int32 FindCallOutput(const UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name)
	{
		for (int32 Index = 0; Index < Call->FunctionOutputs.Num(); ++Index)
		{
			const FFunctionExpressionOutput& Output = Call->FunctionOutputs[Index];
			if (Output.ExpressionOutput && Output.ExpressionOutput->OutputName == FName(Name))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** Copies only the connection (expression, output index and component mask), never the property's own type. */
	void CopyConnection(FExpressionInput& To, const FExpressionInput& From)
	{
		To.Expression = From.Expression;
		To.OutputIndex = From.OutputIndex;
		To.Mask = From.Mask;
		To.MaskR = From.MaskR;
		To.MaskG = From.MaskG;
		To.MaskB = From.MaskB;
		To.MaskA = From.MaskA;
	}

	void Disconnect(FExpressionInput& Input)
	{
		Input.Expression = nullptr;
		Input.OutputIndex = 0;
		Input.Mask = 0;
		Input.MaskR = Input.MaskG = Input.MaskB = Input.MaskA = 0;
	}

	/** The current legacy adapter cannot retain dormant connections or unconnected pin metadata. */
	template <typename TInput>
	bool HasUnsupportedConnection(const TInput& Input)
	{
		return (Input.UseConstant && Input.Expression)
			|| (!Input.Expression && (Input.OutputIndex != 0 || Input.Mask || Input.MaskR || Input.MaskG || Input.MaskB || Input.MaskA));
	}

	/**
	 * What feeds a legacy property: its connection, or a new tagged constant holding the value the material actually
	 * used (Use Constant, or the engine default when unconnected), so routing through the function changes nothing.
	 * Use Constant itself is only cleared once the property is connected to the function (see ClearUseConstant).
	 */
	template <typename TInput>
	FExpressionInput SourceFor(UMaterial* Material, const TInput& Property, EMaterialProperty Which, const FVector4f& ConstantValue, bool bVector, int32 Y)
	{
		FExpressionInput Source;
		if (Property.Expression && !Property.UseConstant)
		{
			CopyConnection(Source, Property);
			return Source;
		}

		const bool bWasConstant = Property.UseConstant != 0;
		const FVector4f Value = bWasConstant ? ConstantValue : FVector4f(FMaterialAttributeDefinitionMap::GetDefaultValue(Which));
		UMaterialExpression* Constant = nullptr;
		if (bVector)
		{
			UMaterialExpressionConstant3Vector* Vector = Cast<UMaterialExpressionConstant3Vector>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant3Vector::StaticClass(), Material->EditorX - 700, Y));
			if (Vector)
			{
				Vector->Constant = FLinearColor(Value.X, Value.Y, Value.Z);
				Constant = Vector;
			}
		}
		else
		{
			UMaterialExpressionConstant* Scalar = Cast<UMaterialExpressionConstant>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass(), Material->EditorX - 700, Y));
			if (Scalar)
			{
				Scalar->R = Value.X;
				Constant = Scalar;
			}
		}
		if (Constant)
		{
			Constant->Desc = bWasConstant ? UsedConstantTag : ConstantTag;
			Source.Expression = Constant;
		}
		return Source;
	}

	/** Puts a routed property back from the call input that held its original source. */
	template <typename TInput>
	void RestoreProperty(UMaterial* Material, TInput& Property, const FExpressionInput* Source)
	{
		UMaterialExpression* Feed = Source ? Source->Expression : nullptr;
		if (IsTagged(Feed, ConstantTag))
		{
			Property.UseConstant = IsTagged(Feed, UsedConstantTag) ? 1 : 0;
			Disconnect(Property);
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Feed);
		}
		else if (Source)
		{
			CopyConnection(Property, *Source);
		}
		else
		{
			Disconnect(Property);
		}
	}

	bool CompileErrors(UMaterial* Material, FString& OutErrors)
	{
		// RecompileMaterial builds streaming data, which runs CollectGarbage. Retain even transient callers.
		TStrongObjectPtr<UMaterial> KeepMaterialAlive(Material);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
		if (!Resource)
		{
			OutErrors = TEXT("no material resource was available to verify compilation");
			return true;
		}
		Resource->FinishCompilation();
		OutErrors = FString::Join(Resource->GetCompileErrors(), TEXT("; "));
		return !OutErrors.IsEmpty();
	}

	bool IsWritable(const UMaterial* Material, FString& OutWhy)
	{
		const FString Package = Material->GetPackage()->GetName();
		if (Package.StartsWith(TEXT("/Engine/")) || Package.StartsWith(TEXT("/Script/")))
		{
			OutWhy = TEXT("engine content; duplicate it into your project and use the copy");
			return false;
		}
		if (Package.StartsWith(TEXT("/Grungify/")))
		{
			OutWhy = TEXT("Grungify's own content already handles weathering");
			return false;
		}
		if (Material->GetPackage()->HasAnyPackageFlags(PKG_Cooked))
		{
			OutWhy = TEXT("cooked content can't be edited");
			return false;
		}
		return true;
	}

	/** First-time copy of the untouched material; later Adds keep that original backup. */
	FString BackUp(UMaterial* Material)
	{
		FString Relative = Material->GetPackage()->GetName();
		Relative.RemoveFromStart(TEXT("/Game"));
		const FString PackagePath = FString(BackupRoot) + FPackageName::GetLongPackagePath(Relative);
		const FString Name = Material->GetName() + TEXT("_PreGrungify");
		const FString PackageName = PackagePath / Name;
		if (FPackageName::DoesPackageExist(PackageName))
		{
			// Never overwrite a first backup, and do not mistake an unrelated/corrupt asset for one.
			const FString ObjectPath = PackageName + TEXT(".") + Name;
			return LoadObject<UMaterial>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet)
				? PackageName : FString();
		}
		// A previous failed save may have left an unsaved duplicate in memory. It is not a disk backup.
		if (FindPackage(nullptr, *PackageName))
		{
			return FString();
		}
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* Copy = AssetTools.DuplicateAsset(Name, PackagePath, Material);
		if (!Copy)
		{
			return FString();
		}
		TStrongObjectPtr<UObject> KeepCopyAlive(Copy);
		return UEditorLoadingAndSavingUtils::SavePackages({ Copy->GetPackage() }, /*bOnlyDirty*/ false)
			? PackageName : FString();
	}

	void CloseEditors(UMaterial* Material)
	{
		// Material editors keep their own copy of the graph; editing underneath them would be lost or clash.
		if (GEditor)
		{
			if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				Editors->CloseAllEditorsForAsset(Material);
			}
		}
	}

	TArray<FGuid> RoutedAttributeIds()
	{
		return { FMaterialAttributeDefinitionMap::GetID(MP_BaseColor), FMaterialAttributeDefinitionMap::GetID(MP_Roughness),
			FMaterialAttributeDefinitionMap::GetID(MP_Metallic), FMaterialAttributeDefinitionMap::GetID(MP_Normal) };
	}

	bool InsertLegacy(UMaterial* Material, UMaterialEditorOnlyData* Data, UMaterialFunction* Function, FGrungifyHookupResult& Result)
	{
		const int32 X = Material->EditorX - 400;
		const int32 Y = Material->EditorY;
		UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass(), X, Y));
		if (!Call || !Call->SetMaterialFunction(Function))
		{
			Result.Notes = TEXT("could not create the MF_GrungifyWeathering call");
			return false;
		}

		const bool bNormal = Material->bTangentSpaceNormal;
		Call->Desc = FString::Printf(TEXT("%s legacy%s"), CallTag, bNormal ? TEXT(" normal") : TEXT(""));

		// UE 5.8 stores this input constant in linear colour space already.
		const FLinearColor ColorConstant = Data->BaseColor.Constant;
		const FExpressionInput BaseColor = SourceFor(Material, Data->BaseColor, MP_BaseColor, FVector4f(ColorConstant.R, ColorConstant.G, ColorConstant.B, 1.f), true, Y);
		const FExpressionInput Roughness = SourceFor(Material, Data->Roughness, MP_Roughness, FVector4f(Data->Roughness.Constant, 0.f, 0.f, 0.f), false, Y + 150);
		const FExpressionInput Metallic = SourceFor(Material, Data->Metallic, MP_Metallic, FVector4f(Data->Metallic.Constant, 0.f, 0.f, 0.f), false, Y + 250);
		FExpressionInput* CallBaseColor = FindCallInput(Call, TEXT("BaseColor"));
		FExpressionInput* CallRoughness = FindCallInput(Call, TEXT("Roughness"));
		FExpressionInput* CallMetallic = FindCallInput(Call, TEXT("Metallic"));
		FExpressionInput* CallNormal = FindCallInput(Call, TEXT("Normal"));
		if (!CallBaseColor || !CallRoughness || !CallMetallic || !CallNormal)
		{
			Result.Notes = TEXT("MF_GrungifyWeathering is missing an input; run Tools > Grungify > Update Plugin Content");
			return false;
		}
		CopyConnection(*CallBaseColor, BaseColor);
		CopyConnection(*CallRoughness, Roughness);
		CopyConnection(*CallMetallic, Metallic);

		// A connected property ignores nothing while Use Constant is on, so clear it as each one is routed.
		bool bOk = true;
		if (UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("BaseColor"), MP_BaseColor)) { Data->BaseColor.UseConstant = 0; } else { bOk = false; }
		if (UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Roughness"), MP_Roughness)) { Data->Roughness.UseConstant = 0; } else { bOk = false; }
		if (UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Metallic"), MP_Metallic)) { Data->Metallic.UseConstant = 0; } else { bOk = false; }
		if (bNormal)
		{
			// Unconnected normals stay unconnected on the call, whose default is the flat tangent-space normal.
			if (Data->Normal.Expression && !Data->Normal.UseConstant)
			{
				CopyConnection(*CallNormal, Data->Normal);
				bOk = UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Normal"), MP_Normal) && bOk;
			}
			else if (!Data->Normal.Expression && !Data->Normal.UseConstant)
			{
				bOk = UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Normal"), MP_Normal) && bOk;
			}
			else
			{
				Result.Notes += TEXT("the normal uses a constant, so it was left alone and Relief has no effect");
			}
		}
		else
		{
			Result.Notes += TEXT("world-space normal material: the normal was left alone, so Relief has no effect");
		}
		return bOk;
	}

	bool InsertAttributes(UMaterial* Material, UMaterialEditorOnlyData* Data, UMaterialFunction* Function, FGrungifyHookupResult& Result)
	{
		const int32 X = Material->EditorX;
		const int32 Y = Material->EditorY;
		UMaterialExpressionGetMaterialAttributes* Get = Cast<UMaterialExpressionGetMaterialAttributes>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionGetMaterialAttributes::StaticClass(), X - 900, Y));
		UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass(), X - 600, Y));
		UMaterialExpressionSetMaterialAttributes* Set = Cast<UMaterialExpressionSetMaterialAttributes>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSetMaterialAttributes::StaticClass(), X - 300, Y));
		if (!Get || !Call || !Set || !Call->SetMaterialFunction(Function))
		{
			Result.Notes = TEXT("could not create the attribute nodes");
			return false;
		}
		Call->Desc = FString::Printf(TEXT("%s attributes-path"), CallTag);
		Get->Desc = AttributesTag;
		Set->Desc = AttributesTag;

		const TArray<FGuid> Ids = RoutedAttributeIds();
		Get->AttributeGetTypes = Ids;
		if (FProperty* Property = FindFProperty<FProperty>(UMaterialExpressionGetMaterialAttributes::StaticClass(), GET_MEMBER_NAME_CHECKED(UMaterialExpressionGetMaterialAttributes, AttributeGetTypes)))
		{
			FPropertyChangedEvent Event(Property);
			Get->PostEditChangeProperty(Event);
		}
		if (Get->GetOutputs().Num() != Ids.Num() + 1)
		{
			Result.Notes = TEXT("Get Material Attributes did not expose the routed attributes");
			return false;
		}

		// Set node: input 0 is the attribute stream, then one input per attribute type.
		Set->Inputs.SetNum(1);
		for (int32 Index = 0; Index < Ids.Num(); ++Index)
		{
			Set->Inputs.AddDefaulted();
		}
		Set->AttributeSetTypes = Ids;

		CopyConnection(Get->MaterialAttributes, Data->MaterialAttributes);
		CopyConnection(Set->Inputs[0], Data->MaterialAttributes);

		const TCHAR* Names[] = { TEXT("BaseColor"), TEXT("Roughness"), TEXT("Metallic"), TEXT("Normal") };
		for (int32 Index = 0; Index < Ids.Num(); ++Index)
		{
			FExpressionInput* CallInput = FindCallInput(Call, Names[Index]);
			const int32 CallOutput = FindCallOutput(Call, Names[Index]);
			if (!CallInput || CallOutput == INDEX_NONE)
			{
				Result.Notes = TEXT("MF_GrungifyWeathering is missing an input or output; run Tools > Grungify > Update Plugin Content");
				return false;
			}
			CallInput->Connect(Index + 1, Get);
			Set->Inputs[Index + 1].Connect(CallOutput, Call);
		}
		Material->Modify();
		Data->MaterialAttributes.Connect(0, Set);
		return true;
	}

	bool InsertSubstrate(UMaterial* Material, UMaterialEditorOnlyData* Data, UMaterialFunction* Function, FGrungifyHookupResult& Result)
	{
		UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass(), Material->EditorX - 400, Material->EditorY));
		if (!Call || !Call->SetMaterialFunction(Function))
		{
			Result.Notes = TEXT("could not create the MF_GrungifySubstrate call");
			return false;
		}
		Call->Desc = FString::Printf(TEXT("%s substrate"), CallTag);
		FExpressionInput* CallMaterial = FindCallInput(Call, TEXT("Material"));
		FExpressionInput* CallNormal = FindCallInput(Call, TEXT("Normal"));
		if (!CallMaterial || !CallNormal)
		{
			Result.Notes = TEXT("MF_GrungifySubstrate is missing an input; run Tools > Grungify > Update Plugin Content");
			return false;
		}
		CopyConnection(*CallMaterial, Data->FrontMaterial);
		// The grime should follow the surface's own normal detail; take it from the slab feeding Front Material, if that's what it is.
		if (const UMaterialExpressionSubstrateSlabBSDF* Slab = Cast<UMaterialExpressionSubstrateSlabBSDF>(Data->FrontMaterial.Expression))
		{
			if (Slab->Normal.Expression)
			{
				CopyConnection(*CallNormal, Slab->Normal);
			}
		}
		else
		{
			Result.Notes += TEXT("Front Material isn't a single slab, so the grime layer uses the vertex normal");
		}
		Material->Modify();
		return UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Material"), MP_FrontMaterial);
	}

	/** Undoes whatever the tool inserted. Returns false if nothing tagged was found. */
	bool RemoveInserted(UMaterial* Material, FGrungifyHookupResult& Result)
	{
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		UMaterialExpressionMaterialFunctionCall* Call = FindInsertedCall(Material);
		if (!Data || !Call)
		{
			return false;
		}

		if (Call->Desc.Contains(TEXT("substrate")))
		{
			Result.Path = PathName(EHookupPath::Substrate);
			if (Data->FrontMaterial.Expression == Call)
			{
				const FExpressionInput* Source = FindCallInput(Call, TEXT("Material"));
				Source ? CopyConnection(Data->FrontMaterial, *Source) : Disconnect(Data->FrontMaterial);
			}
		}
		else if (Call->Desc.Contains(TEXT("attributes-path")))
		{
			Result.Path = PathName(EHookupPath::Attributes);
			TArray<UMaterialExpression*> Tagged;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (IsTagged(Expression, AttributesTag))
				{
					Tagged.Add(Expression);
				}
			}
			for (UMaterialExpression* Expression : Tagged)
			{
				UMaterialExpressionSetMaterialAttributes* Set = Cast<UMaterialExpressionSetMaterialAttributes>(Expression);
				if (Set && Data->MaterialAttributes.Expression == Set && Set->Inputs.Num() > 0)
				{
					CopyConnection(Data->MaterialAttributes, Set->Inputs[0]);
				}
			}
			for (UMaterialExpression* Expression : Tagged)
			{
				UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
			}
		}
		else
		{
			Result.Path = PathName(EHookupPath::Legacy);
			if (Data->BaseColor.Expression == Call) { RestoreProperty(Material, Data->BaseColor, FindCallInput(Call, TEXT("BaseColor"))); }
			if (Data->Roughness.Expression == Call) { RestoreProperty(Material, Data->Roughness, FindCallInput(Call, TEXT("Roughness"))); }
			if (Data->Metallic.Expression == Call) { RestoreProperty(Material, Data->Metallic, FindCallInput(Call, TEXT("Metallic"))); }
			if (Data->Normal.Expression == Call) { RestoreProperty(Material, Data->Normal, FindCallInput(Call, TEXT("Normal"))); }
			// Constants the tool made for properties that weren't routed after all.
			TArray<UMaterialExpression*> Leftovers;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (IsTagged(Expression, ConstantTag)) { Leftovers.Add(Expression); }
			}
			for (UMaterialExpression* Expression : Leftovers)
			{
				UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
			}
		}
		UMaterialEditingLibrary::DeleteMaterialExpression(Material, Call);
		return true;
	}

	UMaterial* ResolveBase(UMaterialInterface* Material)
	{
		return Material ? Material->GetMaterial() : nullptr;
	}

	void Notify(const FText& Message, bool bError)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bError ? 8.f : 5.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	/** Runs Add or Remove over a set of materials with progress, then reports. */
	void RunBatch(const TArray<UMaterial*>& Materials, bool bAdd)
	{
		if (Materials.Num() == 0)
		{
			Notify(LOCTEXT("NothingSelected", "Grungify: no materials to change. Select materials or material instances in the Content Browser."), false);
			return;
		}

		TArray<FGrungifyHookupResult> Results;
		{
			FScopedSlowTask Task(float(Materials.Num()), bAdd ? LOCTEXT("Adding", "Grungify: hooking up materials...") : LOCTEXT("Removing", "Grungify: removing from materials..."));
			Task.MakeDialogDelayed(0.3f);
			for (UMaterial* Material : Materials)
			{
				Task.EnterProgressFrame(1.f, FText::FromString(Material->GetName()));
				Results.Add(bAdd ? UGrungifyMaterialHookup::AddGrungifyToMaterial(Material, true) : UGrungifyMaterialHookup::RemoveGrungifyFromMaterial(Material));
			}
		}

		TMap<FString, int32> Counts;
		TArray<FString> Lines;
		for (const FGrungifyHookupResult& Result : Results)
		{
			Counts.FindOrAdd(Result.Outcome)++;
			Lines.Add(FString::Printf(TEXT("%-8s %-10s %s%s%s"), *Result.Outcome, *Result.Path, *Result.Material,
				Result.Notes.IsEmpty() ? TEXT("") : *(TEXT("  - ") + Result.Notes),
				Result.Backup.IsEmpty() ? TEXT("") : *(TEXT("  (backup ") + Result.Backup + TEXT(")"))));
			UE_LOG(LogGrungify, Log, TEXT("Material hookup: %s"), *Lines.Last());
		}
		const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"), TEXT("MaterialHookup.txt"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")), *File);

		const int32 Failed = Counts.FindRef(TEXT("failed"));
		const FString Summary = bAdd
			? FString::Printf(TEXT("Grungify: hooked up %d material(s), %d already used it, %d skipped, %d failed. Save them to keep the change. Details: Saved/Grungify/MaterialHookup.txt"),
				Counts.FindRef(TEXT("added")), Counts.FindRef(TEXT("already")), Counts.FindRef(TEXT("skipped")), Failed)
			: FString::Printf(TEXT("Grungify: removed from %d material(s), %d skipped, %d failed. Save them to keep the change. Details: Saved/Grungify/MaterialHookup.txt"),
				Counts.FindRef(TEXT("removed")), Counts.FindRef(TEXT("skipped")), Failed);
		Notify(FText::FromString(Summary), Failed > 0);
	}

	TArray<UMaterial*> SelectedBaseMaterials()
	{
		TArray<FAssetData> Assets;
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().GetSelectedAssets(Assets);
		TArray<UMaterial*> Materials;
		for (const FAssetData& Asset : Assets)
		{
			if (UMaterial* Base = ResolveBase(Cast<UMaterialInterface>(Asset.GetAsset())))
			{
				Materials.AddUnique(Base);
			}
		}
		return Materials;
	}
}

bool UGrungifyMaterialHookup::UsesGrungify(const UMaterialInterface* Material)
{
	float Value = 0.f;
	return Material && Material->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(Grungify::ActiveParameterName()), Value);
}

FGrungifyHookupResult UGrungifyMaterialHookup::AddGrungifyToMaterial(UMaterialInterface* MaterialInterface, bool bBackup)
{
	FGrungifyHookupResult Result;
	UMaterial* Material = ResolveBase(MaterialInterface);
	if (!Material)
	{
		Result.Outcome = TEXT("failed");
		Result.Notes = TEXT("no material");
		return Result;
	}
	TStrongObjectPtr<UMaterial> KeepMaterialAlive(Material);
	Result.Material = Material->GetPathName();
	if (MaterialInterface != Material)
	{
		Result.Notes = FString::Printf(TEXT("changed the parent of %s, so all its instances get weathering; "), *MaterialInterface->GetName());
	}

	auto Skip = [&Result](const FString& Why)
	{
		Result.Outcome = TEXT("skipped");
		Result.Notes += Why;
		return Result;
	};

	if (UsesGrungify(Material))
	{
		Result.Outcome = TEXT("already");
		return Result;
	}
	FString Why;
	if (!IsWritable(Material, Why))
	{
		return Skip(Why);
	}
	if (Material->MaterialDomain != MD_Surface)
	{
		return Skip(TEXT("not a surface material (decals, post process and UI aren't weathered)"));
	}
	if (Material->GetShadingModels().IsUnlit())
	{
		return Skip(TEXT("unlit materials have no roughness or metallic to weather"));
	}
	UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
	if (!Data)
	{
		return Skip(TEXT("no editor data (cooked?)"));
	}

	EHookupPath Path = EHookupPath::Legacy;
	if (Data->FrontMaterial.IsConnected())
	{
		Path = EHookupPath::Substrate;
	}
	else if (Material->bUseMaterialAttributes)
	{
		Path = EHookupPath::Attributes;
		if (!Data->MaterialAttributes.IsConnected())
		{
			return Skip(TEXT("Use Material Attributes is on but nothing is connected"));
		}
	}
	Result.Path = PathName(Path);
	if (Path == EHookupPath::Legacy && (HasUnsupportedConnection(Data->BaseColor) || HasUnsupportedConnection(Data->Roughness)
		|| HasUnsupportedConnection(Data->Metallic) || HasUnsupportedConnection(Data->Normal)))
	{
		return Skip(TEXT("dormant Use Constant wiring or unconnected pin metadata cannot be restored exactly; material left unchanged"));
	}
	if (Path == EHookupPath::Attributes && !Material->bTangentSpaceNormal)
	{
		return Skip(TEXT("world-space normal attributes are unsupported by the tangent-space weathering function; material left unchanged"));
	}

	UMaterialFunction* Function = LoadFunction(Path == EHookupPath::Substrate ? SubstrateFunctionPath : WeatheringFunctionPath);
	if (!Function)
	{
		Result.Outcome = TEXT("failed");
		Result.Notes += TEXT("Grungify's material functions are missing; run Tools > Grungify > Update Plugin Content");
		return Result;
	}

	if (bBackup)
	{
		Result.Backup = BackUp(Material);
		if (Result.Backup.IsEmpty())
		{
			Result.Outcome = TEXT("failed");
			Result.Notes += TEXT("could not create and save a backup copy, so the material was left alone");
			return Result;
		}
	}

	CloseEditors(Material);
	FScopedTransaction Transaction(LOCTEXT("AddGrungifyTransaction", "Add Grungify to Material"));
	Material->Modify();
	bool bInserted = false;
	switch (Path)
	{
	case EHookupPath::Legacy: bInserted = InsertLegacy(Material, Data, Function, Result); break;
	case EHookupPath::Attributes: bInserted = InsertAttributes(Material, Data, Function, Result); break;
	case EHookupPath::Substrate: bInserted = InsertSubstrate(Material, Data, Function, Result); break;
	}

	FString Errors;
	if (!bInserted || CompileErrors(Material, Errors))
	{
		// Never leave a half-wired or broken material behind.
		FGrungifyHookupResult Ignored;
		RemoveInserted(Material, Ignored);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		Transaction.Cancel();
		Result.Outcome = TEXT("failed");
		Result.Notes += bInserted ? FString::Printf(TEXT("the hooked-up material didn't compile (%s); change reverted"), *Errors) : TEXT("; change reverted");
		return Result;
	}

	Material->MarkPackageDirty();
	Result.Outcome = TEXT("added");
	return Result;
}

FGrungifyHookupResult UGrungifyMaterialHookup::RemoveGrungifyFromMaterial(UMaterialInterface* MaterialInterface)
{
	FGrungifyHookupResult Result;
	UMaterial* Material = ResolveBase(MaterialInterface);
	if (!Material)
	{
		Result.Outcome = TEXT("failed");
		Result.Notes = TEXT("no material");
		return Result;
	}
	TStrongObjectPtr<UMaterial> KeepMaterialAlive(Material);
	Result.Material = Material->GetPathName();
	FString Why;
	if (!IsWritable(Material, Why))
	{
		Result.Outcome = TEXT("skipped");
		Result.Notes = Why;
		return Result;
	}

	CloseEditors(Material);
	FScopedTransaction Transaction(LOCTEXT("RemoveGrungifyTransaction", "Remove Grungify from Material"));
	Material->Modify();
	// DeleteMaterialExpression also clears incoming links. Snapshot existing nodes for a complete undo.
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (Expression) { Expression->Modify(); }
	}
	if (!RemoveInserted(Material, Result))
	{
		Transaction.Cancel();
		Result.Outcome = TEXT("skipped");
		Result.Notes = UsesGrungify(Material)
			? TEXT("Grungify was wired in by hand, not by this tool; remove it in the material editor")
			: TEXT("doesn't use Grungify");
		return Result;
	}
	FString Errors;
	if (CompileErrors(Material, Errors))
	{
		Result.Notes = FString::Printf(TEXT("nodes removed, but compilation could not be verified (%s); use Undo to restore the hookup"), *Errors);
		Material->MarkPackageDirty();
		Result.Outcome = TEXT("failed");
		return Result;
	}
	Material->MarkPackageDirty();
	Result.Outcome = TEXT("removed");
	return Result;
}

TArray<UMaterial*> UGrungifyMaterialHookup::FindUnhookedMaterialsInLevel()
{
	TArray<UMaterial*> Materials;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return Materials;
	}

	TArray<FBox> Zones;
	for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
	{
		if (const UGrungifyZoneComponent* Zone = It->GetZoneComponent())
		{
			Zones.Add(Zone->Bounds.GetBox());
		}
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TInlineComponentArray<UStaticMeshComponent*> Components(*It);
		for (const UStaticMeshComponent* Component : Components)
		{
			if (!Component || !Component->IsRegistered())
			{
				continue;
			}
			const FBox Box = Component->Bounds.GetBox();
			if (Zones.Num() > 0 && !Zones.ContainsByPredicate([&Box](const FBox& Zone) { return Zone.Intersect(Box); }))
			{
				continue;
			}
			for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
			{
				UMaterial* Base = ResolveBase(Component->GetMaterial(Slot));
				FString Why;
				if (Base && !UsesGrungify(Base) && IsWritable(Base, Why) && Base->MaterialDomain == MD_Surface && !Base->GetShadingModels().IsUnlit())
				{
					Materials.AddUnique(Base);
				}
			}
		}
	}
	return Materials;
}

void UGrungifyMaterialHookup::AddToSelectedAssets()
{
	RunBatch(SelectedBaseMaterials(), /*bAdd*/ true);
}

void UGrungifyMaterialHookup::RemoveFromSelectedAssets()
{
	RunBatch(SelectedBaseMaterials(), /*bAdd*/ false);
}

void UGrungifyMaterialHookup::HookUpLevel()
{
	const TArray<UMaterial*> Materials = FindUnhookedMaterialsInLevel();
	if (Materials.Num() == 0)
	{
		Notify(LOCTEXT("LevelHooked", "Grungify: every editable material in the level (inside zones, if any) already uses Grungify."), false);
		return;
	}

	TArray<FString> Names;
	for (int32 Index = 0; Index < Materials.Num() && Index < 15; ++Index)
	{
		Names.Add(TEXT("  ") + Materials[Index]->GetPathName());
	}
	if (Materials.Num() > 15)
	{
		Names.Add(FString::Printf(TEXT("  ...and %d more"), Materials.Num() - 15));
	}
	const FText Question = FText::Format(LOCTEXT("ConfirmLevel",
		"Hook up {0} master material(s) so they show Grungify weathering?\n\n{1}\n\nEach is backed up to {2} first, and every instance of it is affected. "
		"Remove Grungify (right-click the material) puts the original wiring back."),
		Materials.Num(), FText::FromString(FString::Join(Names, TEXT("\n"))), FText::FromString(BackupRoot));
	if (FMessageDialog::Open(EAppMsgType::OkCancel, Question) == EAppReturnType::Ok)
	{
		RunBatch(Materials, /*bAdd*/ true);
	}
}

#undef LOCTEXT_NAMESPACE
