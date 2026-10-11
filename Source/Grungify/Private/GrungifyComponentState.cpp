#include "GrungifyGenerator.h"

#if WITH_EDITOR

#include "ComponentReregisterContext.h"
#include "Components/StaticMeshComponent.h"
#include "Components/StaticMeshComponentLODInfo.h"
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
		 * The default custom primitive data struct on the component, found by reflection so the whole array
		 * (including its length) can be restored. SetDefaultCustomPrimitiveDataFloat can only grow it.
		 */
		FCustomPrimitiveData* FindDefaultCustomData(UPrimitiveComponent* Component)
		{
			const void* Target = &Component->GetDefaultCustomPrimitiveData();
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

	void CaptureRecord(UStaticMeshComponent* Component, FGrungifyComponentRecord& OutRecord)
	{
		OutRecord = FGrungifyComponentRecord();
		OutRecord.Component = Component;
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
		OutRecord.CustomData = Component->GetDefaultCustomPrimitiveData().Data;
		OutRecord.OverrideMaterials = Component->OverrideMaterials;
	}

	void RestoreRecord(const FGrungifyComponentRecord& Record, bool bTransactional)
	{
		UStaticMeshComponent* Component = Record.Component.Get();
		if (!IsValid(Component))
		{
			return;
		}

		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;

		if (bTransactional)
		{
			Component->SetFlags(RF_Transactional);
			Component->Modify();
		}

		{
			// Render state must be torn down while buffers and the material array are swapped.
			FComponentReregisterContext Reregister(Component);

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

			// Exact array, including its length.
			Component->OverrideMaterials = Record.OverrideMaterials;

			if (FCustomPrimitiveData* Defaults = FindDefaultCustomData(Component))
			{
				Defaults->Data = Record.CustomData;
			}
			else
			{
				UE_LOG(LogGrungify, Warning, TEXT("%s: could not locate default custom primitive data; restoring values only."), *Component->GetPathName());
				for (int32 Index = 0; Index < Record.CustomData.Num(); ++Index)
				{
					Component->SetDefaultCustomPrimitiveDataFloat(Index, Record.CustomData[Index]);
				}
			}
		}

		// The live (non-saved) copy is only refreshed from the defaults on load, so clear our slots in it now.
		// Writing the restored values (zero beyond the original length) turns the material path off immediately.
		const int32 Base = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
		const TArray<float>& Live = Component->GetCustomPrimitiveData().Data;
		for (int32 Offset = 0; Offset < CPD_Count; ++Offset)
		{
			const int32 Index = Base + Offset;
			if (Live.IsValidIndex(Index))
			{
				Component->SetCustomPrimitiveDataFloat(Index, Record.CustomData.IsValidIndex(Index) ? Record.CustomData[Index] : 0.f);
			}
		}

		Component->MarkRenderStateDirty();
	}

	void WriteVertexColors(UStaticMeshComponent* Component, const TArray<TArray<FColor>>& LODColors)
	{
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData)
		{
			return;
		}

		FComponentReregisterContext Reregister(Component);
		const int32 NumLODs = FMath::Min(LODColors.Num(), RenderData->LODResources.Num());
		EnsureLODData(Component, NumLODs);
		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			const TArray<FColor>& Colors = LODColors[LODIndex];
			if (Colors.Num() > 0 && Colors.Num() == int32(RenderData->LODResources[LODIndex].GetNumVertices()))
			{
				SetLODColors(Component, LODIndex, Colors);
			}
		}

		// Lets the engine remap the colours if the mesh is reimported. The previous cache and key are in the record.
		Component->StaticMeshDerivedDataKey = RenderData->DerivedDataKey;
		Component->CachePaintedDataIfNecessary();
	}

	bool CustomDataSlotsFree(const TArray<float>& OriginalData)
	{
		const int32 Base = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
		for (int32 Offset = 0; Offset < CPD_Count; ++Offset)
		{
			if (OriginalData.IsValidIndex(Base + Offset) && OriginalData[Base + Offset] != 0.f)
			{
				return false;
			}
		}
		return true;
	}

	void WriteCustomData(UStaticMeshComponent* Component, const float (&Values)[CPD_Count])
	{
		const int32 Base = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
		for (int32 Offset = 0; Offset < CPD_Count; ++Offset)
		{
			Component->SetDefaultCustomPrimitiveDataFloat(Base + Offset, Values[Offset]);
		}
	}

	UTexture2D* CreateMaskTexture(UStaticMeshComponent* Component, int32 Size, const TArray<FColor>& Pixels)
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

	void BindMaskTexture(UStaticMeshComponent* Component, UTexture2D* Texture, UMaterialInterface* ParentOverride)
	{
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			UMaterialInterface* Parent = ParentOverride ? ParentOverride : Component->GetMaterial(Slot);
			if (!Parent)
			{
				continue;
			}

			const FName Name = MakeUniqueObjectName(Component, UMaterialInstanceConstant::StaticClass(), *FString::Printf(TEXT("%s%d"), MaskInstancePrefix, Slot));
			UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Component, Name, RF_Transactional);
			Instance->SetParentEditorOnly(Parent);
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(MaskTextureParameterName()), Texture);
			Instance->PostEditChange();
			Component->SetMaterial(Slot, Instance);
		}
	}

	void SwapMaterials(UStaticMeshComponent* Component, UMaterialInterface* Material)
	{
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			Component->SetMaterial(Slot, Material);
		}
	}
}

#endif // WITH_EDITOR
