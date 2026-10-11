#include "GrungifyGenerator.h"

#if WITH_EDITOR

#include "Async/ParallelFor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

namespace Grungify
{
	namespace
	{
		FORCEINLINE uint8 Quantize(float Value)
		{
			return uint8(FMath::RoundToInt(FMath::Clamp(Value, 0.f, 1.f) * 255.f));
		}

		FORCEINLINE float Cross2(const FVector2D& A, const FVector2D& B)
		{
			return float(A.X * B.Y - A.Y * B.X);
		}

		float ZoneWeight(const FGrungifyGenerationParams& Params, const FVector& P)
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

		float RunoffFromSource(const FGrungifyRunoffSource& Source, const FVector& P, const FVector& N, const FGrungifyGenerationParams& Params)
		{
			const double Length = FMath::Max<double>(Source.Length > 0.f ? Source.Length : Params.RunoffLength, 10.0);

			// Runoff follows gravity.
			const FVector Rel = P - Source.Origin;
			const double Below = -Rel.Z;
			const double Lead = Source.Radius * 0.25;
			if (Below < -Lead || Below > Length)
			{
				return 0.f;
			}

			// Skip the far side of the wall the source is on.
			const double BackfaceTolerance = Source.Radius * 0.25 + 2.0;
			if (FVector::DotProduct(Source.Origin - P, N) < -BackfaceTolerance)
			{
				return 0.f;
			}

			const double Down = FMath::Max(Below, 0.0);
			const double Lateral = FVector(Rel.X, Rel.Y, 0.0).Size();
			const double Width = Source.Radius * (1.0 + Params.RunoffSpread * Down / Length);
			if (Lateral > Width)
			{
				return 0.f;
			}

			const float Edge = 1.f - FMath::SmoothStep(float(Width * 0.5), float(Width), float(Lateral));
			const float Along = FMath::Pow(float(1.0 - Down / Length), 1.5f);
			const float Lip = Below < 0.0 ? float(1.0 + Below / Lead) : 1.f;

			// Streaks: fine across the flow, long along it.
			const double StreakScale = FMath::Max(Params.FeatureScale * 0.35, 1.0);
			const FVector StreakP(P.X / StreakScale, P.Y / StreakScale, P.Z / (Length * 0.5));
			const float Streak = Fbm(StreakP, Params.Seed + 71u);

			return Edge * Along * Lip * FMath::Lerp(0.35f, 1.f, Streak) * Source.Strength;
		}

		float RangeGate(float Value, float Min, float Max, float Soft, float Lowest, float Highest)
		{
			float Weight = 1.f;
			if (Min > Lowest)
			{
				Weight *= FMath::SmoothStep(Min - Soft, Min + Soft, Value);
			}
			if (Max < Highest)
			{
				Weight *= 1.f - FMath::SmoothStep(Max - Soft, Max + Soft, Value);
			}
			return Weight;
		}

		float GateWeight(const FGrungifySlotInfo& Slot, float NZ, float Height, const FVector2f& GateUV)
		{
			if (!Slot.Gate)
			{
				return 1.f;
			}
			const FGrungifyGate& Gate = *Slot.Gate;
			float Weight = 1.f;
			if (Gate.bUseFacingGate)
			{
				Weight *= RangeGate(NZ, Gate.MinUpFacing, Gate.MaxUpFacing, 0.05f, -1.f, 1.f);
			}
			if (Gate.bUseHeightGate)
			{
				Weight *= RangeGate(Height, Gate.MinHeightCm, Gate.MaxHeightCm, 5.f, -UE_BIG_NUMBER, UE_BIG_NUMBER);
			}
			if (Slot.GateSampler)
			{
				const float Value = Slot.GateSampler->Sample(GateUV, Gate.GateChannel);
				Weight *= Gate.bInvertGate ? 1.f - Value : Value;
			}
			return Weight;
		}

		struct FPointSample
		{
			FVector P;
			FVector N;
			float Convex = 0.f;
			float Concave = 0.f;
			const FGrungifySlotInfo* Slot = nullptr;
			FVector2f GateUV = FVector2f::ZeroVector;
		};

