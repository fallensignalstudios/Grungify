#include "GrungifyGenerator.h"

#if WITH_EDITOR

#include "CollisionQueryParams.h"
#include "ComponentReregisterContext.h"
#include "Components/StaticMeshComponent.h"
#include "Components/StaticMeshComponentLODInfo.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GrungifySettings.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshResources.h"

namespace Grungify
{
	// ------------------------------------------------------------------ Noise

	static FORCEINLINE uint32 HashU(uint32 X)
	{
		X ^= X >> 16;
		X *= 0x7feb352du;
		X ^= X >> 15;
		X *= 0x846ca68bu;
		X ^= X >> 16;
		return X;
	}

	float Hash3(int32 X, int32 Y, int32 Z, uint32 Seed)
	{
		uint32 H = HashU(uint32(Z) * 0xcb1ab31fu ^ Seed);
		H = HashU(uint32(Y) * 0xd8163841u ^ H);
		H = HashU(uint32(X) * 0x8da6b343u ^ H);
		return float(H & 0x00FFFFFFu) / 16777216.f;
	}

	float ValueNoise(const FVector& P, uint32 Seed)
	{
		const double FX = FMath::FloorToDouble(P.X);
		const double FY = FMath::FloorToDouble(P.Y);
		const double FZ = FMath::FloorToDouble(P.Z);
		const int32 X = int32(FX);
		const int32 Y = int32(FY);
		const int32 Z = int32(FZ);

		auto Fade = [](double T) { return float(T * T * (3.0 - 2.0 * T)); };
		const float UX = Fade(P.X - FX);
		const float UY = Fade(P.Y - FY);
		const float UZ = Fade(P.Z - FZ);

		const float N000 = Hash3(X, Y, Z, Seed);
		const float N100 = Hash3(X + 1, Y, Z, Seed);
		const float N010 = Hash3(X, Y + 1, Z, Seed);
		const float N110 = Hash3(X + 1, Y + 1, Z, Seed);
		const float N001 = Hash3(X, Y, Z + 1, Seed);
		const float N101 = Hash3(X + 1, Y, Z + 1, Seed);
		const float N011 = Hash3(X, Y + 1, Z + 1, Seed);
		const float N111 = Hash3(X + 1, Y + 1, Z + 1, Seed);

		const float A = FMath::Lerp(FMath::Lerp(N000, N100, UX), FMath::Lerp(N010, N110, UX), UY);
		const float B = FMath::Lerp(FMath::Lerp(N001, N101, UX), FMath::Lerp(N011, N111, UX), UY);
		return FMath::Lerp(A, B, UZ);
	}

