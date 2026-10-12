// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyInspection.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GrungifyBakeRecord.h"
#include "GrungifySettings.h"
#if WITH_EDITOR
#include "GrungifyGenerator.h"
#endif
#include "GrungifyTypes.h"
#include "GrungifyWeatheringZone.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshComponentLODInfo.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	FString PathOf(const UObject* Object)
	{
		return Object ? FSoftObjectPath(Object).ToString() : FString();
	}

	const UObject* MeshAssetOf(const UMeshComponent* Component)
	{
		if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			return Static->GetStaticMesh();
		}
		if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
		{
			return Skinned->GetSkinnedAsset();
		}
		return nullptr;
	}

	bool HasTransientOuter(const UObject* Object)
	{
		// A saved-looking component/package can still belong to a disposable actor,
		// level or world. Inspection follows those owners without changing flags.
		const UObject* Current = Object;
		for (int32 Depth = 0; Current && Depth < 128; ++Depth)
		{
			if (Current == GetTransientPackage() || Current->HasAnyFlags(RF_Transient))
			{
				return true;
			}
			Current = Current->GetOuter();
		}
		// An unresolved ancestry is conservatively reported as nonpersistent.
		return Current != nullptr;
	}

	FGrungifySlotInspection InspectSlot(UMeshComponent* Component, int32 Slot, const TArray<FName>& SlotNames)
	{
		FGrungifySlotInspection Out;
		Out.Slot = Slot;
		Out.SlotName = SlotNames.IsValidIndex(Slot) ? SlotNames[Slot].ToString() : FString();
		UMaterialInterface* Material = Component->GetMaterial(Slot);
		if (!Material)
		{
			Out.Unavailable.Add(TEXT("material: slot has no material"));
			return Out;
		}
		Out.Material = PathOf(Material);
		Out.MaterialClass = Material->GetClass()->GetName();
		if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
		{
			Out.Parent = PathOf(Instance->Parent);
			Out.bOwnedInstance = Instance->GetOuter() == Component;
			Out.bBakedInstance = !Out.bOwnedInstance && Instance->IsAsset()
				&& (Instance->GetAssetUserDataOfClass(UGrungifyBakeRecord::StaticClass()) || Instance->GetName().StartsWith(TEXT("MI_Grungify_")));
			if (Out.bBakedInstance)
			{
				FString Reason;
				UGrungifyInspectionLibrary::CheckBakeCompatibility(Material, Component, Reason);
				Out.BakeCompatibility = Reason;
			}
		}

		float Value = 0.f;
		Out.bHasActiveParameter = Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Grungify::ActiveParameterName()), Value);
		Out.Active = Out.bHasActiveParameter ? Value : 0.f;
		if (!Out.bHasActiveParameter)
		{
			Out.Unavailable.Add(TEXT("Grungify parameters: the material doesn't use Grungify"));
			return Out;
		}
		if (Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Grungify::SurfaceOverrideParameterName()), Value))
		{
			Out.SurfaceOverride = Value;
		}

		UTexture* Mask = nullptr;
		if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Grungify::MaskTextureParameterName()), Mask) && Mask)
		{
			Out.MaskTexture = PathOf(Mask);
			Out.MaskTextureSize = FIntPoint(int32(Mask->GetSurfaceWidth()), int32(Mask->GetSurfaceHeight()));
		}
		UTexture* Extended = nullptr;
		if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Grungify::ExtendedMaskTextureParameterName()), Extended) && Extended)
		{
			Out.ExtendedMaskTexture = PathOf(Extended);
		}

		float UseUV0 = 0.f;
		const bool bHasUseUV0 = Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Grungify Use UV0")), UseUV0);
		Out.EffectiveUVChannel = bHasUseUV0 && UseUV0 >= 0.5f ? 0 : UGrungifySettings::Get()->MaskUVChannel;
		if (!bHasUseUV0)
		{
			Out.Unavailable.Add(TEXT("Grungify Use UV0: parameter absent (content older than v0.6); assuming the settings channel"));
		}
		return Out;
	}
}

