// Copyright Fallen Signal Studios. All Rights Reserved.
// The public runtime hit path end to end: a real complex trace against cooked collision with UVs, AddWearAtHit, and
// GPU readback. Proves the effective mask UV channel (Grungify Use UV0) and per-axis wrap/clamp addressing.
#include "GrungifyRuntimeWear.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GrungifySettings.h"
#include "MaterialEditingLibrary.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "RenderingThread.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace GrungifyRuntimeHitTests
{
	constexpr int32 MaskSize = 64;
	constexpr double QuadCm = 200.;

	/**
	 * A 2 m x 2 m quad at Z = 0 facing up. UV0 runs -0.5..1.5 on both axes (so hits land outside 0..1);
	 * UV1 runs 0..1. Complex collision only, cooked with the project's UV-from-hit setting in force.
	 */
	UStaticMesh* MakeQuad()
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		UVs.SetNumChannels(2);
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = FName(TEXT("Slot0"));

		TArray<FVertexInstanceID> Corners;
		for (const FVector2f Corner : { FVector2f(0.f, 0.f), FVector2f(1.f, 0.f), FVector2f(1.f, 1.f), FVector2f(0.f, 1.f) })
		{
			const FVertexID Vertex = Description.CreateVertex();
			Positions[Vertex] = FVector3f(Corner.X * float(QuadCm), Corner.Y * float(QuadCm), 0.f);
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
			UVs.Set(Instance, 0, Corner * 2.f - FVector2f(0.5f, 0.5f));
			UVs.Set(Instance, 1, Corner);
			Normals[Instance] = FVector3f::UpVector;
			Corners.Add(Instance);
		}
		Description.CreateTriangle(Group, TArray<FVertexInstanceID>{ Corners[0], Corners[2], Corners[1] });
		Description.CreateTriangle(Group, TArray<FVertexInstanceID>{ Corners[0], Corners[3], Corners[2] });

		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), FName(TEXT("Slot0")), FName(TEXT("Slot0"))));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bBuildSimpleCollision = false;
		Params.bAllowCpuAccess = true;
		Mesh->BuildFromMeshDescriptions({ &Description }, Params);
		Mesh->CreateBodySetup();
		UBodySetup* Body = Mesh->GetBodySetup();
		Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
		Body->bDoubleSidedGeometry = true;
		Body->InvalidatePhysicsData();
		Body->CreatePhysicsMeshes();
		return Mesh;
	}

	UTexture2D* MakeMask(TextureAddress AddressX, TextureAddress AddressY)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(MaskSize, MaskSize, PF_B8G8R8A8);
		if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
		{
			return nullptr;
		}
		Texture->SRGB = false;
		Texture->Filter = TF_Nearest;
		Texture->AddressX = AddressX;
		Texture->AddressY = AddressY;
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		FMemory::Memzero(Mip.BulkData.Lock(LOCK_READ_WRITE), MaskSize * MaskSize * sizeof(FColor));
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		return Texture;
	}

	/** A declared, connected material and constant instance: a MID cannot parent the runtime's new MID. */
	UMaterialInstanceConstant* MakeMaterial(UObject* Outer, UTexture2D* Mask, FAutomationTestBase& Test)
	{
		UMaterial* Parent = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		TStrongObjectPtr<UMaterial> ParentGuard(Parent);
		auto Scalar = [Parent](FName Name, float Default)
		{
			UMaterialExpressionScalarParameter* Node = CastChecked<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionScalarParameter::StaticClass()));
			Node->ParameterName = Name;
			Node->DefaultValue = Default;
			return Node;
		};
		UMaterialExpressionScalarParameter* Active = Scalar(Grungify::ActiveParameterName(), 0.f);
		UMaterialExpressionScalarParameter* TexelSize = Scalar(Grungify::MaskTexelSizeParameterName(), 1.f);
		UMaterialExpressionScalarParameter* UseUV0 = Scalar(TEXT("Grungify Use UV0"), 0.f);
		UMaterialExpressionTextureSampleParameter2D* Texture = CastChecked<UMaterialExpressionTextureSampleParameter2D>(
			UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionTextureSampleParameter2D::StaticClass()));
		Texture->ParameterName = Grungify::MaskTextureParameterName();
		Texture->Texture = Mask;
		Texture->SamplerType = SAMPLERTYPE_LinearColor;
		UMaterialExpressionTextureCoordinate* UV0 = CastChecked<UMaterialExpressionTextureCoordinate>(
			UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionTextureCoordinate::StaticClass()));
		UMaterialExpressionTextureCoordinate* UV1 = CastChecked<UMaterialExpressionTextureCoordinate>(
			UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionTextureCoordinate::StaticClass()));
		UV0->CoordinateIndex = 0;
		UV1->CoordinateIndex = 1;
		UMaterialExpressionLinearInterpolate* Coordinates = CastChecked<UMaterialExpressionLinearInterpolate>(
			UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionLinearInterpolate::StaticClass()));
		UMaterialExpressionMultiply* Color = CastChecked<UMaterialExpressionMultiply>(
			UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionMultiply::StaticClass()));
		auto Connect = [&Test](UMaterialExpression* From, const TCHAR* Output, UMaterialExpression* To, const TCHAR* Input)
		{
			const bool bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(From, Output, To, Input);
			if (!bConnected)
			{
				const TArray<FString> Inputs = UMaterialEditingLibrary::GetMaterialExpressionInputNames(To);
				Test.AddError(FString::Printf(TEXT("Fixture connection failed: %s.%s -> %s.%s; available inputs: [%s]"),
					*From->GetClass()->GetName(), Output, *To->GetClass()->GetName(), Input, *FString::Join(Inputs, TEXT(", "))));
			}
			return bConnected;
		};
		auto Property = [&Test](UMaterialExpression* From, EMaterialProperty Which, const TCHAR* Name)
		{
			const bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(From, TEXT(""), Which);
			if (!bConnected)
			{
				Test.AddError(FString::Printf(TEXT("Fixture property connection failed: %s -> %s"), *From->GetClass()->GetName(), Name));
			}
			return bConnected;
		};
		bool bConnected = Connect(UV1, TEXT(""), Coordinates, TEXT("A"));
		bConnected &= Connect(UV0, TEXT(""), Coordinates, TEXT("B"));
		bConnected &= Connect(UseUV0, TEXT(""), Coordinates, TEXT("Alpha"));
		// MaterialEditingLibrary compares editor pin names: GetShortenPinName maps Coordinates to UVs.
		bConnected &= Connect(Coordinates, TEXT(""), Texture, TEXT("UVs"));
		bConnected &= Connect(Texture, TEXT("RGB"), Color, TEXT("A"));
		bConnected &= Connect(Active, TEXT(""), Color, TEXT("B"));
		bConnected &= Property(Color, MP_BaseColor, TEXT("BaseColor"));
		bConnected &= Property(TexelSize, MP_Roughness, TEXT("Roughness"));
		if (!bConnected)
		{
			return nullptr;
		}
		Parent->PostEditChange();
		UMaterialEditingLibrary::RecompileMaterial(Parent);
		UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(Outer, NAME_None, RF_Transient);
		Material->SetParentEditorOnly(Parent, false);
		Material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Grungify::ActiveParameterName()), 1.f);
		Material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Grungify::MaskTexelSizeParameterName()), 1.f);
		Material->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Grungify::MaskTextureParameterName()), Mask);
		Material->PostEditChange();
		return Material;
	}

	/** The painted texel with the most green (Dust), and the total number of painted texels. */
	FIntPoint Peak(const TArray<FColor>& Pixels, int32& OutPainted)
	{
		FIntPoint Best(INDEX_NONE, INDEX_NONE);
		uint8 BestValue = 0;
		OutPainted = 0;
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			OutPainted += Pixels[Index].G > 0 ? 1 : 0;
			if (Pixels[Index].G > BestValue)
			{
				BestValue = Pixels[Index].G;
				Best = FIntPoint(Index % MaskSize, Index / MaskSize);
			}
		}
		return Best;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyRuntimeHitPathTest, "Grungify.RuntimeWear.ComplexHitUVChannelAndAddressing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyRuntimeHitPathTest::RunTest(const FString& Parameters)
{
	using namespace GrungifyRuntimeHitTests;
	if (!FApp::CanEverRender())
	{
		AddError(TEXT("This test reads the live masks back from the GPU; run it with a rendering RHI (not -nullrhi)."));
		return false;
	}
	// UVs from hits are cooked into collision only with this setting on; restore it (and the cube-free fixture) after.
	TGuardValue<bool> UVGuard(GetMutableDefault<UPhysicsSettings>()->bSupportUVFromHitResults, true);
	TGuardValue<int32> BaseGuard(GetMutableDefault<UGrungifySettings>()->CustomPrimitiveDataIndex, 0);
	TGuardValue<int32> ChannelGuard(GetMutableDefault<UGrungifySettings>()->MaskUVChannel, 1);

	UStaticMesh* Mesh = MakeQuad();
	UTexture2D* Mask = MakeMask(TA_Wrap, TA_Clamp);
	TStrongObjectPtr<UStaticMesh> MeshGuard(Mesh);
	TStrongObjectPtr<UTexture2D> MaskGuard(Mask);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	ON_SCOPE_EXIT
	{
		if (World)
		{
			World->DestroyWorld(false);
			// Retire this isolated world's scene while its transient mesh and texture are still rooted.
			FlushRenderingCommands();
		}
	};
	if (!TestNotNull(TEXT("Quad fixture"), Mesh) || !TestNotNull(TEXT("Mask fixture"), Mask) || !TestNotNull(TEXT("World"), World))
	{
		return false;
	}
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!TestNotNull(TEXT("Quad has built render data"), RenderData)
		|| !TestEqual(TEXT("Quad has one material slot named Slot0"), Mesh->GetMaterialIndex(TEXT("Slot0")), 0)
		|| !TestEqual(TEXT("Quad has one render LOD"), RenderData->LODResources.Num(), 1))
	{
		return false;
	}
	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	if (!TestEqual(TEXT("Quad has one render section"), LOD.Sections.Num(), 1)
		|| !TestEqual(TEXT("Quad section uses material index zero"), LOD.Sections[0].MaterialIndex, 0)
		|| !TestEqual(TEXT("Quad contains two triangles"), int32(LOD.GetNumTriangles()), 2)
		|| !TestEqual(TEXT("Quad contains both collision UV channels"), int32(LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()), 2))
	{
		return false;
	}

	AActor* Actor = World->SpawnActor<AActor>();
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
	Actor->SetRootComponent(Component);
	Component->SetStaticMesh(Mesh);
	Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Component->SetCollisionResponseToAllChannels(ECR_Block);
	UMaterialInstanceConstant* Material = MakeMaterial(Component, Mask, *this);
	if (!TestNotNull(TEXT("Declared material with a constant-instance parent fixture"), Material))
	{
		return false;
	}
	Component->SetMaterial(0, Material);
	Component->SetCustomPrimitiveDataFloat(Grungify::CPD_MaskMode, Grungify::MaskModeTexture);
	Component->RegisterComponent();

	UGrungifyRuntimeWearSubsystem* Subsystem = NewObject<UGrungifyRuntimeWearSubsystem>(World);

	// A real complex trace straight down onto the quad at (X, Y) cm, painted through the public hit path.
	auto PaintAt = [&](double X, double Y, const TCHAR* What, TArray<FColor>& OutPixels)
	{
		FCollisionQueryParams Query(SCENE_QUERY_STAT(GrungifyRuntimeHitTest), /*bTraceComplex*/ true);
		Query.bReturnFaceIndex = true;
		FHitResult Hit;
		if (!TestTrue(FString::Printf(TEXT("%s: complex trace hits the quad"), What),
			World->LineTraceSingleByChannel(Hit, FVector(X, Y, 100.), FVector(X, Y, -100.), ECC_Visibility, Query) && Hit.GetComponent() == Component))
		{
			return false;
		}
		TestTrue(FString::Printf(TEXT("%s: the hit carries a face index"), What), Hit.FaceIndex != INDEX_NONE);
		if (!TestTrue(FString::Printf(TEXT("%s: AddWearAtHit paints (UVs cooked into the collision)"), What),
			Subsystem->AddWearAtHit(Hit, EGrungifyWearLayer::Dust, /*RadiusCm*/ 2.f, /*Amount*/ 1.f)))
		{
			return false;
		}
		int32 Width = 0, Height = 0;
		return TestTrue(FString::Printf(TEXT("%s: GPU readback"), What), Subsystem->ReadRuntimeWearMask(Component, 0, false, OutPixels, Width, Height)
			&& Width == MaskSize && Height == MaskSize);
	};
	auto Near = [](const FIntPoint& Texel, double U, double V)
	{
		// The stroke centre sits on a texel corner or centre; its peak is within one texel of U, V.
		return FMath::Abs(Texel.X + 0.5 - U * MaskSize) <= 1.0 && FMath::Abs(Texel.Y + 0.5 - V * MaskSize) <= 1.0;
	};

	// 1. Effective channel UV0, U beyond 1 on a wrapping axis: X = 175 cm is UV0 U = 1.25, which samples texel U 0.25.
	Material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grungify Use UV0")), 1.f);
	Material->PostEditChange();
	TArray<FColor> Pixels;
	int32 Painted = 0;
	if (PaintAt(175., 100., TEXT("Wrap X"), Pixels))
	{
		const FIntPoint Texel = Peak(Pixels, Painted);
		TestTrue(FString::Printf(TEXT("Wrap X: paint at U 0.25, V 0.5 (got texel %d, %d)"), Texel.X, Texel.Y), Near(Texel, 0.25, 0.5));
	}
	Subsystem->ResetRuntimeWear(Component);

	// 2. V beyond 1 on a clamping axis: Y = 190 cm is UV0 V = 1.4; the surface there shows the last row, so paint goes
	// there and nothing wraps to the top rows.
	if (PaintAt(100., 190., TEXT("Clamp Y"), Pixels))
	{
		const FIntPoint Texel = Peak(Pixels, Painted);
		TestTrue(FString::Printf(TEXT("Clamp Y: paint on the last rows at U 0.5 (got texel %d, %d)"), Texel.X, Texel.Y),
			FMath::Abs(Texel.X + 0.5 - 0.5 * MaskSize) <= 1.0 && Texel.Y >= MaskSize - 3);
		bool bTopRowsClean = true;
		for (int32 Index = 0; Index < 3 * MaskSize; ++Index)
		{
			bTopRowsClean &= Pixels[Index].G == 0;
		}
		TestTrue(TEXT("Clamp Y: nothing wraps to the top rows"), bTopRowsClean);
	}
	Subsystem->ResetRuntimeWear(Component);

	// 3. The settings channel (UV1) when Use UV0 is off: X = Y = 50 cm is UV1 (0.25, 0.25), UV0 (0, 0).
	Material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grungify Use UV0")), 0.f);
	Material->PostEditChange();
	if (PaintAt(50., 50., TEXT("UV1"), Pixels))
	{
		const FIntPoint Texel = Peak(Pixels, Painted);
		TestTrue(FString::Printf(TEXT("UV1: paint at the material's channel, U 0.25, V 0.25 (got texel %d, %d)"), Texel.X, Texel.Y), Near(Texel, 0.25, 0.25));
		TestEqual(TEXT("UV1: nothing at UV0's position"), int32(Pixels[0].G), 0);
	}
	Subsystem->ResetRuntimeWear(Component);

	// 4. A blend, as the material computes it: Use UV0 0.5 at (50, 50) is UV (0.125, 0.125).
	Material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Grungify Use UV0")), .5f);
	Material->PostEditChange();
	if (PaintAt(50., 50., TEXT("Blend"), Pixels))
	{
		const FIntPoint Texel = Peak(Pixels, Painted);
		TestTrue(FString::Printf(TEXT("Blend: paint at lerp(UV1, UV0, 0.5) = 0.125 (got texel %d, %d)"), Texel.X, Texel.Y), Near(Texel, 0.125, 0.125));
	}
	Subsystem->ResetRuntimeWear(Component);
	return true;
}

#endif