	float Fbm(const FVector& P, uint32 Seed, int32 Octaves)
	{
		float Sum = 0.f;
		float Amp = 0.5f;
		float Norm = 0.f;
		FVector Q = P;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			Sum += ValueNoise(Q, Seed + uint32(Octave) * 101u) * Amp;
			Norm += Amp;
			Q = Q * 2.03 + FVector(17.17);
			Amp *= 0.5f;
		}
		return Norm > 0.f ? Sum / Norm : 0.f;
	}

	// --------------------------------------------------------- Classification

	void TokenizeName(const FString& Name, TArray<FString>& OutTokens)
	{
		FString Current;
		auto Flush = [&]()
		{
			if (!Current.IsEmpty())
			{
				OutTokens.Add(Current.ToLower());
				Current.Reset();
			}
		};

		TCHAR Prev = 0;
		for (const TCHAR Ch : Name)
		{
			if (!FChar::IsAlnum(Ch))
			{
				Flush();
				Prev = 0;
				continue;
			}

			if (Prev != 0)
			{
				const bool bLowerToUpper = FChar::IsLower(Prev) && FChar::IsUpper(Ch);
				const bool bDigitChange = FChar::IsDigit(Prev) != FChar::IsDigit(Ch);
				if (bLowerToUpper || bDigitChange)
				{
					Flush();
				}
				else if (FChar::IsUpper(Prev) && FChar::IsLower(Ch) && Current.Len() > 1)
				{
					// "HVACVent": the last capital of an acronym starts the next word.
					const TCHAR Carry = Current[Current.Len() - 1];
					Current.LeftChopInline(1);
					Flush();
					Current.AppendChar(Carry);
				}
			}

			Current.AppendChar(Ch);
			Prev = Ch;
		}
		Flush();
	}

	bool NameMatches(const FString& Name, const TArray<FString>& Keywords)
	{
		if (Name.IsEmpty() || Keywords.Num() == 0)
		{
			return false;
		}

		TArray<FString> Tokens;
		TokenizeName(Name, Tokens);
		for (const FString& Token : Tokens)
		{
			for (const FString& Keyword : Keywords)
			{
				if (!Keyword.IsEmpty() && Token.StartsWith(Keyword, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	static EGrungifySurfaceType SurfaceFromTags(const TArray<FName>& Tags, const UGrungifySettings* Settings)
	{
		for (const FName& Tag : Tags)
		{
			if (Tag == Settings->TrimTag)
			{
				return EGrungifySurfaceType::Trim;
			}
			if (Tag == Settings->MetalTag)
			{
				return EGrungifySurfaceType::Metal;
			}
			if (Tag == Settings->StoneTag)
			{
				return EGrungifySurfaceType::Stone;
			}
		}
		return EGrungifySurfaceType::Auto;
	}

	static EGrungifySurfaceType SurfaceFromName(const FString& Name, const UGrungifySettings* Settings)
	{
		if (NameMatches(Name, Settings->TrimKeywords))
		{
			return EGrungifySurfaceType::Trim;
		}
		if (NameMatches(Name, Settings->MetalKeywords))
		{
			return EGrungifySurfaceType::Metal;
		}
		if (NameMatches(Name, Settings->StoneKeywords))
		{
			return EGrungifySurfaceType::Stone;
		}
		return EGrungifySurfaceType::Auto;
	}

	EGrungifySurfaceType ClassifySlot(const UStaticMeshComponent* Component, int32 MaterialIndex, EGrungifySurfaceType Override)
	{
		if (Override != EGrungifySurfaceType::Auto)
		{
			return Override;
		}

		const UGrungifySettings* Settings = UGrungifySettings::Get();
		const AActor* Owner = Component->GetOwner();

		// 1. Explicit tags win.
		EGrungifySurfaceType Result = SurfaceFromTags(Component->ComponentTags, Settings);
		if (Result == EGrungifySurfaceType::Auto && Owner)
		{
			Result = SurfaceFromTags(Owner->Tags, Settings);
		}
		if (Result != EGrungifySurfaceType::Auto)
		{
			return Result;
		}

		// 2. Physical material, then material names up the instance chain.
		if (UMaterialInterface* Material = Component->GetMaterial(MaterialIndex))
		{
			if (const UPhysicalMaterial* PhysMat = Material->GetPhysicalMaterial())
			{
				if (const EGrungifySurfaceType* Mapped = Settings->PhysicalSurfaceMap.Find(PhysMat->SurfaceType))
				{
					if (*Mapped != EGrungifySurfaceType::Auto)
					{
						return *Mapped;
					}
				}
			}

			for (const UMaterialInterface* It = Material; It; )
			{
				Result = SurfaceFromName(It->GetName(), Settings);
				if (Result != EGrungifySurfaceType::Auto)
				{
					return Result;
				}
				const UMaterialInstance* Instance = Cast<UMaterialInstance>(It);
				It = Instance ? Instance->Parent.Get() : nullptr;
			}
		}

		// 3. Slot name, mesh name, actor label.
		if (const UStaticMesh* Mesh = Component->GetStaticMesh())
		{
			const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
			if (StaticMaterials.IsValidIndex(MaterialIndex))
			{
				Result = SurfaceFromName(StaticMaterials[MaterialIndex].MaterialSlotName.ToString(), Settings);
				if (Result != EGrungifySurfaceType::Auto)
				{
					return Result;
				}
			}

			Result = SurfaceFromName(Mesh->GetName(), Settings);
			if (Result != EGrungifySurfaceType::Auto)
			{
				return Result;
			}
		}

		if (Owner)
		{
			Result = SurfaceFromName(Owner->GetActorLabel(), Settings);
			if (Result != EGrungifySurfaceType::Auto)
			{
				return Result;
			}
		}

		return Settings->FallbackSurfaceType == EGrungifySurfaceType::Auto ? EGrungifySurfaceType::Stone : Settings->FallbackSurfaceType;
	}

	bool IsRail(const UStaticMeshComponent* Component)
	{
		const UGrungifySettings* Settings = UGrungifySettings::Get();
		const AActor* Owner = Component->GetOwner();

		if (Component->ComponentHasTag(Settings->RailTag) || (Owner && Owner->ActorHasTag(Settings->RailTag)))
		{
			return true;
		}
		if (const UStaticMesh* Mesh = Component->GetStaticMesh())
		{
			if (NameMatches(Mesh->GetName(), Settings->RailKeywords))
			{
				return true;
			}
		}
		return Owner && NameMatches(Owner->GetActorLabel(), Settings->RailKeywords);
	}

	bool IsVent(const AActor* Actor, const UPrimitiveComponent* Component)
	{
		const UGrungifySettings* Settings = UGrungifySettings::Get();

		if (Actor)
		{
			return Actor->ActorHasTag(Settings->VentTag) || NameMatches(Actor->GetActorLabel(), Settings->VentKeywords);
		}

		if (Component)
		{
			if (Component->ComponentHasTag(Settings->VentTag))
			{
				return true;
			}
			if (const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component))
			{
				const UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
				return Mesh && NameMatches(Mesh->GetName(), Settings->VentKeywords);
			}
		}
		return false;
	}

	bool HasIgnoreTag(const AActor* Actor, const UActorComponent* Component)
	{
		const FName IgnoreTag = UGrungifySettings::Get()->IgnoreTag;
		return (Actor && Actor->ActorHasTag(IgnoreTag)) || (Component && Component->ComponentHasTag(IgnoreTag));
	}
}

// --------------------------------------------------------------- Floor field

void FGrungifyFloorField::BuildFlat(double InFloorZ)
{
	FlatZ = InFloorZ;
	NumX = NumY = 0;
	Heights.Reset();
}

void FGrungifyFloorField::BuildTraced(UWorld* World, const FBox& InBounds, double Spacing, const AActor* IgnoreActor)
{
	BuildFlat(InBounds.Min.Z);
	if (!World || !InBounds.IsValid)
	{
		return;
	}

	const UGrungifySettings* Settings = UGrungifySettings::Get();
	constexpr int32 MaxCellsPerAxis = 128;
	constexpr int32 MaxHitsPerCell = 16;

	Bounds = InBounds;
	const FVector Size = Bounds.GetSize();
	const double Step = FMath::Max(Spacing, 10.0);
	NumX = FMath::Clamp(FMath::CeilToInt(Size.X / Step), 1, MaxCellsPerAxis);
	NumY = FMath::Clamp(FMath::CeilToInt(Size.Y / Step), 1, MaxCellsPerAxis);
	CellX = Size.X / NumX;
	CellY = Size.Y / NumY;
	Heights.SetNum(NumX * NumY);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GrungifyFloorTrace), /*bTraceComplex*/ true);
	if (IgnoreActor)
	{
		QueryParams.AddIgnoredActor(IgnoreActor);
	}

	auto IsFloor = [Settings](const FHitResult& Hit)
	{
		if (Hit.ImpactNormal.Z < 0.65)
		{
			return false;
		}
		const UPrimitiveComponent* HitComponent = Hit.GetComponent();
		const AActor* HitActor = Hit.GetActor();
		if (Grungify::HasIgnoreTag(HitActor, HitComponent))
		{
			return false;
		}
		if ((HitComponent && HitComponent->ComponentHasTag(Settings->FloorTag)) || (HitActor && HitActor->ActorHasTag(Settings->FloorTag)))
		{
			return true;
		}
		if (!HitComponent)
		{
			return true;
		}
		const FVector Extent = HitComponent->Bounds.BoxExtent;
		return FMath::Min(Extent.X, Extent.Y) * 2.0 >= Settings->MinFloorSizeCm;
	};

	const double TopZ = Bounds.Max.Z + 50.0;
	const double BottomZ = Bounds.Min.Z - 500.0;

	for (int32 Y = 0; Y < NumY; ++Y)
	{
		for (int32 X = 0; X < NumX; ++X)
		{
			TArray<double, TInlineAllocator<4>>& Cell = Heights[Y * NumX + X];
			FVector Start(Bounds.Min.X + (X + 0.5) * CellX, Bounds.Min.Y + (Y + 0.5) * CellY, TopZ);
			const FVector End(Start.X, Start.Y, BottomZ);

			for (int32 HitIndex = 0; HitIndex < MaxHitsPerCell; ++HitIndex)
			{
				FHitResult Hit;
				if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, QueryParams))
				{
					break;
				}
				if (IsFloor(Hit))
				{
					Cell.Add(Hit.ImpactPoint.Z);
				}
				// Step just past the hit and keep going to find the storeys below.
				Start.Z = Hit.ImpactPoint.Z - 1.0;
				if (Start.Z <= End.Z)
				{
					break;
				}
			}

			Cell.Sort([](double A, double B) { return A > B; });
		}
	}
}