FGrungifyComponentInspection UGrungifyInspectionLibrary::InspectComponent(UMeshComponent* Component)
{
	FGrungifyComponentInspection Out;
	if (!IsValid(Component))
	{
		Out.Unavailable.Add(TEXT("component: null or being destroyed"));
		return Out;
	}
	Out.bValid = true;

	// Identity and persistence.
	Out.Component = PathOf(Component);
	Out.ComponentClass = Component->GetClass()->GetName();
	const AActor* Owner = Component->GetOwner();
#if WITH_EDITOR
	Out.Owner = Owner ? Owner->GetActorLabel() : FString();
#else
	Out.Owner = Owner ? Owner->GetName() : FString();
#endif
	Out.Outer = PathOf(Component->GetOuter());
	const UPackage* Package = Component->GetPackage();
	Out.Package = Package ? Package->GetName() : FString();
	Out.bTransient = HasTransientOuter(Component) || !Package || Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient);
	Out.MeshAsset = PathOf(MeshAssetOf(Component));

	// Ownership.
	if (UWorld* World = Component->GetWorld())
	{
		for (TActorIterator<AGrungifyWeatheringZone> It(World); It; ++It)
		{
			if (It->IsPreviewing(Component))
			{
				Out.PreviewedBy = PathOf(*It);
			}
			if (It->FindAppliedRecord(Component))
			{
				Out.AppliedBy = PathOf(*It);
			}
		}
	}
	else
	{
		Out.Unavailable.Add(TEXT("ownership: the component isn't in a world"));
	}

	// Custom primitive data, defaults and live values separately.
	const int32 Base = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
	Out.CustomDataBase = Base;
	Out.DefaultCustomData = Component->GetDefaultCustomPrimitiveData().Data;
	Out.LiveCustomData = Component->GetCustomPrimitiveData().Data;
	Out.MaskMode = Out.LiveCustomData.IsValidIndex(Base + Grungify::CPD_MaskMode) ? Out.LiveCustomData[Base + Grungify::CPD_MaskMode] : 0.f;
	Out.DebugChannel = Out.LiveCustomData.IsValidIndex(Base + Grungify::CPD_DebugChannel) ? Out.LiveCustomData[Base + Grungify::CPD_DebugChannel] : 0.f;
	Out.MaskModeName = Out.MaskMode == Grungify::MaskModeVertex ? TEXT("vertex") : Out.MaskMode == Grungify::MaskModeTexture ? TEXT("texture")
		: Out.MaskMode == Grungify::MaskModeInstanced ? TEXT("instanced") : TEXT("none");

	// Vertex colours (static meshes only).
	if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
	{
		for (const FStaticMeshComponentLODInfo& Info : Static->LODData)
		{
			Out.OverrideVertexColorCounts.Add(Info.OverrideVertexColors ? int32(Info.OverrideVertexColors->GetNumVertices()) : 0);
		}
	}

	// Instances.
	if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
	{
		Out.bInstanced = true;
		Out.InstanceCount = Instanced->GetInstanceCount();
		Out.NumInstanceCustomDataFloats = Instanced->NumCustomDataFloats;
		const int32 InstanceBase = UGrungifySettings::Get()->InstanceCustomDataIndex;
		const int32 Floats = Instanced->NumCustomDataFloats;
		if (Floats >= InstanceBase + Grungify::InstanceDataCount && Instanced->PerInstanceSMCustomData.Num() == Out.InstanceCount * Floats)
		{
			for (int32 Index = 0; Index < Out.InstanceCount; ++Index)
			{
				const float* Data = Instanced->PerInstanceSMCustomData.GetData() + Index * Floats + InstanceBase;
				Out.InstanceMasks.Add(FLinearColor(Data[0], Data[1], Data[2], Data[3]));
			}
		}
		else if (Out.InstanceCount > 0)
		{
			Out.Unavailable.Add(FString::Printf(TEXT("instance masks: %d custom data floats per instance, Grungify's start at %d"), Floats, InstanceBase));
		}
	}

	// Slots.
	const TArray<FName> SlotNames = Component->GetMaterialSlotNames();
	bool bAnyBaked = false;
	for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
	{
		FGrungifySlotInspection& Inspected = Out.Slots.Add_GetRef(InspectSlot(Component, Slot, SlotNames));
		bAnyBaked |= Inspected.bBakedInstance;
	}

	const bool bPreviewed = !Out.PreviewedBy.IsEmpty();
	const bool bApplied = !Out.AppliedBy.IsEmpty();
	Out.State = bPreviewed && bApplied ? TEXT("previewed+applied") : bPreviewed ? TEXT("previewed") : bApplied ? (bAnyBaked ? TEXT("baked") : TEXT("applied")) : TEXT("none");
	return Out;
}