		/** Chips, dust, runoff, handling in 0..1 (R, G, B, A). */
		FLinearColor EvaluatePoint(const FPointSample& S, const FGrungifyGenerationParams& Params)
		{
			const FLinearColor None(0.f, 0.f, 0.f, 0.f);
			if (!S.Slot || S.Slot->bExcluded)
			{
				return None;
			}

			const float ZoneW = ZoneWeight(Params, S.P);
			if (ZoneW <= 0.f)
			{
				return None;
			}

			const FGrungifySurfaceProfile& Profile = S.Slot->Profile;
			const FVector& N = S.N;
			const float NZ = float(N.Z);
			const float Height = float(FMath::Max(0.0, S.P.Z - Params.Floor->FloorBelow(S.P)));

			const float Gate = GateWeight(*S.Slot, NZ, Height, S.GateUV);
			if (Gate <= 0.f)
			{
				return None;
			}

			const FVector NP = S.P / FMath::Max<double>(Params.FeatureScale, 1.0);

			// Handling: hands polish rails and, more lightly, edges at hand height.
			const float HandSoft = FMath::Max((Params.HandMax - Params.HandMin) * 0.25f, 5.f);
			const float Band = FMath::SmoothStep(Params.HandMin - HandSoft, Params.HandMin, Height)
				* (1.f - FMath::SmoothStep(Params.HandMax, Params.HandMax + HandSoft, Height));
			const float Top = FMath::Clamp(NZ * 0.7f + 0.5f, 0.f, 1.f);
			float Handling = S.Slot->bRail
				? Top * (0.6f + 0.4f * Band)
				: Band * S.Convex * Params.GeneralHandling;
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
			Dust = FMath::Max(Dust, S.Concave * Params.CreviceDust);
			Dust *= FMath::Lerp(0.6f, 1.f, Fbm(NP * 0.8, Params.Seed + 23u));
			Dust *= FMath::Clamp(1.2f + NZ, 0.f, 1.f); // little on undersides
			Dust *= 1.f - Handling * 0.8f;              // hands keep it clean
			Dust *= Params.DustIntensity * Profile.Dust;

			// Runoff: streaks down from vents and leaks, plus faint general streaking on walls.
			float Runoff = 0.f;
			for (const FGrungifyRunoffSource& Source : Params.Sources)
			{
				Runoff = FMath::Max(Runoff, RunoffFromSource(Source, S.P, N, Params));
			}
			if (Params.AmbientStreaks > 0.f)
			{
				const float Vertical = 1.f - FMath::Abs(NZ);
				const FVector StreakP(NP.X * 3.0, NP.Y * 3.0, NP.Z * 0.2);
				Runoff = FMath::Max(Runoff, Vertical * Params.AmbientStreaks * FMath::SmoothStep(0.45f, 0.8f, Fbm(StreakP, Params.Seed + 37u)));
			}
			Runoff *= FMath::Clamp(1.f + NZ * 1.5f, 0.f, 1.f) * Params.RunoffIntensity * Profile.Runoff;

			// Edge chips: convex edges, more where they face into the wear direction or get kicked.
			const float Exposure = FMath::Lerp(1.f, FMath::Clamp(0.25f + 0.75f * float(FVector::DotProduct(N, -Params.WearDir)), 0.f, 1.f), Params.WearInfluence);
			const float Kick = (1.f - FMath::SmoothStep(0.f, FMath::Max(Params.FloorKickHeight, 1.f), Height)) * 0.5f;
			float Chips = S.Convex * FMath::Min(Exposure + Kick, 1.f);
			Chips *= FMath::Lerp(0.35f, 1.f, Fbm(NP * 1.5, Params.Seed + 51u)) * Params.ChipIntensity * Profile.EdgeChips;

			const float K = Params.Intensity * ZoneW * Gate;
			return FLinearColor(
				FMath::Clamp(Chips * K, 0.f, 1.f),
				FMath::Clamp(Dust * K, 0.f, 1.f),
				FMath::Clamp(Runoff * K, 0.f, 1.f),
				FMath::Clamp(Handling * K, 0.f, 1.f));
		}

		FColor ToColor(const FLinearColor& M)
		{
			return FColor(Quantize(M.R), Quantize(M.G), Quantize(M.B), Quantize(M.A));
		}