double FGrungifyFloorField::FloorBelow(const FVector& P) const
{
	if (NumX == 0 || NumY == 0)
	{
		return FlatZ;
	}

	const int32 X = FMath::Clamp(FMath::FloorToInt((P.X - Bounds.Min.X) / CellX), 0, NumX - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt((P.Y - Bounds.Min.Y) / CellY), 0, NumY - 1);
	for (const double Height : Heights[Y * NumX + X])
	{
		// Small tolerance so vertices sitting on the floor (or sunk into it) still find it.
		if (Height <= P.Z + 2.0)
		{
			return Height;
		}
	}
	return FMath::Min(FlatZ, P.Z);
}

// ---------------------------------------------------------------- Generation

namespace Grungify
{
	static FORCEINLINE uint8 Quantize(float Value)
	{
		return uint8(FMath::RoundToInt(FMath::Clamp(Value, 0.f, 1.f) * 255.f));
	}

	static float ZoneWeight(const FGrungifyGenerationParams& Params, const FVector& P)
	{
		const FVector Local = Params.ZoneTransform.InverseTransformPositionNoScale(P);
		const FVector Inside = Params.ZoneExtent - Local.GetAbs();
		const double MinInside = Inside.GetMin();
		if (MinInside <= 0.0)
		{
			return 0.f;
		}
		return Params.ZoneFalloff > 1.f ? FMath::Clamp(float(MinInside / Params.ZoneFalloff), 0.f, 1.f) : 1.f;
	}