TArray<FGrungifyComponentInspection> UGrungifyInspectionLibrary::InspectZone(AGrungifyWeatheringZone* Zone)
{
	TArray<FGrungifyComponentInspection> Out;
	if (!IsValid(Zone))
	{
		FGrungifyComponentInspection& Missing = Out.AddDefaulted_GetRef();
		Missing.Unavailable.Add(TEXT("zone: null or being destroyed"));
		return Out;
	}

	TSet<const UMeshComponent*> Seen;
	auto AddRecords = [&Out, &Seen](const TArray<FGrungifyComponentRecord>& Records)
	{
		for (const FGrungifyComponentRecord& Record : Records)
		{
			UMeshComponent* Component = Record.Component.Get();
			if (!Component)
			{
				FGrungifyComponentInspection& Unloaded = Out.AddDefaulted_GetRef();
				Unloaded.Component = Record.Component.ToSoftObjectPath().ToString();
				Unloaded.Unavailable.Add(TEXT("component: not loaded (another World Partition cell or an unloaded level); its backup is retained"));
				continue;
			}
			if (!Seen.Contains(Component))
			{
				Seen.Add(Component);
				Out.Add(InspectComponent(Component));
			}
		}
	};
	AddRecords(Zone->GetPreviewRecords());
	AddRecords(Zone->GetAppliedRecords());
	return Out;
}

UObject* UGrungifyInspectionLibrary::ResolveGrungifyObject(const FString& ObjectPath)
{
	if (ObjectPath.IsEmpty())
	{
		return nullptr;
	}
	// ResolveObject only finds objects already in memory, so inspection never triggers a load.
	return FSoftObjectPath(ObjectPath).ResolveObject();
}

bool UGrungifyInspectionLibrary::CheckBakeCompatibility(UMaterialInterface* BakedMaterial, UMeshComponent* Component, FString& OutReason)
{
#if WITH_EDITOR
	if (!BakedMaterial || !IsValid(Component))
	{
		OutReason = TEXT("unknown: no material or component");
		return false;
	}
	const UGrungifyBakeRecord* Record = Cast<UGrungifyBakeRecord>(BakedMaterial->GetAssetUserDataOfClass(UGrungifyBakeRecord::StaticClass()));
	if (!Record)
	{
		OutReason = TEXT("unknown: baked before 0.7, so it has no bake record (re-bake to make it checkable)");
		return false;
	}
	const UObject* Mesh = Grungify::MeshAssetOf(Component);
	if (!Mesh || FSoftObjectPath(Mesh) != Record->SourceMesh)
	{
		OutReason = FString::Printf(TEXT("incompatible: baked for %s, the component draws %s; the masks would land in another mesh's UV layout"),
			*Record->SourceMesh.ToString(), Mesh ? *Mesh->GetPathName() : TEXT("nothing"));
		return false;
	}
	if (Record->SourceMeshBuild != 0 && int64(Grungify::MeshBuildHash(Component)) != Record->SourceMeshBuild)
	{
		OutReason = TEXT("incompatible: the mesh was rebuilt since the bake, so its UVs may have changed; re-bake");
		return false;
	}
	if (Grungify::NumUVChannelsOf(Component) <= Record->UVChannel)
	{
		OutReason = FString::Printf(TEXT("incompatible: the masks are in UV%d, which the mesh doesn't have"), Record->UVChannel);
		return false;
	}
	OutReason = TEXT("compatible");
	return true;
#else
	OutReason = TEXT("unknown: compatibility is checked in the editor only");
	return false;
#endif
}