		struct FMaskStats
		{
			FLinearColor Sum = FLinearColor(0.f, 0.f, 0.f, 0.f);
			FLinearColor Max = FLinearColor(0.f, 0.f, 0.f, 0.f);
			int64 Count = 0;

			void Add(const FColor& C)
			{
				const FLinearColor V(C.R / 255.f, C.G / 255.f, C.B / 255.f, C.A / 255.f);
				Sum += V;
				Max = FLinearColor(FMath::Max(Max.R, V.R), FMath::Max(Max.G, V.G), FMath::Max(Max.B, V.B), FMath::Max(Max.A, V.A));
				++Count;
			}

			void WriteTo(FGrungifyComponentResult& Result) const
			{
				Result.MeanMasks = Count > 0 ? Sum / float(Count) : FLinearColor(0.f, 0.f, 0.f, 0.f);
				Result.MaxMasks = Max;
			}
		};

		/** World-space mesh with welded topology, per-vertex edge convexity and per-triangle slot. */
		struct FTopology
		{
			int32 NumVerts = 0;
			TArray<FVector> Pos;
			TArray<FVector> Nrm;
			TArray<int32> Weld;
			TArray<int32> WeldFirst;
			TArray<uint32> Indices;
			TArray<FVector> FaceN;
			TArray<int32> TriSlot;
			TArray<float> Convex;
			TArray<float> Concave;
			double Area = 0.0;

			bool Build(const FStaticMeshLODResources& LOD, const FTransform& Transform, float SharpRef)
			{
				const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
				const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
				NumVerts = int32(LOD.GetNumVertices());
				LOD.IndexBuffer.GetCopy(Indices);
				if (NumVerts == 0 || Indices.Num() < 3 || int32(Positions.GetNumVertices()) != NumVerts || int32(Attributes.GetNumVertices()) != NumVerts)
				{
					return false;
				}

				const FVector Scale = Transform.GetScale3D();
				const FVector InvScale(
					FMath::IsNearlyZero(Scale.X) ? 0.0 : 1.0 / Scale.X,
					FMath::IsNearlyZero(Scale.Y) ? 0.0 : 1.0 / Scale.Y,
					FMath::IsNearlyZero(Scale.Z) ? 0.0 : 1.0 / Scale.Z);

				Pos.SetNumUninitialized(NumVerts);
				Nrm.SetNumUninitialized(NumVerts);
				Weld.SetNumUninitialized(NumVerts);
				TMap<FIntVector, int32> WeldMap;
				WeldMap.Reserve(NumVerts);
				WeldFirst.Reset();

				for (int32 V = 0; V < NumVerts; ++V)
				{
					const FVector3f L = Positions.VertexPosition(V);
					const FVector4f N = Attributes.VertexTangentZ(V);
					Pos[V] = Transform.TransformPosition(FVector(L.X, L.Y, L.Z));
					Nrm[V] = Transform.TransformVectorNoScale(FVector(N.X, N.Y, N.Z) * InvScale).GetSafeNormal();

					// Weld split vertices (hard edges, UV seams) so edge detection sees the real topology.
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

				TriSlot.Init(0, NumTris);
				for (const FStaticMeshSection& Section : LOD.Sections)
				{
					const int32 FirstTri = int32(Section.FirstIndex / 3);
					const int32 LastTri = FMath::Min(FirstTri + int32(Section.NumTriangles), NumTris);
					for (int32 T = FirstTri; T < LastTri; ++T)
					{
						TriSlot[T] = Section.MaterialIndex;
					}
				}

				// Area-weighted average normal per welded vertex.
				FaceN.SetNumZeroed(NumTris);
				TArray<FVector> AvgNormal;
				AvgNormal.Init(FVector::ZeroVector, NumWelded);
				Area = 0.0;

				for (int32 T = 0; T < NumTris; ++T)
				{
					const uint32 I0 = Indices[T * 3 + 0];
					const uint32 I1 = Indices[T * 3 + 1];
					const uint32 I2 = Indices[T * 3 + 2];
					if (I0 >= uint32(NumVerts) || I1 >= uint32(NumVerts) || I2 >= uint32(NumVerts))
					{
						continue;
					}

					FVector Cross = FVector::CrossProduct(Pos[I1] - Pos[I0], Pos[I2] - Pos[I0]);
					if (Cross.SizeSquared() < UE_SMALL_NUMBER)
					{
						continue;
					}
					// Orient by the authored normals so winding / mirroring doesn't matter.
					if (FVector::DotProduct(Cross, Nrm[I0] + Nrm[I1] + Nrm[I2]) < 0.0)
					{
						Cross = -Cross;
					}
					FaceN[T] = Cross.GetUnsafeNormal();
					Area += Cross.Size() * 0.5;

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
					if (FaceN[T].IsZero())
					{
						continue;
					}
					const int32 W[3] = { Weld[Indices[T * 3 + 0]], Weld[Indices[T * 3 + 1]], Weld[Indices[T * 3 + 2]] };
					for (int32 K = 0; K < 3; ++K)
					{
						const int32 Wk = W[K];
						Sharp[Wk] = FMath::Max(Sharp[Wk], float(1.0 - FVector::DotProduct(FaceN[T], AvgNormal[Wk])));

						const FVector& Pk = Pos[WeldFirst[Wk]];
						for (int32 J = 0; J < 3; ++J)
						{
							if (W[J] == Wk)
							{
								continue;
							}
							const FVector Dir = (Pos[WeldFirst[W[J]]] - Pk).GetSafeNormal();
							if (!Dir.IsZero())
							{
								SideSum[Wk] += float(FVector::DotProduct(AvgNormal[Wk], Dir));
								++SideCount[Wk];
							}
						}
					}
				}

				Convex.SetNumUninitialized(NumWelded);
				Concave.SetNumUninitialized(NumWelded);
				for (int32 Wi = 0; Wi < NumWelded; ++Wi)
				{
					const float SharpN = FMath::Clamp(Sharp[Wi] / SharpRef, 0.f, 1.f);
					const float Side = SideCount[Wi] > 0 ? SideSum[Wi] / SideCount[Wi] : 0.f;
					Convex[Wi] = SharpN * FMath::Clamp(-Side * 3.f, 0.f, 1.f);
					Concave[Wi] = SharpN * FMath::Clamp(Side * 3.f, 0.f, 1.f);
				}
				return true;
			}
		};