	static float RunoffFromSource(const FGrungifyRunoffSource& Source, const FVector& P, const FVector& N, const FGrungifyGenerationParams& Params)
	{
		// Runoff follows gravity.
		const FVector Rel = P - Source.Origin;
		const double Below = -Rel.Z;
		const double Lead = Source.Radius * 0.25;
		if (Below < -Lead || Below > Params.RunoffLength)
		{
			return 0.f;
		}

		// Skip the far side of the wall the vent is mounted on.
		const double BackfaceTolerance = Source.Radius * 0.25 + 2.0;
		if (FVector::DotProduct(Source.Origin - P, N) < -BackfaceTolerance)
		{
			return 0.f;
		}

		const double Down = FMath::Max(Below, 0.0);
		const double Lateral = FVector(Rel.X, Rel.Y, 0.0).Size();
		const double Width = Source.Radius * (1.0 + Params.RunoffSpread * Down / Params.RunoffLength);
		if (Lateral > Width)
		{
			return 0.f;
		}

		const float Edge = 1.f - FMath::SmoothStep(float(Width * 0.5), float(Width), float(Lateral));
		const float Along = FMath::Pow(float(1.0 - Down / Params.RunoffLength), 1.5f);
		const float Lip = Below < 0.0 ? float(1.0 + Below / Lead) : 1.f;

		// Streaks: fine across the flow, long along it.
		const double StreakScale = FMath::Max(Params.FeatureScale * 0.35, 1.0);
		const FVector StreakP(P.X / StreakScale, P.Y / StreakScale, P.Z / (Params.RunoffLength * 0.5));
		const float Streak = Fbm(StreakP, Params.Seed + 71u);

		return Edge * Along * Lip * FMath::Lerp(0.35f, 1.f, Streak);
	}

	bool GenerateComponent(UStaticMeshComponent* Component, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult)
	{
		UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData || RenderData->LODResources.Num() == 0 || !Params.Floor)
		{
			return false;
		}

		OutResult.Component = Component;
		OutResult.bNanite = Mesh->HasValidNaniteData();
		OutResult.LODColors.SetNum(RenderData->LODResources.Num());

		const FTransform& Transform = Component->GetComponentTransform();
		const FVector Scale = Transform.GetScale3D();
		const FVector InvScale(
			FMath::IsNearlyZero(Scale.X) ? 0.0 : 1.0 / Scale.X,
			FMath::IsNearlyZero(Scale.Y) ? 0.0 : 1.0 / Scale.Y,
			FMath::IsNearlyZero(Scale.Z) ? 0.0 : 1.0 / Scale.Z);

