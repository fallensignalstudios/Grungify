// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyGenerator.h"

#if WITH_EDITOR

#include "ComponentReregisterContext.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkinnedAssetCommon.h"
#include "StaticMeshComponentLODInfo.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "GrungifySettings.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshResources.h"
#include "UObject/UnrealType.h"

namespace Grungify
{
	namespace
	{
		const TCHAR* MaskInstancePrefix = TEXT("GrungifyMI_");

		/**
		 * Locate either reflected CPD struct by its public getter's address. Per-element setters cannot
		 * truncate arrays; default setters also reset live-only values throughout the other array.
		 */
		FCustomPrimitiveData* FindCustomData(UPrimitiveComponent* Component, const FCustomPrimitiveData& Data)
		{
			const void* Target = &Data;
			for (TFieldIterator<FStructProperty> It(UPrimitiveComponent::StaticClass()); It; ++It)
			{
				if (It->Struct && It->Struct->GetFName() == TEXT("CustomPrimitiveData") && It->ContainerPtrToValuePtr<void>(Component) == Target)
				{
					return It->ContainerPtrToValuePtr<FCustomPrimitiveData>(Component);
				}
			}
			return nullptr;
		}

		void SetLODColors(UStaticMeshComponent* Component, int32 LODIndex, const TArray<FColor>& Colors)
		{
			FStaticMeshComponentLODInfo& Info = Component->LODData[LODIndex];
			if (Info.OverrideVertexColors)
			{
				Info.ReleaseOverrideVertexColorsAndBlock();
			}
			Info.OverrideVertexColors = new FColorVertexBuffer;
			Info.OverrideVertexColors->InitFromColorArray(Colors);
			BeginInitResource(Info.OverrideVertexColors);
		}

		void ReleaseLODColors(UStaticMeshComponent* Component, int32 LODIndex)
		{
			if (Component->LODData.IsValidIndex(LODIndex) && Component->LODData[LODIndex].OverrideVertexColors)
			{
				Component->LODData[LODIndex].ReleaseOverrideVertexColorsAndBlock();
			}
		}

		void EnsureLODData(UStaticMeshComponent* Component, int32 NumLODs)
		{
			if (Component->LODData.Num() < NumLODs)
			{
				Component->SetLODDataCount(uint32(NumLODs), uint32(NumLODs));
			}
		}
	}

	namespace
	{
		/** What a slot shows with no override: the static or skeletal mesh asset's own material. */
		UMaterialInterface* DefaultMaterialOf(const UMeshComponent* Component, int32 MaterialIndex)
		{
			if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
			{
				const UStaticMesh* Mesh = Static->GetStaticMesh();
				return Mesh ? Mesh->GetMaterial(MaterialIndex) : nullptr;
			}
			if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
			{
				const USkinnedAsset* Asset = Skinned->GetSkinnedAsset();
				return Asset && Asset->GetMaterials().IsValidIndex(MaterialIndex) ? Asset->GetMaterials()[MaterialIndex].MaterialInterface.Get() : nullptr;
			}
			return nullptr;
		}
	}

	void CaptureStaticState(UStaticMeshComponent* Component, FGrungifyComponentRecord& OutRecord);
	void CaptureCommonState(UMeshComponent* Component, FGrungifyComponentRecord& OutRecord);
	void RestoreStaticState(UStaticMeshComponent* Component, const FGrungifyComponentRecord& Record, const FStaticMeshRenderData* RenderData);
	bool RestoreRecordTail(UMeshComponent* Component, UInstancedStaticMeshComponent* Instanced, const FGrungifyComponentRecord& Record);