		/** Distance to the mesh's creased edges, so texture masks get crisp edge wear even on sparse geometry. */
		class FEdgeField
		{
		public:
			void Build(const FTopology& Topo, float EdgeAngleDeg, float InWidth)
			{
				Width = FMath::Max(InWidth, 0.5f);
				CellSize = Width;
				const float BendRef = 1.f - FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(EdgeAngleDeg, 5.f, 170.f)));

				// Welded edge -> the (up to two) faces that share it.
				TMap<uint64, FIntPoint> EdgeFaces;
				const int32 NumTris = Topo.FaceN.Num();
				for (int32 T = 0; T < NumTris; ++T)
				{
					if (Topo.FaceN[T].IsZero())
					{
						continue;
					}
					for (int32 K = 0; K < 3; ++K)
					{
						const uint32 A = uint32(Topo.Weld[Topo.Indices[T * 3 + K]]);
						const uint32 B = uint32(Topo.Weld[Topo.Indices[T * 3 + (K + 1) % 3]]);
						if (A == B)
						{
							continue;
						}
						const uint64 Key = (uint64(FMath::Min(A, B)) << 32) | uint64(FMath::Max(A, B));
						if (FIntPoint* Faces = EdgeFaces.Find(Key))
						{
							Faces->Y = Faces->Y == -1 ? T : -2; // -2: non-manifold, ignored
						}
						else
						{
							EdgeFaces.Add(Key, FIntPoint(T, -1));
						}
					}
				}