		const float SharpRef = 1.f - FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Params.EdgeAngleDeg, 5.f, 170.f) * 0.5f));
		const FVector Upwind = -Params.WearDir;
		const double FeatureScale = FMath::Max<double>(Params.FeatureScale, 1.0);
		const EGrungifySurfaceType DefaultSurface = Context.SlotSurfaces.Num() > 0 ? Context.SlotSurfaces[0] : EGrungifySurfaceType::Stone;

		int32 SurfaceVotes[3] = { 0, 0, 0 };
		bool bAnyLOD = false;

		for (int32 LODIndex = 0; LODIndex < RenderData->LODResources.Num(); ++LODIndex)
		{
			const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
			const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
			const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
			const int32 NumVerts = int32(LOD.GetNumVertices());

			TArray<uint32> Indices;
			LOD.IndexBuffer.GetCopy(Indices);

			if (NumVerts == 0 || Indices.Num() < 3 || int32(Positions.GetNumVertices()) != NumVerts || int32(Attributes.GetNumVertices()) != NumVerts)
			{
				continue;
			}

			// World-space positions and normals.
			TArray<FVector> WorldPos;
			TArray<FVector> WorldNormal;
			WorldPos.SetNumUninitialized(NumVerts);
			WorldNormal.SetNumUninitialized(NumVerts);

			// Weld split vertices (hard edges, UV seams) so edge detection sees the real topology.
			TArray<int32> Weld;
			Weld.SetNumUninitialized(NumVerts);
			TMap<FIntVector, int32> WeldMap;
			WeldMap.Reserve(NumVerts);
			TArray<int32> WeldFirst;
			WeldFirst.Reserve(NumVerts);

			for (int32 V = 0; V < NumVerts; ++V)
			{
				const FVector3f L = Positions.VertexPosition(V);
				const FVector4f N = Attributes.VertexTangentZ(V);
				WorldPos[V] = Transform.TransformPosition(FVector(L.X, L.Y, L.Z));
				WorldNormal[V] = Transform.TransformVectorNoScale(FVector(N.X, N.Y, N.Z) * InvScale).GetSafeNormal();

				const FIntVector Key(FMath::RoundToInt(L.X * 20.f), FMath::RoundToInt(L.Y * 20.f), FMath::RoundToInt(L.Z * 20.f));
				if (const int32* Found = WeldMap.Find(Key))
				{
					Weld[V] = *Found;
				}
				else
				{
					Weld[V] = WeldFirst.Num();
					WeldMap.Add(Key, WeldFirst.Num());
					WeldFirst.Add(V);
				}
			}

			const int32 NumWelded = WeldFirst.Num();
			const int32 NumTris = Indices.Num() / 3;

			// Area-weighted average normal per welded vertex.
			TArray<FVector> FaceNormal;
			FaceNormal.SetNumZeroed(NumTris);
			TArray<FVector> AvgNormal;
			AvgNormal.Init(FVector::ZeroVector, NumWelded);

			for (int32 T = 0; T < NumTris; ++T)
			{
				const uint32 I0 = Indices[T * 3 + 0];
				const uint32 I1 = Indices[T * 3 + 1];
				const uint32 I2 = Indices[T * 3 + 2];
				if (I0 >= uint32(NumVerts) || I1 >= uint32(NumVerts) || I2 >= uint32(NumVerts))
				{
					continue;
				}

				FVector Cross = FVector::CrossProduct(WorldPos[I1] - WorldPos[I0], WorldPos[I2] - WorldPos[I0]);
				if (Cross.SizeSquared() < UE_SMALL_NUMBER)
				{
					continue;
				}
				// Orient by the authored normals so winding / mirroring doesn't matter.
				if (FVector::DotProduct(Cross, WorldNormal[I0] + WorldNormal[I1] + WorldNormal[I2]) < 0.0)
				{
					Cross = -Cross;
				}
				FaceNormal[T] = Cross.GetUnsafeNormal();

				const int32 W0 = Weld[I0], W1 = Weld[I1], W2 = Weld[I2];
				AvgNormal[W0] += Cross;
				if (W1 != W0)
				{
					AvgNormal[W1] += Cross;
				}
				if (W2 != W0 && W2 != W1)
				{
					AvgNormal[W2] += Cross;
				}
			}
			for (FVector& N : AvgNormal)
			{
				N = N.GetSafeNormal();
			}

			// Sharpness: how far the faces around a vertex bend away from its average normal.
			// Side: whether the neighbours sit below (convex) or above (concave) the tangent plane.
			TArray<float> Sharp;
			Sharp.Init(0.f, NumWelded);
			TArray<float> SideSum;
			SideSum.Init(0.f, NumWelded);
			TArray<int32> SideCount;
			SideCount.Init(0, NumWelded);

			for (int32 T = 0; T < NumTris; ++T)
			{
				if (FaceNormal[T].IsZero())
				{
					continue;
				}
				const int32 W[3] = { Weld[Indices[T * 3 + 0]], Weld[Indices[T * 3 + 1]], Weld[Indices[T * 3 + 2]] };
				for (int32 K = 0; K < 3; ++K)
				{
					const int32 Wk = W[K];
					Sharp[Wk] = FMath::Max(Sharp[Wk], float(1.0 - FVector::DotProduct(FaceNormal[T], AvgNormal[Wk])));

					const FVector& Pk = WorldPos[WeldFirst[Wk]];
					for (int32 J = 0; J < 3; ++J)
					{
						const int32 Wj = W[J];
						if (Wj == Wk)
						{
							continue;
						}
						const FVector Dir = (WorldPos[WeldFirst[Wj]] - Pk).GetSafeNormal();
						if (!Dir.IsZero())
						{
							SideSum[Wk] += float(FVector::DotProduct(AvgNormal[Wk], Dir));
							++SideCount[Wk];
						}
					}
				}
			}

			TArray<float> Convex;
			TArray<float> Concave;
			Convex.SetNumUninitialized(NumWelded);
			Concave.SetNumUninitialized(NumWelded);
			for (int32 Wi = 0; Wi < NumWelded; ++Wi)
			{
				const float SharpN = FMath::Clamp(Sharp[Wi] / SharpRef, 0.f, 1.f);
				const float Side = SideCount[Wi] > 0 ? SideSum[Wi] / SideCount[Wi] : 0.f;
				Convex[Wi] = SharpN * FMath::Clamp(-Side * 3.f, 0.f, 1.f);
				Concave[Wi] = SharpN * FMath::Clamp(Side * 3.f, 0.f, 1.f);
			}

			// Surface type per vertex, from the material slot of the section it belongs to.
			TArray<EGrungifySurfaceType> VertSurface;
			VertSurface.Init(DefaultSurface, NumVerts);
			for (const FStaticMeshSection& Section : LOD.Sections)
			{
				const EGrungifySurfaceType SectionSurface = Context.SlotSurfaces.IsValidIndex(Section.MaterialIndex) ? Context.SlotSurfaces[Section.MaterialIndex] : DefaultSurface;
				const uint32 First = Section.FirstIndex;
				const uint32 Last = FMath::Min<uint32>(First + Section.NumTriangles * 3, uint32(Indices.Num()));
				for (uint32 Idx = First; Idx < Last; ++Idx)
				{
					if (Indices[Idx] < uint32(NumVerts))
					{
						VertSurface[Indices[Idx]] = SectionSurface;
					}
				}
			}

			const float HandSoft = FMath::Max((Params.HandMax - Params.HandMin) * 0.25f, 5.f);

			TArray<FColor>& Colors = OutResult.LODColors[LODIndex];
			Colors.SetNumUninitialized(NumVerts);

			for (int32 V = 0; V < NumVerts; ++V)
			{
				const FVector& P = WorldPos[V];
				const FVector& N = WorldNormal[V];
				const int32 Wi = Weld[V];

				const int32 SurfaceIndex = FMath::Clamp(int32(VertSurface[V]) - 1, 0, 2);
				if (LODIndex == 0)
				{
					++SurfaceVotes[SurfaceIndex];
				}

				const float ZoneW = ZoneWeight(Params, P);
				if (ZoneW <= 0.f)
				{
					Colors[V] = FColor(0, 0, 0, 0);
					continue;
				}

				const FGrungifySurfaceProfile& Profile = Params.Profiles[SurfaceIndex];
				const float Height = float(FMath::Max(0.0, P.Z - Params.Floor->FloorBelow(P)));
				const FVector NP = P / FeatureScale;
				const float NZ = float(N.Z);

				// Handling: hands polish rails and, more lightly, edges at hand height.
				const float Band = FMath::SmoothStep(Params.HandMin - HandSoft, Params.HandMin, Height)
					* (1.f - FMath::SmoothStep(Params.HandMax, Params.HandMax + HandSoft, Height));
				const float Top = FMath::Clamp(NZ * 0.7f + 0.5f, 0.f, 1.f);
				float Handling = Context.bIsRail
					? Top * (0.6f + 0.4f * Band)
					: Band * Convex[Wi] * Params.GeneralHandling;
				Handling *= FMath::Lerp(0.7f, 1.f, Fbm(NP * 0.5, Params.Seed + 11u)) * Params.HandlingIntensity * Profile.Handling;
				Handling = FMath::Clamp(Handling, 0.f, 1.f);

				// Dust: thick at the base of walls, on ledges and packed into creases.
				const float UpFacing = FMath::Clamp((NZ - 0.6f) / 0.4f, 0.f, 1.f);
				const bool bOnFloor = UpFacing > 0.5f && Height < 5.f;
				float Dust = 1.f - FMath::SmoothStep(0.f, FMath::Max(Params.DustHeight, 1.f), Height);
				Dust *= Dust;
				if (bOnFloor)
				{
					Dust *= Params.FloorDust;
				}
				else
				{
					Dust = FMath::Max(Dust, UpFacing * Params.LedgeDust);
				}
				Dust = FMath::Max(Dust, Concave[Wi] * Params.CreviceDust);
				Dust *= FMath::Lerp(0.6f, 1.f, Fbm(NP * 0.8, Params.Seed + 23u));
				Dust *= FMath::Clamp(1.2f + NZ, 0.f, 1.f); // little on undersides
				Dust *= 1.f - Handling * 0.8f;              // hands keep it clean
				Dust *= Params.DustIntensity * Profile.Dust;

				// Runoff: streaks down from vents, plus faint general streaking on walls.
				float Runoff = 0.f;
				for (const FGrungifyRunoffSource& Source : Params.Sources)
				{
					Runoff = FMath::Max(Runoff, RunoffFromSource(Source, P, N, Params));
				}
				if (Params.AmbientStreaks > 0.f)
				{
					const float Vertical = 1.f - FMath::Abs(NZ);
					const FVector StreakP(NP.X * 3.0, NP.Y * 3.0, NP.Z * 0.2);
					Runoff = FMath::Max(Runoff, Vertical * Params.AmbientStreaks * FMath::SmoothStep(0.45f, 0.8f, Fbm(StreakP, Params.Seed + 37u)));
				}
				Runoff *= FMath::Clamp(1.f + NZ * 1.5f, 0.f, 1.f) * Params.RunoffIntensity * Profile.Runoff;

				// Edge chips: convex edges, more where they face into the wear direction or get kicked.
				const float Exposure = FMath::Lerp(1.f, FMath::Clamp(0.25f + 0.75f * float(FVector::DotProduct(N, Upwind)), 0.f, 1.f), Params.WearInfluence);
				const float Kick = (1.f - FMath::SmoothStep(0.f, FMath::Max(Params.FloorKickHeight, 1.f), Height)) * 0.5f;
				float Chips = Convex[Wi] * FMath::Min(Exposure + Kick, 1.f);
				Chips *= FMath::Lerp(0.35f, 1.f, Fbm(NP * 1.5, Params.Seed + 51u)) * Params.ChipIntensity * Profile.EdgeChips;

				const float K = Params.Intensity * ZoneW;
				Colors[V] = FColor(Quantize(Chips * K), Quantize(Dust * K), Quantize(Runoff * K), Quantize(Handling * K));
			}

			bAnyLOD = true;
		}

		int32 Best = 0;
		for (int32 Index = 1; Index < 3; ++Index)
		{
			if (SurfaceVotes[Index] > SurfaceVotes[Best])
			{
				Best = Index;
			}
		}
		OutResult.DominantSurface = EGrungifySurfaceType(Best + 1);
		return bAnyLOD;
	}

	// ------------------------------------------------------- Writing / restoring

	static void SetLODColors(UStaticMeshComponent* Component, int32 LODIndex, const TArray<FColor>& Colors)
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

	static void RemoveLODColors(UStaticMeshComponent* Component, int32 LODIndex)
	{
		if (!Component->LODData.IsValidIndex(LODIndex))
		{
			return;
		}
		FStaticMeshComponentLODInfo& Info = Component->LODData[LODIndex];
		if (Info.OverrideVertexColors)
		{
			Info.ReleaseOverrideVertexColorsAndBlock();
		}
#if WITH_EDITORONLY_DATA
		Info.PaintedVertices.Empty();
#endif
	}

	static void FinishVertexColorEdit(UStaticMeshComponent* Component, const FStaticMeshRenderData* RenderData)
	{
#if WITH_EDITORONLY_DATA
		// Lets the engine remap the colours if the mesh is reimported later.
		Component->StaticMeshDerivedDataKey = RenderData->DerivedDataKey;
#endif
		Component->CachePaintedDataIfNecessary();
	}

	static void EnsureLODData(UStaticMeshComponent* Component, int32 NumLODs)
	{
		if (Component->LODData.Num() < NumLODs)
		{
			Component->SetLODDataCount(uint32(NumLODs), uint32(NumLODs));
		}
	}

	void CaptureRecord(UStaticMeshComponent* Component, bool bCaptureMaterials, FGrungifyComponentRecord& OutRecord)
	{
		OutRecord = FGrungifyComponentRecord();
		OutRecord.Component = Component;

		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		const int32 NumLODs = RenderData ? RenderData->LODResources.Num() : 0;

		OutRecord.LODs.SetNum(NumLODs);
		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			if (!Component->LODData.IsValidIndex(LODIndex))
			{
				continue;
			}
			const FColorVertexBuffer* Existing = Component->LODData[LODIndex].OverrideVertexColors;
			if (Existing && Existing->GetNumVertices() > 0)
			{
				FGrungifyLODBackup& Backup = OutRecord.LODs[LODIndex];
				Backup.bHadOverride = true;
				Backup.Colors.SetNumUninitialized(Existing->GetNumVertices());
				for (uint32 V = 0; V < Existing->GetNumVertices(); ++V)
				{
					Backup.Colors[V] = Existing->VertexColor(V);
				}
			}
		}

		const int32 Base = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
		const TArray<float>& Data = Component->GetDefaultCustomPrimitiveData().Data;
		OutRecord.CustomData.SetNum(CPD_Count);
		for (int32 Offset = 0; Offset < CPD_Count; ++Offset)
		{
			OutRecord.CustomData[Offset] = Data.IsValidIndex(Base + Offset) ? Data[Base + Offset] : 0.f;
		}

		OutRecord.bMaterialsSwapped = bCaptureMaterials;
		if (bCaptureMaterials)
		{
			OutRecord.OverrideMaterials = Component->OverrideMaterials;
		}
	}

	void RestoreRecord(const FGrungifyComponentRecord& Record)
	{
		UStaticMeshComponent* Component = Record.Component.Get();
		if (!Component)
		{
			return;
		}

		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;

		Component->SetFlags(RF_Transactional);
		Component->Modify();

		{
			FComponentReregisterContext Reregister(Component);
			for (int32 LODIndex = 0; LODIndex < Record.LODs.Num(); ++LODIndex)
			{
				const FGrungifyLODBackup& Backup = Record.LODs[LODIndex];
				const bool bCanRestore = Backup.bHadOverride && RenderData && RenderData->LODResources.IsValidIndex(LODIndex)
					&& Backup.Colors.Num() == int32(RenderData->LODResources[LODIndex].GetNumVertices());
				if (bCanRestore)
				{
					EnsureLODData(Component, LODIndex + 1);
					SetLODColors(Component, LODIndex, Backup.Colors);
				}
				else
				{
					RemoveLODColors(Component, LODIndex);
				}
			}
			if (RenderData)
			{
				FinishVertexColorEdit(Component, RenderData);
			}
		}

		const int32 Base = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
		for (int32 Offset = 0; Offset < Record.CustomData.Num(); ++Offset)
		{
			Component->SetDefaultCustomPrimitiveDataFloat(Base + Offset, Record.CustomData[Offset]);
		}

		if (Record.bMaterialsSwapped)
		{
			const int32 NumSlots = FMath::Max(Component->GetNumMaterials(), Record.OverrideMaterials.Num());
			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				Component->SetMaterial(Slot, Record.OverrideMaterials.IsValidIndex(Slot) ? Record.OverrideMaterials[Slot].Get() : nullptr);
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

		Component->SetFlags(RF_Transactional);
		Component->Modify();

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
		FinishVertexColorEdit(Component, RenderData);
	}

	void WriteCustomData(UStaticMeshComponent* Component, const float (&Values)[CPD_Count])
	{
		const int32 Base = UGrungifySettings::Get()->CustomPrimitiveDataIndex;
		for (int32 Offset = 0; Offset < CPD_Count; ++Offset)
		{
			Component->SetDefaultCustomPrimitiveDataFloat(Base + Offset, Values[Offset]);
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