	void CaptureRecord(UMeshComponent* Component, FGrungifyComponentRecord& OutRecord)
	{
		OutRecord = FGrungifyComponentRecord();
		OutRecord.Component = Component;
		OutRecord.MeshAsset = const_cast<UObject*>(MeshAssetOf(Component));

		// Vertex colours, mesh paint and the derived-data key only exist on static meshes.
		if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			CaptureStaticState(Static, OutRecord);
		}
		CaptureCommonState(Component, OutRecord);
	}

	void CaptureStaticState(UStaticMeshComponent* Component, FGrungifyComponentRecord& OutRecord)
	{
		OutRecord.StaticMesh = Component->GetStaticMesh();
		OutRecord.LODDataCount = Component->LODData.Num();
		OutRecord.LODs.SetNum(OutRecord.LODDataCount);

		for (int32 LODIndex = 0; LODIndex < OutRecord.LODDataCount; ++LODIndex)
		{
			const FStaticMeshComponentLODInfo& Info = Component->LODData[LODIndex];
			FGrungifyLODBackup& Backup = OutRecord.LODs[LODIndex];

			if (const FColorVertexBuffer* Existing = Info.OverrideVertexColors)
			{
				Backup.bHadOverride = true;
				Backup.Colors.SetNumUninitialized(Existing->GetNumVertices());
				for (uint32 V = 0; V < Existing->GetNumVertices(); ++V)
				{
					Backup.Colors[V] = Existing->VertexColor(V);
				}
			}

#if WITH_EDITORONLY_DATA
			Backup.PaintedVertices.Reserve(Info.PaintedVertices.Num());
			for (const FPaintedVertex& Painted : Info.PaintedVertices)
			{
				FGrungifyPaintedVertex& Copy = Backup.PaintedVertices.AddDefaulted_GetRef();
				Copy.Position = FVector(Painted.Position.X, Painted.Position.Y, Painted.Position.Z);
				Copy.Normal = FVector4(Painted.Normal.X, Painted.Normal.Y, Painted.Normal.Z, Painted.Normal.W);
				Copy.Color = Painted.Color;
			}
#endif
		}

#if WITH_EDITORONLY_DATA
		OutRecord.StaticMeshDerivedDataKey = Component->StaticMeshDerivedDataKey;
#endif
	}

	void CaptureCommonState(UMeshComponent* Component, FGrungifyComponentRecord& OutRecord)
	{
		OutRecord.CustomData = Component->GetDefaultCustomPrimitiveData().Data;
		OutRecord.LiveCustomData = Component->GetCustomPrimitiveData().Data;
		OutRecord.bHasLiveCustomData = true;
		OutRecord.CustomDataBase = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
		OutRecord.OverrideMaterials = Component->OverrideMaterials;

		if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
		{
			OutRecord.bHasInstanceData = true;
			OutRecord.InstanceCount = Instanced->GetInstanceCount();
			OutRecord.NumInstanceCustomDataFloats = Instanced->NumCustomDataFloats;
			OutRecord.InstanceCustomData = Instanced->PerInstanceSMCustomData;
		}
	}

	bool RestoreRecord(const FGrungifyComponentRecord& Record, bool bTransactional)
	{
		UMeshComponent* Component = Record.Component.Get();
		if (!IsValid(Component))
		{
			return false;
		}
		UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component);
		if (!Record.MeshAsset.IsNull() && Record.MeshAsset.Get() != MeshAssetOf(Component))
		{
			UE_LOG(LogGrungify, Error, TEXT("%s: mesh changed since capture; state and original backup retained."), *Component->GetPathName());
			return false;
		}

		FCustomPrimitiveData* Defaults = FindCustomData(Component, Component->GetDefaultCustomPrimitiveData());
		FCustomPrimitiveData* Live = FindCustomData(Component, Component->GetCustomPrimitiveData());
		if (!Defaults || !Live)
		{
			UE_LOG(LogGrungify, Error, TEXT("%s: could not locate both custom primitive data arrays; state and backup retained."), *Component->GetPathName());
			return false;
		}

		const UStaticMesh* Mesh = Static ? Static->GetStaticMesh() : nullptr;
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if ((Static && !Record.StaticMesh.IsNull() && Record.StaticMesh.Get() != Mesh)
			|| Record.LODs.ContainsByPredicate([RenderData](const FGrungifyLODBackup& Backup)
				{ return Backup.bHadOverride && !RenderData; }))
		{
			UE_LOG(LogGrungify, Error, TEXT("%s: mesh changed since capture; state and original backup retained."), *Component->GetPathName());
			return false;
		}
		for (int32 LODIndex = 0; LODIndex < Record.LODs.Num(); ++LODIndex)
		{
			const FGrungifyLODBackup& Backup = Record.LODs[LODIndex];
			if (Backup.bHadOverride && (!RenderData->LODResources.IsValidIndex(LODIndex)
				|| Backup.Colors.Num() != int32(RenderData->LODResources[LODIndex].GetNumVertices())))
			{
				UE_LOG(LogGrungify, Error, TEXT("%s LOD%d: geometry changed since capture; state and original backup retained."), *Component->GetPathName(), LODIndex);
				return false;
			}
		}

		UInstancedStaticMeshComponent* Instanced = nullptr;
		if (Record.bHasInstanceData)
		{
			Instanced = Cast<UInstancedStaticMeshComponent>(Component);
			const int32 NumFloats = Record.NumInstanceCustomDataFloats;
			// Older nonzero-stride backups encode their original count in the array length.
			const int32 CapturedInstances = Record.InstanceCount != INDEX_NONE ? Record.InstanceCount
				: NumFloats > 0 && Record.InstanceCustomData.Num() % NumFloats == 0
					? Record.InstanceCustomData.Num() / NumFloats : INDEX_NONE;
			if (!Instanced || NumFloats < 0 || CapturedInstances < 0
				|| Instanced->GetInstanceCount() != CapturedInstances
				|| int64(Record.InstanceCustomData.Num()) != int64(CapturedInstances) * NumFloats)
			{
				UE_LOG(LogGrungify, Error, TEXT("%s: instance layout changed or cannot be verified; state and original backup retained."), *Component->GetPathName());
				return false;
			}
		}

		if (bTransactional)
		{
			Component->SetFlags(RF_Transactional);
			Component->Modify();
		}

		{
			// Render state must be torn down while buffers and the material array are swapped.
			FComponentReregisterContext Reregister(Component);

			if (Static)
			{
				RestoreStaticState(Static, Record, RenderData);
			}

			// Use the public material setter for renderer/physics/parameter-cache notifications.
			// Null or absent original overrides resolve to the mesh default, then the raw assignment
			// restores their exact null entries and array length without trimming overlay materials.
			const int32 MaterialCount = FMath::Max(Component->OverrideMaterials.Num(), Record.OverrideMaterials.Num());
			for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
			{
				UMaterialInterface* OriginalMaterial = Record.OverrideMaterials.IsValidIndex(MaterialIndex)
					? Record.OverrideMaterials[MaterialIndex].Get() : nullptr;
				if (!OriginalMaterial)
				{
					OriginalMaterial = DefaultMaterialOf(Component, MaterialIndex);
				}
				Component->SetMaterial(MaterialIndex, OriginalMaterial);
			}
			Component->OverrideMaterials = Record.OverrideMaterials;

			Defaults->Data = Record.CustomData;
			Live->Data = Record.bHasLiveCustomData ? Record.LiveCustomData : Record.CustomData;
			if (!Record.bHasLiveCustomData)
			{
				UE_LOG(LogGrungify, Warning, TEXT("%s: legacy backup has no live CPD snapshot; restoring live data from its original defaults."), *Component->GetPathName());
			}
		}
		return RestoreRecordTail(Component, Instanced, Record);
	}

	void RestoreStaticState(UStaticMeshComponent* Component, const FGrungifyComponentRecord& Record, const FStaticMeshRenderData* RenderData)
	{
		{
			// Release anything we added beyond the original LOD count, then truncate back to it.
			for (int32 LODIndex = Record.LODDataCount; LODIndex < Component->LODData.Num(); ++LODIndex)
			{
				ReleaseLODColors(Component, LODIndex);
			}
			EnsureLODData(Component, Record.LODDataCount);
			if (Component->LODData.Num() > Record.LODDataCount)
			{
				Component->SetLODDataCount(uint32(Record.LODDataCount), uint32(Record.LODDataCount));
			}

			for (int32 LODIndex = 0; LODIndex < Record.LODDataCount && LODIndex < Record.LODs.Num(); ++LODIndex)
			{
				const FGrungifyLODBackup& Backup = Record.LODs[LODIndex];
				const bool bMatchesMesh = RenderData && RenderData->LODResources.IsValidIndex(LODIndex)
					&& Backup.Colors.Num() == int32(RenderData->LODResources[LODIndex].GetNumVertices());

				if (Backup.bHadOverride && bMatchesMesh)
				{
					SetLODColors(Component, LODIndex, Backup.Colors);
				}
				else
				{
					if (Backup.bHadOverride)
					{
						UE_LOG(LogGrungify, Warning, TEXT("%s LOD%d: the mesh changed since its colours were backed up; removing them instead of restoring."), *Component->GetPathName(), LODIndex);
					}
					ReleaseLODColors(Component, LODIndex);
				}

#if WITH_EDITORONLY_DATA
				TArray<FPaintedVertex>& Painted = Component->LODData[LODIndex].PaintedVertices;
				Painted.Reset(Backup.PaintedVertices.Num());
				for (const FGrungifyPaintedVertex& Copy : Backup.PaintedVertices)
				{
					FPaintedVertex& Restored = Painted.AddDefaulted_GetRef();
					Restored.Position.X = Copy.Position.X;
					Restored.Position.Y = Copy.Position.Y;
					Restored.Position.Z = Copy.Position.Z;
					Restored.Normal.X = Copy.Normal.X;
					Restored.Normal.Y = Copy.Normal.Y;
					Restored.Normal.Z = Copy.Normal.Z;
					Restored.Normal.W = Copy.Normal.W;
					Restored.Color = Copy.Color;
				}
#endif
			}

#if WITH_EDITORONLY_DATA
			Component->StaticMeshDerivedDataKey = Record.StaticMeshDerivedDataKey;
#endif
		}
	}

	/** Instanced is the component RestoreRecord verified against the captured instance layout (null otherwise). */
	bool RestoreRecordTail(UMeshComponent* Component, UInstancedStaticMeshComponent* Instanced, const FGrungifyComponentRecord& Record)
	{
		// Instanced meshes: put the per-instance custom data back exactly (count and values).
		if (Instanced)
		{
			const int32 NumFloats = Record.NumInstanceCustomDataFloats;
			Instanced->SetNumCustomDataFloats(NumFloats);
			if (NumFloats > 0)
			{
				for (int32 Index = 0; Index < Instanced->GetInstanceCount(); ++Index)
				{
					Instanced->SetCustomData(Index, TArrayView<const float>(Record.InstanceCustomData.GetData() + Index * NumFloats, NumFloats));
				}
			}
		}

		Component->MarkRenderStateDirty();
		return true;
	}

	bool WriteVertexColors(UStaticMeshComponent* Component, const TArray<TArray<FColor>>& LODColors,
		const TArray<EGrungifySurfaceType>* MaterialSurfaces)
	{
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData)
		{
			return false;
		}

		const int32 NumLODs = FMath::Min(LODColors.Num(), RenderData->LODResources.Num());
		if (NumLODs == 0)
		{
			return false;
		}
		TArray<TArray<FColor>> WrittenColors = LODColors;
		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
			if (WrittenColors[LODIndex].Num() != int32(LOD.GetNumVertices()))
			{
				return false;
			}
			if (MaterialSurfaces)
			{
				TArray<uint8> Usage;
				Usage.SetNumZeroed(LOD.GetNumVertices());
				const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
				for (const FStaticMeshSection& Section : LOD.Sections)
				{
					const bool bAdmitted = MaterialSurfaces->IsValidIndex(Section.MaterialIndex)
						&& (*MaterialSurfaces)[Section.MaterialIndex] != EGrungifySurfaceType::Auto;
					for (uint32 I = Section.FirstIndex; I < Section.FirstIndex + Section.NumTriangles * 3; ++I)
					{
						if (I >= uint32(Indices.Num()) || !Usage.IsValidIndex(Indices[I]))
						{
							return false;
						}
						Usage[Indices[I]] |= bAdmitted ? 1 : 2;
					}
				}
				const FColorVertexBuffer* Previous = Component->LODData.IsValidIndex(LODIndex)
					? Component->LODData[LODIndex].OverrideVertexColors : nullptr;
				for (int32 V = 0; V < Usage.Num(); ++V)
				{
					if (Usage[V] == 3)
					{
						UE_LOG(LogGrungify, Warning, TEXT("%s: admitted/excluded slots share render vertices; use texture masks to preserve excluded paint."), *Component->GetPathName());
						return false;
					}
					if ((Usage[V] & 1) == 0)
					{
						WrittenColors[LODIndex][V] = Previous && Previous->GetNumVertices() == LOD.GetNumVertices()
							? Previous->VertexColor(V)
							: LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() == LOD.GetNumVertices()
								? LOD.VertexBuffers.ColorVertexBuffer.VertexColor(V) : FColor::White;
					}
				}
			}
		}
		FComponentReregisterContext Reregister(Component);
		EnsureLODData(Component, NumLODs);
		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			const TArray<FColor>& Colors = WrittenColors[LODIndex];
			if (Colors.Num() > 0 && Colors.Num() == int32(RenderData->LODResources[LODIndex].GetNumVertices()))
			{
				SetLODColors(Component, LODIndex, Colors);
#if WITH_EDITORONLY_DATA
				// CachePaintedDataIfNecessary deliberately keeps existing caches. These colours need a new cache.
				Component->LODData[LODIndex].PaintedVertices.Reset();
#endif
			}
		}

		// Lets the engine remap the colours if the mesh is reimported. The previous cache and key are in the record.
		Component->StaticMeshDerivedDataKey = RenderData->DerivedDataKey;
		Component->CachePaintedDataIfNecessary();
		return true;
	}

	bool WriteInstanceMasks(UInstancedStaticMeshComponent* Component, const TArray<FVector4f>& Masks)
	{
		const int32 Base = UGrungifySettings::Get()->InstanceCustomDataIndex;
		const int32 Instances = Component->GetInstanceCount();
		if (Masks.Num() != Instances || Base < 0)
		{
			return false;
		}

		// Keep every value another system put there; only our four floats change.
		const int32 OldFloats = Component->NumCustomDataFloats;
		const TArray<float> OldData = Component->PerInstanceSMCustomData;
		const int32 NewFloats = FMath::Max(OldFloats, Base + InstanceDataCount);
		if (NewFloats != OldFloats)
		{
			Component->SetNumCustomDataFloats(NewFloats);
		}

		TArray<float> Values;
		for (int32 Index = 0; Index < Instances; ++Index)
		{
			Values.Init(0.f, NewFloats);
			for (int32 Float = 0; Float < OldFloats && OldData.IsValidIndex(Index * OldFloats + Float); ++Float)
			{
				Values[Float] = OldData[Index * OldFloats + Float];
			}
			const FVector4f& Mask = Masks[Index];
			Values[Base + 0] = Mask.X;
			Values[Base + 1] = Mask.Y;
			Values[Base + 2] = Mask.Z;
			Values[Base + 3] = Mask.W;
			Component->SetCustomData(Index, Values);
		}
		Component->MarkRenderStateDirty();
		return true;
	}

	bool CustomDataSlotsFree(const TArray<float>& OriginalData, const TArray<float>& OriginalLiveData, int32 Base)
	{
		if (Base < 0 || Base > FCustomPrimitiveData::NumCustomPrimitiveDataFloats - CPD_Count)
		{
			return false;
		}
		for (int32 Offset = 0; Offset < CPD_Count; ++Offset)
		{
			if ((OriginalData.IsValidIndex(Base + Offset) && OriginalData[Base + Offset] != 0.f)
				|| (OriginalLiveData.IsValidIndex(Base + Offset) && OriginalLiveData[Base + Offset] != 0.f))
			{
				return false;
			}
		}
		return true;
	}

	bool WriteCustomData(UMeshComponent* Component, const float (&Values)[CPD_Count])
	{
		const int32 Base = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
		FCustomPrimitiveData* Defaults = FindCustomData(Component, Component->GetDefaultCustomPrimitiveData());
		FCustomPrimitiveData* Live = FindCustomData(Component, Component->GetCustomPrimitiveData());
		if (!Defaults || !Live || Base < 0 || Base > FCustomPrimitiveData::NumCustomPrimitiveDataFloats - CPD_Count)
		{
			return false;
		}
		if (Defaults->Data.Num() < Base + CPD_Count)
		{
			Defaults->Data.SetNumZeroed(Base + CPD_Count);
		}
		if (Live->Data.Num() < Base + CPD_Count)
		{
			Live->Data.SetNumZeroed(Base + CPD_Count);
		}
		for (int32 Offset = 0; Offset < CPD_Count; ++Offset)
		{
			Defaults->Data[Base + Offset] = Values[Offset];
			Live->Data[Base + Offset] = Values[Offset];
		}
		Component->MarkRenderStateDirty();
		return true;
	}

	UTexture2D* CreateMaskTexture(UMeshComponent* Component, int32 Size, const TArray<FColor>& Pixels)
	{
		if (Size <= 0 || Pixels.Num() != Size * Size)
		{
			return nullptr;
		}

		// Lives inside the component, so it is saved with the actor and disappears with it.
		const FName Name = MakeUniqueObjectName(Component, UTexture2D::StaticClass(), TEXT("GrungifyMask"));
		UTexture2D* Texture = NewObject<UTexture2D>(Component, Name, RF_Transactional);
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Default;
		Texture->LODGroup = TEXTUREGROUP_World;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->PostEditChange();
		return Texture;
	}

	/** Writes only the ticked palette fields, so everything else keeps inheriting from the mesh's material. */
	static void ApplyPalette(UMaterialInstanceConstant* Instance, const FGrungifyPalette& Palette)
	{
		auto Vector = [Instance](bool bOverride, const TCHAR* Name, const FLinearColor& Value)
		{
			if (bOverride)
			{
				Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
			}
		};
		auto Scalar = [Instance](bool bOverride, const TCHAR* Name, float Value)
		{
			if (bOverride)
			{
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
			}
		};
		Vector(Palette.bOverrideDustColor, TEXT("Grungify Dust Color"), Palette.DustColor);
		Vector(Palette.bOverrideRunoffColor, TEXT("Grungify Runoff Color"), Palette.RunoffColor);
		Vector(Palette.bOverrideTrimChipColor, TEXT("Grungify Trim Chip Color"), Palette.TrimChipColor);
		Scalar(Palette.bOverrideStrength, TEXT("Grungify Strength"), Palette.Strength);
		Scalar(Palette.bOverrideContrast, TEXT("Grungify Contrast"), Palette.Contrast);
		Scalar(Palette.bOverrideChipStrength, TEXT("Grungify Chip Strength"), Palette.ChipStrength);
		Scalar(Palette.bOverrideDustStrength, TEXT("Grungify Dust Strength"), Palette.DustStrength);
		Scalar(Palette.bOverrideRunoffStrength, TEXT("Grungify Runoff Strength"), Palette.RunoffStrength);
		Scalar(Palette.bOverrideHandlingStrength, TEXT("Grungify Handling Strength"), Palette.HandlingStrength);
		Scalar(Palette.bOverrideReliefStrength, TEXT("Grungify Relief Strength"), Palette.ReliefStrength);
		Scalar(Palette.bOverrideCrackStrength, TEXT("Grungify Crack Strength"), Palette.CrackStrength);
		Scalar(Palette.bOverrideMoistureStrength, TEXT("Grungify Moisture Strength"), Palette.MoistureStrength);
		Scalar(Palette.bOverrideSootStrength, TEXT("Grungify Soot Strength"), Palette.SootStrength);
		Scalar(Palette.bOverrideScorchStrength, TEXT("Grungify Scorch Strength"), Palette.ScorchStrength);
		Scalar(Palette.bOverrideMossStrength, TEXT("Grungify Moss Strength"), Palette.MossStrength);
		Vector(Palette.bOverrideMossColor, TEXT("Grungify Moss Color"), Palette.MossColor);
		Vector(Palette.bOverrideSootColor, TEXT("Grungify Soot Color"), Palette.SootColor);
		Vector(Palette.bOverrideEfflorescenceColor, TEXT("Grungify Efflorescence Color"), Palette.EfflorescenceColor);
	}

	void ConfigureOwnedInstance(UMaterialInstanceConstant* Instance, UMaterialInterface* Parent, UTexture2D* Texture, UTexture2D* ExtendedTexture,
		EGrungifySurfaceType Surface, const FGrungifyPalette* Palette, float TexelSizeCm)
	{
		Instance->SetParentEditorOnly(Parent);
		if (Texture)
		{
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(MaskTextureParameterName()), Texture);
		}
		if (ExtendedTexture)
		{
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(ExtendedMaskTextureParameterName()), ExtendedTexture);
		}
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(ActiveParameterName()), 1.f);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(SurfaceOverrideParameterName()), float(int32(Surface) - 1));
		if (TexelSizeCm > 0.f)
		{
			Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(MaskTexelSizeParameterName()), TexelSizeCm);
		}
		if (Palette)
		{
			ApplyPalette(Instance, *Palette);
		}
		Instance->PostEditChange();
	}

	bool InstanceSlotsFree(const FGrungifyComponentRecord& OriginalState)
	{
		const int32 Base = UGrungifySettings::Get()->InstanceCustomDataIndex;
		const int32 Floats = OriginalState.NumInstanceCustomDataFloats;
		if (Base < 0 || !OriginalState.bHasInstanceData || Floats <= Base)
		{
			return Base >= 0;
		}
		for (int32 Start = 0; Start + Floats <= OriginalState.InstanceCustomData.Num(); Start += Floats)
		{
			for (int32 Offset = Base; Offset < FMath::Min(Floats, Base + InstanceDataCount); ++Offset)
			{
				if (OriginalState.InstanceCustomData[Start + Offset] != 0.f)
				{
					return false;
				}
			}
		}
		return true;
	}

	bool BindOwnedMaterials(UMeshComponent* Component, UTexture2D* Texture,
		const TArray<EGrungifySurfaceType>& MaterialSurfaces, UMaterialInterface* ParentOverride,
		const FGrungifyPalette* Palette, UTexture2D* ExtendedTexture, float TexelSizeCm, bool bUseUV0)
	{
		if (MaterialSurfaces.Num() != Component->GetNumMaterials())
		{
			return false;
		}
		// Validate all admitted parents before changing any raw override array.
		for (int32 Slot = 0; Slot < MaterialSurfaces.Num(); ++Slot)
		{
			if (MaterialSurfaces[Slot] != EGrungifySurfaceType::Auto && !ParentOverride && !Component->GetMaterial(Slot))
			{
				return false;
			}
		}
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			if (MaterialSurfaces[Slot] == EGrungifySurfaceType::Auto)
			{
				continue;
			}
			UMaterialInterface* Parent = ParentOverride ? ParentOverride : Component->GetMaterial(Slot);
			if (!Parent)
			{
				continue;
			}

			const FName Name = MakeUniqueObjectName(Component, UMaterialInstanceConstant::StaticClass(), *FString::Printf(TEXT("%s%d"), MaskInstancePrefix, Slot));
			UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Component, Name, RF_Transactional);
			ConfigureOwnedInstance(Instance, Parent, Texture, ExtendedTexture, MaterialSurfaces[Slot], Palette, TexelSizeCm);
			if (bUseUV0)
			{
				// Only this newly-created admitted instance belongs to this write. A preexisting
				// component-outer MIC in an excluded slot must never receive parameter edits.
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grungify Use UV0")), 1.f);
				Instance->PostEditChange();
			}
			Component->SetMaterial(Slot, Instance);
		}
		return true;
	}
}

#endif // WITH_EDITOR