				for (const TPair<uint64, FIntPoint>& Pair : EdgeFaces)
				{
					const int32 F1 = Pair.Value.X;
					const int32 F2 = Pair.Value.Y;
					if (F2 < 0)
					{
						continue; // open border or non-manifold
					}
					const FVector& N1 = Topo.FaceN[F1];
					const FVector& N2 = Topo.FaceN[F2];
					const float Bend = float(1.0 - FVector::DotProduct(N1, N2));
					if (Bend < 0.02f)
					{
						continue;
					}

					FFeatureEdge Edge;
					Edge.A = Topo.Pos[Topo.WeldFirst[int32(Pair.Key >> 32)]];
					Edge.B = Topo.Pos[Topo.WeldFirst[int32(Pair.Key & 0xffffffffu)]];
					const FVector Mid = (Edge.A + Edge.B) * 0.5;
					const FVector C2 = (Topo.Pos[Topo.Indices[F2 * 3]] + Topo.Pos[Topo.Indices[F2 * 3 + 1]] + Topo.Pos[Topo.Indices[F2 * 3 + 2]]) / 3.0;
					Edge.bConvex = FVector::DotProduct(N1, C2 - Mid) < 0.0;
					Edge.Strength = FMath::Clamp(Bend / BendRef, 0.f, 1.f);

					const int32 EdgeIndex = Edges.Add(Edge);
					Insert(EdgeIndex);
				}
			}

			void Query(const FVector& P, float& OutConvex, float& OutConcave) const
			{
				OutConvex = 0.f;
				OutConcave = 0.f;
				const TArray<int32>* List = Cells.Find(CellOf(P));
				if (!List)
				{
					return;
				}
				for (const int32 EdgeIndex : *List)
				{
					const FFeatureEdge& Edge = Edges[EdgeIndex];
					const float Dist = float(FMath::PointDistToSegment(P, Edge.A, Edge.B));
					if (Dist >= Width)
					{
						continue;
					}
					const float Value = Edge.Strength * (1.f - FMath::SmoothStep(Width * 0.35f, Width, Dist));
					float& Target = Edge.bConvex ? OutConvex : OutConcave;
					Target = FMath::Max(Target, Value);
				}
			}

		private:
			struct FFeatureEdge
			{
				FVector A;
				FVector B;
				float Strength = 0.f;
				bool bConvex = true;
			};

			TArray<FFeatureEdge> Edges;
			TMap<FIntVector, TArray<int32>> Cells;
			double CellSize = 8.0;
			float Width = 8.f;

			FIntVector CellOf(const FVector& P) const
			{
				return FIntVector(FMath::FloorToInt(P.X / CellSize), FMath::FloorToInt(P.Y / CellSize), FMath::FloorToInt(P.Z / CellSize));
			}

			/** Registers the edge in every cell within one cell of the segment, so a point's own cell lists every edge within Width. */
			void Insert(int32 EdgeIndex)
			{
				const FFeatureEdge& Edge = Edges[EdgeIndex];
				const double Length = FVector::Dist(Edge.A, Edge.B);
				const int32 Steps = FMath::Max(1, FMath::CeilToInt(Length / (CellSize * 0.5)));
				TSet<FIntVector> Touched;
				for (int32 Step = 0; Step <= Steps; ++Step)
				{
					const FIntVector Center = CellOf(FMath::Lerp(Edge.A, Edge.B, double(Step) / Steps));
					for (int32 Z = -1; Z <= 1; ++Z)
					{
						for (int32 Y = -1; Y <= 1; ++Y)
						{
							for (int32 X = -1; X <= 1; ++X)
							{
								const FIntVector Cell = Center + FIntVector(X, Y, Z);
								if (!Touched.Contains(Cell))
								{
									Touched.Add(Cell);
									Cells.FindOrAdd(Cell).Add(EdgeIndex);
								}
							}
						}
					}
				}
			}
		};

		FVector2f GateUVOf(const FGrungifySlotInfo& Slot, const FStaticMeshVertexBuffer& Attributes, uint32 Vertex)
		{
			if (!Slot.GateSampler || !Slot.Gate)
			{
				return FVector2f::ZeroVector;
			}
			const uint32 Channel = uint32(FMath::Clamp<int32>(Slot.Gate->GateUVChannel, 0, int32(Attributes.GetNumTexCoords()) - 1));
			return Attributes.GetVertexUV(Vertex, Channel);
		}

		const FGrungifySlotInfo* SlotFor(const FGrungifyComponentContext& Context, int32 MaterialIndex)
		{
			if (Context.Slots.IsValidIndex(MaterialIndex))
			{
				return &Context.Slots[MaterialIndex];
			}
			return Context.Slots.Num() > 0 ? &Context.Slots[0] : nullptr;
		}

		void BakeVertexColors(const FStaticMeshRenderData& RenderData, const FTransform& Transform, const FGrungifyComponentContext& Context,
			const FGrungifyGenerationParams& Params, float SharpRef, FGrungifyComponentResult& Out, int32 (&SurfaceVotes)[3])
		{
			Out.LODColors.SetNum(RenderData.LODResources.Num());
			FMaskStats Stats;

			for (int32 LODIndex = 0; LODIndex < RenderData.LODResources.Num(); ++LODIndex)
			{
				const FStaticMeshLODResources& LOD = RenderData.LODResources[LODIndex];
				FTopology Topo;
				if (!Topo.Build(LOD, Transform, SharpRef))
				{
					continue;
				}

				// Each vertex takes the slot of a triangle that uses it.
				TArray<int32> VertSlot;
				VertSlot.Init(0, Topo.NumVerts);
				for (int32 T = 0; T < Topo.TriSlot.Num(); ++T)
				{
					for (int32 K = 0; K < 3; ++K)
					{
						const uint32 V = Topo.Indices[T * 3 + K];
						if (V < uint32(Topo.NumVerts))
						{
							VertSlot[V] = Topo.TriSlot[T];
						}
					}
				}

				const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
				TArray<FColor>& Colors = Out.LODColors[LODIndex];
				Colors.SetNumUninitialized(Topo.NumVerts);

				for (int32 V = 0; V < Topo.NumVerts; ++V)
				{
					FPointSample S;
					S.P = Topo.Pos[V];
					S.N = Topo.Nrm[V];
					S.Convex = Topo.Convex[Topo.Weld[V]];
					S.Concave = Topo.Concave[Topo.Weld[V]];
					S.Slot = SlotFor(Context, VertSlot[V]);
					if (S.Slot)
					{
						S.GateUV = GateUVOf(*S.Slot, Attributes, uint32(V));
					}
					Colors[V] = ToColor(EvaluatePoint(S, Params));

					if (LODIndex == 0)
					{
						Stats.Add(Colors[V]);
						if (S.Slot)
						{
							++SurfaceVotes[FMath::Clamp(int32(S.Slot->Surface) - 1, 0, 2)];
						}
					}
				}
			}

			Out.Vertices = RenderData.LODResources[0].GetNumVertices();
			Stats.WriteTo(Out);
		}

		bool BakeTexture(const FStaticMeshLODResources& LOD, const FTopology& Topo, const FGrungifyComponentContext& Context,
			const FGrungifyGenerationParams& Params, FGrungifyComponentResult& Out, int32 (&SurfaceVotes)[3])
		{
			const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
			const uint32 MaskUV = uint32(Params.MaskUVChannel);

			// Resolution from the physical texel size, assuming ~60% of the UV square is used.
			const double TexelSize = FMath::Max<double>(Params.MaskTexelSize, 0.25);
			const double Texels = Topo.Area / (TexelSize * TexelSize) / 0.6;
			const int32 Wanted = FMath::CeilToInt(FMath::Sqrt(FMath::Max(Texels, 1.0)));
			const int32 Size = FMath::Clamp(int32(FMath::RoundUpToPowerOfTwo(uint32(FMath::Max(Wanted, 1)))), 32, FMath::Max(Params.MaxMaskResolution, 32));
			const int32 NumPixels = Size * Size;

			TArray<FVector2D> UV;
			UV.SetNumUninitialized(Topo.NumVerts);
			for (int32 V = 0; V < Topo.NumVerts; ++V)
			{
				const FVector2f T = Attributes.GetVertexUV(uint32(V), MaskUV);
				UV[V] = FVector2D(T.X * Size, T.Y * Size);
			}

			// Pass 1 (serial, deterministic): each texel belongs to the first triangle that covers its centre.
			TArray<int32> Owner;
			Owner.Init(INDEX_NONE, NumPixels);
			TArray<FVector3f> Bary;
			Bary.SetNumUninitialized(NumPixels);

			const int32 NumTris = Topo.FaceN.Num();
			for (int32 T = 0; T < NumTris; ++T)
			{
				if (Topo.FaceN[T].IsZero())
				{
					continue;
				}
				const FVector2D& P0 = UV[Topo.Indices[T * 3 + 0]];
				const FVector2D& P1 = UV[Topo.Indices[T * 3 + 1]];
				const FVector2D& P2 = UV[Topo.Indices[T * 3 + 2]];
				const float Area2 = Cross2(P1 - P0, P2 - P0);
				if (FMath::Abs(Area2) < 1e-8f)
				{
					continue;
				}

				const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.X, P1.X, P2.X)), 0, Size - 1);
				const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.X, P1.X, P2.X)), 0, Size - 1);
				const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.Y, P1.Y, P2.Y)), 0, Size - 1);
				const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.Y, P1.Y, P2.Y)), 0, Size - 1);

				for (int32 Y = MinY; Y <= MaxY; ++Y)
				{
					for (int32 X = MinX; X <= MaxX; ++X)
					{
						const int32 Pixel = Y * Size + X;
						if (Owner[Pixel] != INDEX_NONE)
						{
							continue;
						}
						const FVector2D C(X + 0.5, Y + 0.5);
						const float W0 = Cross2(P2 - P1, C - P1) / Area2;
						const float W1 = Cross2(P0 - P2, C - P2) / Area2;
						const float W2 = 1.f - W0 - W1;
						if (W0 >= -1e-4f && W1 >= -1e-4f && W2 >= -1e-4f)
						{
							Owner[Pixel] = T;
							Bary[Pixel] = FVector3f(W0, W1, W2);
						}
					}
				}
			}

			FEdgeField Edges;
			Edges.Build(Topo, Params.EdgeAngleDeg, Params.EdgeWearWidth);

			// Pass 2 (parallel over rows): evaluate every covered texel.
			Out.TexturePixels.Init(FColor(0, 0, 0, 0), NumPixels);
			ParallelFor(Size, [&](int32 Y)
			{
				for (int32 X = 0; X < Size; ++X)
				{
					const int32 Pixel = Y * Size + X;
					const int32 T = Owner[Pixel];
					if (T == INDEX_NONE)
					{
						continue;
					}
					const uint32 I0 = Topo.Indices[T * 3 + 0];
					const uint32 I1 = Topo.Indices[T * 3 + 1];
					const uint32 I2 = Topo.Indices[T * 3 + 2];
					const FVector3f& W = Bary[Pixel];

					FPointSample S;
					S.P = Topo.Pos[I0] * W.X + Topo.Pos[I1] * W.Y + Topo.Pos[I2] * W.Z;
					S.N = (Topo.Nrm[I0] * W.X + Topo.Nrm[I1] * W.Y + Topo.Nrm[I2] * W.Z).GetSafeNormal();
					if (S.N.IsZero())
					{
						S.N = Topo.FaceN[T];
					}
					Edges.Query(S.P, S.Convex, S.Concave);
					S.Slot = SlotFor(Context, Topo.TriSlot[T]);
					if (S.Slot && S.Slot->GateSampler)
					{
						S.GateUV = GateUVOf(*S.Slot, Attributes, I0) * W.X + GateUVOf(*S.Slot, Attributes, I1) * W.Y + GateUVOf(*S.Slot, Attributes, I2) * W.Z;
					}
					Out.TexturePixels[Pixel] = ToColor(EvaluatePoint(S, Params));
				}
			});

			FMaskStats Stats;
			for (int32 Pixel = 0; Pixel < NumPixels; ++Pixel)
			{
				if (Owner[Pixel] != INDEX_NONE)
				{
					Stats.Add(Out.TexturePixels[Pixel]);
					if (const FGrungifySlotInfo* Slot = SlotFor(Context, Topo.TriSlot[Owner[Pixel]]))
					{
						++SurfaceVotes[FMath::Clamp(int32(Slot->Surface) - 1, 0, 2)];
					}
				}
			}
			Stats.WriteTo(Out);

			// Grow the islands a few texels so filtering and mips don't pull in black from the gutters.
			TArray<bool> Covered;
			Covered.SetNumUninitialized(NumPixels);
			for (int32 Pixel = 0; Pixel < NumPixels; ++Pixel)
			{
				Covered[Pixel] = Owner[Pixel] != INDEX_NONE;
			}
			for (int32 Pass = 0; Pass < 4; ++Pass)
			{
				TArray<bool> NextCovered = Covered;
				TArray<FColor> NextPixels = Out.TexturePixels;
				for (int32 Y = 0; Y < Size; ++Y)
				{
					for (int32 X = 0; X < Size; ++X)
					{
						const int32 Pixel = Y * Size + X;
						if (Covered[Pixel])
						{
							continue;
						}
						int32 Sum[4] = { 0, 0, 0, 0 };
						int32 Count = 0;
						const FIntPoint Neighbours[4] = { { X - 1, Y }, { X + 1, Y }, { X, Y - 1 }, { X, Y + 1 } };
						for (const FIntPoint& Nb : Neighbours)
						{
							if (Nb.X < 0 || Nb.Y < 0 || Nb.X >= Size || Nb.Y >= Size || !Covered[Nb.Y * Size + Nb.X])
							{
								continue;
							}
							const FColor& C = Out.TexturePixels[Nb.Y * Size + Nb.X];
							Sum[0] += C.R; Sum[1] += C.G; Sum[2] += C.B; Sum[3] += C.A;
							++Count;
						}
						if (Count > 0)
						{
							NextPixels[Pixel] = FColor(uint8(Sum[0] / Count), uint8(Sum[1] / Count), uint8(Sum[2] / Count), uint8(Sum[3] / Count));
							NextCovered[Pixel] = true;
						}
					}
				}
				Covered = MoveTemp(NextCovered);
				Out.TexturePixels = MoveTemp(NextPixels);
			}

			Out.TextureSize = Size;
			Out.Vertices = Topo.NumVerts;
			return true;
		}
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

		const FTransform& Transform = Component->GetComponentTransform();
		const float SharpRef = 1.f - FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Params.EdgeAngleDeg, 5.f, 170.f) * 0.5f));
		int32 SurfaceVotes[3] = { 0, 0, 0 };

		// Decide the output from LOD0 (for Nanite meshes this is the fallback mesh, which shares the UV layout).
		const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];
		FTopology Topo0;
		if (!Topo0.Build(LOD0, Transform, SharpRef))
		{
			return false;
		}

		const bool bHasMaskUV = int32(LOD0.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()) > Params.MaskUVChannel;
		const double AreaM2 = FMath::Max(Topo0.Area / 10000.0, 1e-3);
		const double CellsPerM2 = FMath::Square(100.0 / FMath::Max<double>(Params.FeatureScale, 1.0));
		const bool bSparse = Topo0.NumVerts / AreaM2 < CellsPerM2;

		bool bTexture = false;
		switch (Params.MaskOutput)
		{
		case EGrungifyMaskOutput::Texture:
			bTexture = bHasMaskUV;
			if (!bHasMaskUV)
			{
				OutResult.Notes += FString::Printf(TEXT("No UV channel %d for the mask texture; used vertex colours. "), Params.MaskUVChannel);
			}
			break;
		case EGrungifyMaskOutput::Auto:
			bTexture = bHasMaskUV && (OutResult.bNanite || bSparse);
			if (!bHasMaskUV && (OutResult.bNanite || bSparse))
			{
				OutResult.Notes += FString::Printf(TEXT("%s mesh but no UV channel %d; used vertex colours. "), OutResult.bNanite ? TEXT("Nanite") : TEXT("Sparse"), Params.MaskUVChannel);
			}
			break;
		default:
			break;
		}
		if (bSparse && !bTexture)
		{
			OutResult.Notes += TEXT("Sparse geometry: vertex-colour detail will be coarse. ");
		}

		if (bTexture)
		{
			OutResult.Output = EGrungifyMaskOutput::Texture;
			BakeTexture(LOD0, Topo0, Context, Params, OutResult, SurfaceVotes);
		}
		else
		{
			OutResult.Output = EGrungifyMaskOutput::VertexColor;
			BakeVertexColors(*RenderData, Transform, Context, Params, SharpRef, OutResult, SurfaceVotes);
			if (OutResult.bNanite)
			{
				OutResult.Notes += TEXT("Nanite mesh with vertex-colour masks: may not render. ");
			}
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
		return true;
	}
}

#endif // WITH_EDITOR
