// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyGenerator.h"

#if WITH_EDITOR

#include "Async/ParallelFor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"
#endif

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

		void ApplyStamp(FLinearColor& Primary, FLinearColor* Extended, const FGrungifyStampLayers& Layers, float Weight)
		{
			Primary.R = FMath::Clamp(Primary.R + Weight * Layers.EdgeChips, 0.f, 1.f);
			Primary.G = FMath::Clamp(Primary.G + Weight * Layers.Dust, 0.f, 1.f);
			Primary.B = FMath::Clamp(Primary.B + Weight * Layers.Runoff, 0.f, 1.f);
			Primary.A = FMath::Clamp(Primary.A + Weight * Layers.Handling, 0.f, 1.f);
			if (Extended)
			{
				Extended->R = FMath::Clamp(Extended->R + Weight * Layers.Moisture, 0.f, 1.f);
				Extended->G = FMath::Clamp(Extended->G + Weight * Layers.Soot, 0.f, 1.f);
				Extended->B = FMath::Clamp(Extended->B + Weight * Layers.Scorch, 0.f, 1.f);
				Extended->A = FMath::Clamp(Extended->A + Weight * Layers.Moss, 0.f, 1.f);
			}
		}

		/**
		 * Chips, dust, runoff, handling in 0..1 (R, G, B, A). With OutExtended, also moisture, soot, scorch, moss.
		 * Every optional input is guarded, so with them off the primary result is exactly the original one.
		 */
		FLinearColor EvaluatePoint(const FPointSample& S, const FGrungifyGenerationParams& Params, FLinearColor* OutExtended = nullptr)
		{
			const FLinearColor None(0.f, 0.f, 0.f, 0.f);
			if (OutExtended)
			{
				*OutExtended = None;
			}
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
			const FGrungifyExtendedLayers& Ext = Params.Extended;

			// Sky openness, sampled a little off the surface so it reads the air side.
			const bool bSky = Params.Exposure && Params.Exposure->IsValid() && Params.SkyInfluence > 0.f;
			const float Sky = bSky ? Params.Exposure->At(S.P + N * 40.0) : 1.f;

			// Handling: hands polish rails and, more lightly, edges at hand height.
			const float HandSoft = FMath::Max((Params.HandMax - Params.HandMin) * 0.25f, 5.f);
			const float Band = FMath::SmoothStep(Params.HandMin - HandSoft, Params.HandMin, Height)
				* (1.f - FMath::SmoothStep(Params.HandMax, Params.HandMax + HandSoft, Height));
			const float Top = FMath::Clamp(NZ * 0.7f + 0.5f, 0.f, 1.f);
			float Handling = S.Slot->bRail
				? Top * (0.6f + 0.4f * Band)
				: Band * S.Convex * Params.GeneralHandling;
			// Variation lives in the shader; the bake carries the structure at full strength.
			Handling *= Params.HandlingIntensity * Profile.Handling;
			Handling = FMath::Clamp(Handling, 0.f, 1.f);

			// Dust: thick at the base of walls, on ledges and packed into creases.
			const float UpFacing = FMath::Clamp((NZ - 0.6f) / 0.4f, 0.f, 1.f);
			const bool bOnFloor = UpFacing > 0.5f && Height < 5.f;
			// Falls off with height, but holds up longer than a plain square so it reads above the skirting.
			float Dust = 1.f - FMath::SmoothStep(0.f, FMath::Max(Params.DustHeight, 1.f), Height);
			Dust = FMath::Pow(Dust, 1.4f);
			if (bOnFloor)
			{
				Dust *= Params.FloorDust;
			}
			else
			{
				Dust = FMath::Max(Dust, UpFacing * Params.LedgeDust);
			}
			Dust = FMath::Max(Dust, S.Concave * Params.CreviceDust);
			Dust *= FMath::Clamp(1.2f + NZ, 0.f, 1.f); // little on undersides
			Dust *= 1.f - Handling * 0.8f;              // hands keep it clean
			Dust *= Params.DustIntensity * Profile.Dust;
			if (bSky)
			{
				// Sheltered corners collect more, wind-swept surfaces less.
				Dust *= FMath::Lerp(1.f, 1.3f - 0.6f * Sky, Params.SkyInfluence);
			}

			// Runoff: streaks down from vents and leaks, plus faint general streaking on walls.
			float Runoff = 0.f;
			for (const FGrungifyRunoffSource& Source : Params.Sources)
			{
				// Sources with a traced water path are handled by the flow field below.
				if (Params.Flow && Source.Path.Num() > 1)
				{
					continue;
				}
				Runoff = FMath::Max(Runoff, RunoffFromSource(Source, S.P, N, Params));
			}
			float FlowNear = 0.f;
			if (Params.Flow)
			{
				const double StreakScale = FMath::Max(Params.FeatureScale * 0.35, 1.0);
				const float Streak = Fbm(FVector(S.P.X / StreakScale, S.P.Y / StreakScale, S.P.Z / 120.0), Params.Seed + 71u);
				Params.Flow->ForEachNear(S.P, [&](const FGrungifySegmentField::FSegment& Seg, float Distance, float Along)
				{
					const float Edge = 1.f - FMath::SmoothStep(Seg.Radius * 0.5f, Seg.Radius, Distance);
					FlowNear = FMath::Max(FlowNear, Edge * Seg.Value);
					Runoff = FMath::Max(Runoff, Edge * Seg.Value * FMath::Lerp(1.f, 0.45f, Along) * FMath::Lerp(0.35f, 1.f, Streak));
				});
			}
			if (Params.AmbientStreaks > 0.f)
			{
				float Ambient = Params.AmbientStreaks;
				if (bSky)
				{
					Ambient *= FMath::Lerp(1.f, Sky, Params.SkyInfluence);
				}
				const float Vertical = 1.f - FMath::Abs(NZ);
				const FVector StreakP(NP.X * 3.0, NP.Y * 3.0, NP.Z * 0.2);
				Runoff = FMath::Max(Runoff, Vertical * Ambient * FMath::SmoothStep(0.45f, 0.8f, Fbm(StreakP, Params.Seed + 37u)));
			}
			Runoff *= FMath::Clamp(1.f + NZ * 1.5f, 0.f, 1.f) * Params.RunoffIntensity * Profile.Runoff;

			// Rust bloom: stains around fasteners on metal, with a short streak below each.
			if (Ext.RustBloomIntensity > 0.f && S.Slot->Surface == EGrungifySurfaceType::Metal)
			{
				const float R = FMath::Max(Ext.RustBloomRadiusCm, 1.f);
				for (const FVector& Fastener : Params.Fasteners)
				{
					const FVector Rel = S.P - Fastener;
					if (Rel.SizeSquared() > FMath::Square(R * 6.f))
					{
						continue;
					}
					const float Bloom = 1.f - FMath::SmoothStep(R * 0.3f, R, float(Rel.Size()));
					float Streak = 0.f;
					if (Rel.Z < 0.0 && -Rel.Z < R * 5.0)
					{
						const float Lateral = float(FVector(Rel.X, Rel.Y, 0.0).Size());
						Streak = (1.f - FMath::SmoothStep(R * 0.2f, R * 0.6f, Lateral)) * float(1.0 + Rel.Z / (R * 5.0)) * 0.7f;
					}
					Runoff = FMath::Max(Runoff, FMath::Max(Bloom, Streak) * Ext.RustBloomIntensity);
				}
			}

			// Edge chips: convex edges, more where they face into the wear direction or get kicked.
			float Exposure = FMath::Lerp(1.f, FMath::Clamp(0.25f + 0.75f * float(FVector::DotProduct(N, -Params.WearDir)), 0.f, 1.f), Params.WearInfluence);
			if (bSky)
			{
				Exposure *= FMath::Lerp(1.f, 0.4f + 0.6f * Sky, Params.SkyInfluence);
			}
			const float Kick = (1.f - FMath::SmoothStep(0.f, FMath::Max(Params.FloorKickHeight, 1.f), Height)) * 0.5f;
			float Chips = S.Convex * FMath::Min(Exposure + Kick, 1.f);
			Chips *= FMath::Lerp(0.35f, 1.f, Fbm(NP * 1.5, Params.Seed + 51u)) * Params.ChipIntensity * Profile.EdgeChips;

			// Traffic scuffs: floors along paths get polished (and swept of dust), wall bottoms beside them get kicked.
			if (Params.Traffic && Ext.ScuffIntensity > 0.f)
			{
				float FloorScuff = 0.f;
				float WallScuff = 0.f;
				const bool bWall = FMath::Abs(NZ) < 0.5f && Height < Ext.ScuffWallHeightCm;
				Params.Traffic->ForEachNear(S.P, [&](const FGrungifySegmentField::FSegment& Seg, float Distance, float /*Along*/)
				{
					// Segment radius is half the path width plus a 40 cm margin for the walls beside it.
					const float Half = FMath::Max(Seg.Radius - 40.f, 5.f);
					if (bOnFloor)
					{
						FloorScuff = FMath::Max(FloorScuff, (1.f - FMath::SmoothStep(Half * 0.5f, Half, Distance)) * Seg.Value);
					}
					else if (bWall)
					{
						WallScuff = FMath::Max(WallScuff, (1.f - FMath::SmoothStep(Half, Seg.Radius, Distance)) * Seg.Value);
					}
				});
				if (FloorScuff > 0.f)
				{
					const float Scuff = FloorScuff * Ext.ScuffIntensity * FMath::Lerp(0.5f, 1.f, Fbm(NP * 2.0, Params.Seed + 97u));
					Handling = FMath::Clamp(FMath::Max(Handling, Scuff), 0.f, 1.f);
					Dust *= 1.f - 0.7f * FMath::Clamp(Scuff, 0.f, 1.f);
				}
				if (WallScuff > 0.f)
				{
					const float Low = 1.f - Height / FMath::Max(Ext.ScuffWallHeightCm, 1.f);
					Chips = FMath::Max(Chips, WallScuff * Low * Ext.ScuffIntensity * FMath::Lerp(0.3f, 1.f, Fbm(NP * 1.7, Params.Seed + 101u)));
				}
			}

			const float K = Params.Intensity * ZoneW * Gate;
			FLinearColor Primary(
				FMath::Clamp(Chips * K, 0.f, 1.f),
				FMath::Clamp(Dust * K, 0.f, 1.f),
				FMath::Clamp(Runoff * K, 0.f, 1.f),
				FMath::Clamp(Handling * K, 0.f, 1.f));

			if (OutExtended && Params.bExtended)
			{
				// Moisture: pools, water paths, an old flood line, and damp sheltered ground.
				float Moisture = 0.f;
				for (const FGrungifyPool& Pool : Params.Pools)
				{
					const float Reach = Pool.Radius + Ext.PoolDampRadiusCm;
					const float Lateral = float(FVector(S.P.X - Pool.Center.X, S.P.Y - Pool.Center.Y, 0.0).Size());
					const float Above = float(S.P.Z - Pool.Center.Z);
					if (Lateral > Reach || Above < -20.f || Above > 60.f)
					{
						continue;
					}
					const float Spread = 1.f - FMath::SmoothStep(Pool.Radius, Reach, Lateral);
					Moisture = FMath::Max(Moisture, Above < 10.f ? Spread : Spread * (1.f - (Above - 10.f) / 50.f) * 0.8f);
				}
				Moisture = FMath::Max(Moisture, FlowNear * 0.5f);
				if (Ext.WaterLineHeightCm > 0.f)
				{
					const float Below = 1.f - FMath::SmoothStep(Ext.WaterLineHeightCm - 2.f, Ext.WaterLineHeightCm + 2.f, Height);
					const float Tide = 1.f - FMath::SmoothStep(0.f, 6.f, FMath::Abs(Height - Ext.WaterLineHeightCm));
					Moisture = FMath::Max(Moisture, FMath::Max(Below * 0.7f, Tide));
				}
				if (bSky)
				{
					Moisture = FMath::Max(Moisture, (1.f - Sky) * 0.25f * (1.f - FMath::SmoothStep(0.f, 100.f, Height)));
				}
				const float RawMoisture = Moisture;
				Moisture *= Ext.MoistureIntensity * FMath::Lerp(0.6f, 1.f, Fbm(NP * 0.7, Params.Seed + 83u));

				// Soot plumes rise and widen above lamps and fires; scorch hugs fires.
				float Soot = 0.f;
				float Scorch = 0.f;
				for (const FGrungifyHeatSource& Heat : Params.HeatSources)
				{
					const FVector Rel = S.P - Heat.Location;
					const float Facing = FVector::DotProduct(N, -Rel.GetSafeNormal()) > -0.2f ? 1.f : 0.3f;
					if (Ext.SootIntensity > 0.f && Rel.Z > -Ext.SootRadiusCm * 0.3f && Rel.Z < Ext.SootPlumeHeightCm)
					{
						const float Up = float(FMath::Max(Rel.Z, 0.0)) / Ext.SootPlumeHeightCm;
						const float Radius = Ext.SootRadiusCm * (0.6f + 0.8f * Up);
						const float Lateral = float(FVector(Rel.X, Rel.Y, 0.0).Size());
						Soot = FMath::Max(Soot, (1.f - FMath::SmoothStep(Radius * 0.4f, Radius, Lateral)) * (1.f - Up) * Facing);
					}
					if (Heat.bFire && Ext.ScorchIntensity > 0.f)
					{
						Scorch = FMath::Max(Scorch, (1.f - FMath::SmoothStep(Ext.ScorchRadiusCm * 0.3f, Ext.ScorchRadiusCm, float(Rel.Size()))) * Facing);
					}
				}
				Soot *= Ext.SootIntensity * FMath::Lerp(0.6f, 1.f, Fbm(NP * 0.9, Params.Seed + 87u));
				Scorch *= Ext.ScorchIntensity * FMath::Lerp(0.5f, 1.f, Fbm(NP * 1.4, Params.Seed + 89u));

				// Moss: damp, sheltered, north-facing stone and trim; never on metal or where hands keep it clean.
				float Moss = 0.f;
				if (Ext.MossIntensity > 0.f && S.Slot->Surface != EGrungifySurfaceType::Metal && NZ > -0.3f)
				{
					const float Yaw = FMath::DegreesToRadians(Ext.NorthYawDegrees);
					const FVector North(FMath::Cos(Yaw), FMath::Sin(Yaw), 0.0);
					const float Northness = UpFacing > 0.5f ? 0.7f : FMath::Clamp(float(FVector::DotProduct(N, North)) * 0.5f + 0.5f, 0.f, 1.f);
					const float Facing = FMath::Lerp(1.f, Northness, Ext.MossNorthBias);
					const float Damp = FMath::Clamp(0.25f + RawMoisture + (bSky ? (1.f - Sky) * 0.5f : 0.2f), 0.f, 1.f);
					Moss = Ext.MossIntensity * Facing * Damp * FMath::SmoothStep(0.35f, 0.7f, Fbm(NP * 0.6, Params.Seed + 91u)) * (1.f - Handling);
				}

				*OutExtended = FLinearColor(
					FMath::Clamp(Moisture * K, 0.f, 1.f),
					FMath::Clamp(Soot * K, 0.f, 1.f),
					FMath::Clamp(Scorch * K, 0.f, 1.f),
					FMath::Clamp(Moss * K, 0.f, 1.f));
			}

			// Hand-placed stamps add or erase on top of the procedural result.
			for (const FGrungifyStampData& Stamp : Params.Stamps)
			{
				const float Weight = StampWeight(Stamp, S.P, Params.FeatureScale);
				if (Weight > 0.f)
				{
					ApplyStamp(Primary, (OutExtended && Params.bExtended) ? OutExtended : nullptr, Stamp.Layers, Weight * ZoneW * Gate);
				}
			}
			return Primary;
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

		/** Area-uniform random points on LOD0 (deterministic per mesh and seed), evaluated with every layer. */
		void CollectScatterSamples(const FTopology& Topo, const FStaticMeshVertexBuffer& Attributes, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params,
			FGrungifyComponentResult& Out, uint32 MeshSeed)
		{
			constexpr int32 MaxSamples = 4000;
			const int32 NumTris = Topo.FaceN.Num();
			for (int32 T = 0; T < NumTris && Out.ScatterSamples.Num() < MaxSamples; ++T)
			{
				if (Topo.FaceN[T].IsZero())
				{
					continue;
				}
				if ((T & 1023) == 0 && Params.CancelRequested())
				{
					Out.bCancelled = true;
					return;
				}
				const uint32 I0 = Topo.Indices[T * 3 + 0];
				const uint32 I1 = Topo.Indices[T * 3 + 1];
				const uint32 I2 = Topo.Indices[T * 3 + 2];
				const double AreaM2 = FVector::CrossProduct(Topo.Pos[I1] - Topo.Pos[I0], Topo.Pos[I2] - Topo.Pos[I0]).Size() * 0.5 / 10000.0;
				const double Expected = AreaM2 * Params.ScatterDensity;
				int32 Count = FMath::FloorToInt(Expected);
				if (Hash3(T, 0, 1, MeshSeed) < Expected - Count)
				{
					++Count;
				}

				for (int32 K = 0; K < Count && Out.ScatterSamples.Num() < MaxSamples; ++K)
				{
					float U = Hash3(T, K, 2, MeshSeed);
					float V = Hash3(T, K, 3, MeshSeed);
					if (U + V > 1.f)
					{
						U = 1.f - U;
						V = 1.f - V;
					}
					const float W = 1.f - U - V;

					FPointSample S;
					S.P = Topo.Pos[I0] * W + Topo.Pos[I1] * U + Topo.Pos[I2] * V;
					S.N = Topo.FaceN[T];
					S.Convex = Topo.Convex[Topo.Weld[I0]] * W + Topo.Convex[Topo.Weld[I1]] * U + Topo.Convex[Topo.Weld[I2]] * V;
					S.Concave = Topo.Concave[Topo.Weld[I0]] * W + Topo.Concave[Topo.Weld[I1]] * U + Topo.Concave[Topo.Weld[I2]] * V;
					S.Slot = SlotFor(Context, Topo.TriSlot[T]);
					if (!S.Slot || S.Slot->bExcluded)
					{
						continue;
					}
					S.GateUV = GateUVOf(*S.Slot, Attributes, I0) * W + GateUVOf(*S.Slot, Attributes, I1) * U + GateUVOf(*S.Slot, Attributes, I2) * V;

					FGrungifyComponentResult::FScatterSample Sample;
					Sample.Primary = EvaluatePoint(S, Params, &Sample.Extended);
					if (!Params.bExtended)
					{
						Sample.Extended = FLinearColor(0.f, 0.f, 0.f, 0.f);
					}
					if (FMath::Max(Sample.Primary.GetMax(), Sample.Extended.GetMax()) < 0.05f)
					{
						continue;
					}
					Sample.P = S.P;
					Sample.N = S.N;
					Sample.Surface = S.Slot->Surface;
					Sample.Random = Hash3(T, K, 4, MeshSeed);
					Sample.Random2 = Hash3(T, K, 5, MeshSeed);
					Out.ScatterSamples.Add(Sample);
				}
			}
		}

		/**
		 * Area-weighted coverage per slot and channel, read back from the finished masks (texture or vertex colours) at
		 * seven points per triangle. Done after the bake, so it never changes the masks themselves.
		 */
		void MeasureSlotCoverage(const FStaticMeshLODResources& LOD, const FTopology& Topo, int32 UVChannel, FGrungifyComponentResult& Out)
		{
			constexpr float Threshold = 0.1f;
			const int32 NumTris = Topo.Indices.Num() / 3;
			if (NumTris == 0 || Topo.TriSlot.Num() < NumTris)
			{
				return;
			}
			const bool bTexture = Out.Output == EGrungifyMaskOutput::Texture && Out.TextureSize > 0 && Out.TexturePixels.Num() == Out.TextureSize * Out.TextureSize;
			const bool bExtended = bTexture && Out.ExtendedPixels.Num() == Out.TexturePixels.Num();
			const TArray<FColor>* Vertex = !bTexture && Out.LODColors.Num() > 0 && Out.LODColors[0].Num() == Topo.NumVerts ? &Out.LODColors[0] : nullptr;
			if (!bTexture && !Vertex)
			{
				return;
			}
			const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
			if (bTexture && UVChannel >= int32(Attributes.GetNumTexCoords()))
			{
				return;
			}

			auto Unpack = [](const FColor& C, float* Values)
			{
				Values[0] = C.R / 255.f;
				Values[1] = C.G / 255.f;
				Values[2] = C.B / 255.f;
				Values[3] = C.A / 255.f;
			};
			auto TexelAt = [&Out](const TArray<FColor>& Pixels, const FVector2D& UV) -> const FColor&
			{
				const int32 Size = Out.TextureSize;
				const int32 X = FMath::Clamp(FMath::FloorToInt(UV.X * Size), 0, Size - 1);
				const int32 Y = FMath::Clamp(FMath::FloorToInt(UV.Y * Size), 0, Size - 1);
				return Pixels[Y * Size + X];
			};

			// Centroid, edge midpoints and points near each corner (barycentric weights).
			static const FVector3f Samples[7] = {
				{ 1.f / 3, 1.f / 3, 1.f / 3 }, { .5f, .5f, 0.f }, { 0.f, .5f, .5f }, { .5f, 0.f, .5f },
				{ .8f, .1f, .1f }, { .1f, .8f, .1f }, { .1f, .1f, .8f } };

			for (int32 T = 0; T < NumTris; ++T)
			{
				const int32 Slot = Topo.TriSlot[T];
				if (Slot < 0)
				{
					continue;
				}
				if (Out.SlotCoverage.Num() <= Slot)
				{
					Out.SlotCoverage.SetNum(Slot + 1);
				}
				FGrungifyComponentResult::FSlotCoverage& Coverage = Out.SlotCoverage[Slot];
				const uint32 I[3] = { Topo.Indices[T * 3], Topo.Indices[T * 3 + 1], Topo.Indices[T * 3 + 2] };
				const double Area = 0.5 * FVector::CrossProduct(Topo.Pos[I[1]] - Topo.Pos[I[0]], Topo.Pos[I[2]] - Topo.Pos[I[0]]).Size() / 10000.0;
				if (Area <= 0.0)
				{
					continue;
				}
				Coverage.Area += Area;
				const double Weight = Area / UE_ARRAY_COUNT(Samples);

				FVector2D UVs[3];
				if (bTexture)
				{
					for (int32 K = 0; K < 3; ++K)
					{
						const FVector2f UV = Attributes.GetVertexUV(I[K], uint32(UVChannel));
						UVs[K] = FVector2D(UV.X, UV.Y);
					}
				}
				for (const FVector3f& B : Samples)
				{
					float Values[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
					if (bTexture)
					{
						const FVector2D UV = UVs[0] * B.X + UVs[1] * B.Y + UVs[2] * B.Z;
						Unpack(TexelAt(Out.TexturePixels, UV), Values);
						if (bExtended)
						{
							Unpack(TexelAt(Out.ExtendedPixels, UV), Values + 4);
						}
					}
					else
					{
						float Corner[3][4];
						for (int32 K = 0; K < 3; ++K)
						{
							Unpack((*Vertex)[I[K]], Corner[K]);
						}
						for (int32 C = 0; C < 4; ++C)
						{
							Values[C] = Corner[0][C] * B.X + Corner[1][C] * B.Y + Corner[2][C] * B.Z;
						}
					}
					for (int32 C = 0; C < 8; ++C)
					{
						if (Values[C] > Threshold)
						{
							Coverage.Covered[C] += Weight;
							Coverage.Sum[C] += Weight * Values[C];
						}
						Coverage.Max[C] = FMath::Max(Coverage.Max[C], Values[C]);
					}
				}
			}
		}

		void BakeVertexColors(TConstArrayView<const FStaticMeshLODResources*> LODs, const FTransform& Transform, const FGrungifyComponentContext& Context,
			const FGrungifyGenerationParams& Params, float SharpRef, FGrungifyComponentResult& Out, int32 (&SurfaceVotes)[3])
		{
			Out.LODColors.SetNum(LODs.Num());
			FMaskStats Stats;

			for (int32 LODIndex = 0; LODIndex < LODs.Num(); ++LODIndex)
			{
				if (Params.CancelRequested())
				{
					Out.bCancelled = true;
					return;
				}
				const FStaticMeshLODResources& LOD = *LODs[LODIndex];
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

			Out.Vertices = LODs[0]->GetNumVertices();
			Stats.WriteTo(Out);
		}

		// The false specialization is used only by the conservation regression test.
		template<bool bSkipInactiveTexels = true>
		bool BakeTexture(const FStaticMeshLODResources& LOD, const FTopology& Topo, const FGrungifyComponentContext& Context,
			const FGrungifyGenerationParams& Params, FGrungifyComponentResult& Out, int32 (&SurfaceVotes)[3]
#if WITH_DEV_AUTOMATION_TESTS
			, TArray<int32>* OutEdgeQueriesPerRow = nullptr
#endif
		)
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

			if (Params.CancelRequested())
			{
				Out.bCancelled = true;
				return false;
			}

			FEdgeField Edges;
			Edges.Build(Topo, Params.EdgeAngleDeg, Params.EdgeWearWidth);

			// Pass 2 (parallel over rows): retain ownership of inactive texels, but avoid their edge queries.
			Out.TexturePixels.Init(FColor(0, 0, 0, 0), NumPixels);
			const bool bExtended = Params.bExtended;
			if (bExtended)
			{
				Out.ExtendedPixels.Init(FColor(0, 0, 0, 0), NumPixels);
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (OutEdgeQueriesPerRow)
			{
				OutEdgeQueriesPerRow->Init(0, Size);
			}
#endif
			// Rows are processed in batches so cancellation can be polled on the game thread between them.
			// Batching changes neither the order-independent per-texel results nor any output.
			auto ProcessRow = [&](int32 Y)
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
					S.Slot = SlotFor(Context, Topo.TriSlot[T]);
					if constexpr (bSkipInactiveTexels)
					{
						if (!S.Slot || S.Slot->bExcluded)
						{
							continue;
						}
					}
					S.P = Topo.Pos[I0] * W.X + Topo.Pos[I1] * W.Y + Topo.Pos[I2] * W.Z;
					S.N = (Topo.Nrm[I0] * W.X + Topo.Nrm[I1] * W.Y + Topo.Nrm[I2] * W.Z).GetSafeNormal();
					if (S.N.IsZero())
					{
						S.N = Topo.FaceN[T];
					}
					if (S.Slot && S.Slot->GateSampler)
					{
						S.GateUV = GateUVOf(*S.Slot, Attributes, I0) * W.X + GateUVOf(*S.Slot, Attributes, I1) * W.Y + GateUVOf(*S.Slot, Attributes, I2) * W.Z;
					}
					if constexpr (bSkipInactiveTexels)
					{
						// These are the same early-outs as EvaluatePoint, independent of convexity/concavity.
						// Do not change Owner: black first-owner texels must still block overlapping triangles,
						// contribute to statistics/votes, and remain covered during the four dilation passes.
						if (ZoneWeight(Params, S.P) <= 0.f)
						{
							continue;
						}
						const float Height = float(FMath::Max(0.0, S.P.Z - Params.Floor->FloorBelow(S.P)));
						if (GateWeight(*S.Slot, float(S.N.Z), Height, S.GateUV) <= 0.f)
						{
							continue;
						}
					}
					Edges.Query(S.P, S.Convex, S.Concave);
#if WITH_DEV_AUTOMATION_TESTS
					if (OutEdgeQueriesPerRow)
					{
						++(*OutEdgeQueriesPerRow)[Y];
					}
#endif
					FLinearColor Extended;
					Out.TexturePixels[Pixel] = ToColor(EvaluatePoint(S, Params, bExtended ? &Extended : nullptr));
					if (bExtended)
					{
						Out.ExtendedPixels[Pixel] = ToColor(Extended);
					}
				}
			};
			constexpr int32 RowsPerBatch = 32;
			for (int32 FirstRow = 0; FirstRow < Size; FirstRow += RowsPerBatch)
			{
				if (Params.CancelRequested())
				{
					Out.bCancelled = true;
					return false;
				}
				const int32 NumRows = FMath::Min(RowsPerBatch, Size - FirstRow);
				ParallelFor(NumRows, [&](int32 Row) { ProcessRow(FirstRow + Row); });
			}

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
				TArray<FColor> NextExtended = Out.ExtendedPixels;
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
						int32 ExtSum[4] = { 0, 0, 0, 0 };
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
							if (bExtended)
							{
								const FColor& E = Out.ExtendedPixels[Nb.Y * Size + Nb.X];
								ExtSum[0] += E.R; ExtSum[1] += E.G; ExtSum[2] += E.B; ExtSum[3] += E.A;
							}
							++Count;
						}
						if (Count > 0)
						{
							NextPixels[Pixel] = FColor(uint8(Sum[0] / Count), uint8(Sum[1] / Count), uint8(Sum[2] / Count), uint8(Sum[3] / Count));
							if (bExtended)
							{
								NextExtended[Pixel] = FColor(uint8(ExtSum[0] / Count), uint8(ExtSum[1] / Count), uint8(ExtSum[2] / Count), uint8(ExtSum[3] / Count));
							}
							NextCovered[Pixel] = true;
						}
					}
				}
				Covered = MoveTemp(NextCovered);
				Out.TexturePixels = MoveTemp(NextPixels);
				Out.ExtendedPixels = MoveTemp(NextExtended);
			}

			Out.TextureSize = Size;
			Out.TexelSizeCm = float(FMath::Sqrt(FMath::Max(Topo.Area, 1.0) / 0.6) / Size);
			Out.Vertices = Topo.NumVerts;
			return true;
		}
	}

#if WITH_DEV_AUTOMATION_TESTS
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyScatterGateTest, "Grungify.Generation.ScatterGateUV",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FGrungifyScatterGateTest::RunTest(const FString& Parameters)
	{
		FStaticMeshLODResources LOD(false);
		FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
		Attributes.SetUseFullPrecisionUVs(true);
		Attributes.Init(3, 2);
		const FVector2f UVs[] = { { 0.f, 0.f }, { 1.f, 0.f }, { 0.f, 1.f } };
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			Attributes.SetVertexUV(Index, 0, FVector2f::ZeroVector);
			Attributes.SetVertexUV(Index, 1, UVs[Index]);
		}
		FTopology Topo;
		Topo.Pos = { FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 400.0, 0.0) };
		Topo.Indices = { 0, 1, 2 };
		Topo.FaceN = { FVector::UpVector };
		Topo.TriSlot = { 0 };
		Topo.Weld = { 0, 1, 2 };
		Topo.Convex = { 0.f, 0.f, 0.f };
		Topo.Concave = { 0.f, 0.f, 0.f };
		FGrungifyFloorField Floor;
		Floor.BuildFlat(0.0);
		FGrungifyGenerationParams Params;
		Params.Floor = &Floor;
		Params.ZoneExtent = FVector(500.0, 500.0, 100.0);
		Params.DustIntensity = 2.f;
		// This is an upward floor face: default FloorDust=0 deliberately suppresses
		// its dust. Admit dust explicitly so the regression tests gate registration.
		Params.FloorDust = 1.f;
		Params.ScatterDensity = 50.f;
		FGrungifyComponentContext Context;
		Context.Slots.AddDefaulted();
		FGrungifyGate Gate;
		Gate.GateUVChannel = 1;
		UTexture2D* Texture = NewObject<UTexture2D>();
		uint8 Pixels[] = { 0, 255 };
		Texture->Source.Init(2, 1, 1, 1, TSF_G8, Pixels);
		Gate.GateTexture = Texture;
		FGrungifyGateSampler Sampler;
		FString Error;
		if (!TestTrue(TEXT("Scatter gate source readable"), Sampler.Init(Texture, Error))) { return false; }
		Context.Slots[0].Gate = &Gate;
		Context.Slots[0].GateSampler = &Sampler;
		FPointSample WhitePoint;
		WhitePoint.P = FVector(300.0, 50.0, 0.0);
		WhitePoint.N = FVector::UpVector;
		WhitePoint.Slot = &Context.Slots[0];
		WhitePoint.GateUV = FVector2f(0.75f, 0.125f);
		TestTrue(TEXT("White UV1 floor point has nonzero admitted dust before scatter"), EvaluatePoint(WhitePoint, Params).G > 0.05f);
		FPointSample BlackPoint = WhitePoint;
		BlackPoint.P.X = 100.0;
		BlackPoint.GateUV.X = 0.25f;
		TestEqual(TEXT("Black UV1 floor point suppresses all primary layers"), EvaluatePoint(BlackPoint, Params).GetMax(), 0.f);
		FGrungifyComponentResult Permitted;
		CollectScatterSamples(Topo, Attributes, Context, Params, Permitted, 123u);
		AddInfo(FString::Printf(TEXT("UV1 white-half scatter samples: %d"), Permitted.ScatterSamples.Num()));
		TestTrue(TEXT("White part of UV1 produces scatter even though UV0 and gate origin are black"), Permitted.ScatterSamples.Num() > 0);
		for (const FGrungifyComponentResult::FScatterSample& Sample : Permitted.ScatterSamples)
		{
			TestTrue(TEXT("No scatter appears in the black half of the registered gate"), Sample.P.X >= 200.0);
		}
		Gate.bInvertGate = true;
		FGrungifyComponentResult Inverted;
		CollectScatterSamples(Topo, Attributes, Context, Params, Inverted, 123u);
		AddInfo(FString::Printf(TEXT("Inverted UV1 white-half scatter samples: %d"), Inverted.ScatterSamples.Num()));
		TestTrue(TEXT("Inverted gate produces scatter on its newly permitted half"), Inverted.ScatterSamples.Num() > 0);
		for (const FGrungifyComponentResult::FScatterSample& Sample : Inverted.ScatterSamples)
		{
			TestTrue(TEXT("Inverted gate retains the same triangle UV registration"), Sample.P.X < 200.0);
		}
		return !HasAnyErrors();
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyTextureGateConservationTest, "Grungify.Generation.TextureGateOutputConservation",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FGrungifyTextureGateConservationTest::RunTest(const FString& Parameters)
	{
		// CPU-only fixture: two creased faces and a later triangle overlapping the excluded first face.
		// No assets, editor world, GPU resources or production mesh data are changed.
		FStaticMeshLODResources LOD(false); // Stack-owned: no intrusive reference for its destructor to release.
		FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
		Attributes.SetUseFullPrecisionUVs(true);
		Attributes.Init(9, 2);
		const FVector2f UVs[9] = {
			{ .05f, .05f }, { .95f, .05f }, { .05f, .95f },
			{ .95f, .05f }, { .95f, .95f }, { .05f, .95f },
			{ .05f, .05f }, { .95f, .05f }, { .05f, .95f }
		};
		for (uint32 V = 0; V < 9; ++V)
		{
			Attributes.SetVertexUV(V, 0, UVs[V]);
			Attributes.SetVertexUV(V, 1, UVs[V]);
		}

		FTopology Topo;
		Topo.NumVerts = 9;
		Topo.Pos = {
			{ 0., 0., 100. }, { 100., 0., 100. }, { 0., 100., 100. },
			{ 100., 0., 100. }, { 100., 0., 0. }, { 0., 0., 100. },
			{ 300., 0., 100. }, { 400., 0., 100. }, { 300., 100., 100. }
		};
		Topo.Nrm = {
			FVector::UpVector, FVector::UpVector, FVector::UpVector,
			FVector(0., -1., 0.), FVector(0., -1., 0.), FVector(0., -1., 0.),
			FVector::UpVector, FVector::UpVector, FVector::UpVector
		};
		Topo.Indices = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
		Topo.FaceN = { FVector::UpVector, FVector(0., -1., 0.), FVector::UpVector };
		Topo.TriSlot = { 0, 1, 1 };
		Topo.Weld = { 0, 1, 2, 1, 3, 0, 4, 5, 6 };
		Topo.WeldFirst = { 0, 1, 2, 4, 6, 7, 8 };
		Topo.Area = 15000.;

		FGrungifyFloorField Floor;
		Floor.BuildFlat(-100.);
		FGrungifyGenerationParams Params;
		Params.Floor = &Floor;
		Params.Seed = 1337;
		Params.ZoneTransform = FTransform::Identity;
		Params.ZoneExtent = FVector(500.);
		Params.ZoneFalloff = 20.f;
		Params.DustHeight = 600.f;
		Params.CreviceDust = .8f;
		Params.GeneralHandling = .5f;
		Params.MaskUVChannel = 1;
		Params.MaskTexelSize = .5f;
		Params.MaxMaskResolution = 64;

		FGrungifyComponentContext Context;
		Context.Slots.SetNum(2);
		Context.Slots[0].bExcluded = true;
		Context.Slots[1].Surface = EGrungifySurfaceType::Metal;
		FGrungifyGate Gate;
		Gate.GateUVChannel = 0;
		FGrungifyGateSampler Sampler;
		UTexture2D* Permission = NewObject<UTexture2D>();
		TArray<uint8> PermissionPixels;
		PermissionPixels.Init(0, 16 * 16);
		for (int32 Y = 10; Y < 16; ++Y)
		{
			for (int32 X = 10; X < 16; ++X)
			{
				PermissionPixels[Y * 16 + X] = 255;
			}
		}
		// Positive sub-byte output is still eligible; do not introduce a threshold or quantized skip.
		PermissionPixels[14 * 16 + 8] = 1;
		PermissionPixels[14 * 16 + 9] = 128;
		auto LoadPermission = [&]()
		{
			Permission->Source.Init(16, 16, 1, 1, TSF_G8, PermissionPixels.GetData());
			FString Error;
			return TestTrue(TEXT("Permission source readable"), Sampler.Init(Permission, Error));
		};

		auto Compare = [&](const TCHAR* Name, bool bExpectNoQueries, bool bExpectSomeQueries, bool bCheckDilation = false)
		{
			FGrungifyComponentResult Legacy, Optimized;
			int32 LegacyVotes[3] = { 0, 0, 0 };
			int32 OptimizedVotes[3] = { 0, 0, 0 };
			TArray<int32> LegacyQueries, OptimizedQueries;
			BakeTexture<false>(LOD, Topo, Context, Params, Legacy, LegacyVotes, &LegacyQueries);
			BakeTexture<true>(LOD, Topo, Context, Params, Optimized, OptimizedVotes, &OptimizedQueries);
			const FString Prefix(Name);
			TestTrue(Prefix + TEXT(": exact final RGBA including all four dilation passes"), Legacy.TexturePixels == Optimized.TexturePixels);
			TestTrue(Prefix + TEXT(": exact covered-texel mean"), Legacy.MeanMasks == Optimized.MeanMasks);
			TestTrue(Prefix + TEXT(": exact maxima"), Legacy.MaxMasks == Optimized.MaxMasks);
			TestEqual(Prefix + TEXT(": exact resolution"), Legacy.TextureSize, Optimized.TextureSize);
			TestEqual(Prefix + TEXT(": exact vertex count"), Legacy.Vertices, Optimized.Vertices);
			int32 LegacyCount = 0, OptimizedCount = 0;
			for (int32 Y = 0; Y < LegacyQueries.Num(); ++Y)
			{
				LegacyCount += LegacyQueries[Y];
				OptimizedCount += OptimizedQueries[Y];
			}
			for (int32 Surface = 0; Surface < 3; ++Surface)
			{
				TestEqual(Prefix + TEXT(": exact surface votes"), LegacyVotes[Surface], OptimizedVotes[Surface]);
			}
			TestTrue(Prefix + TEXT(": edge queries reduced"), OptimizedCount < LegacyCount);
			if (bExpectNoQueries)
			{
				TestEqual(Prefix + TEXT(": no inactive edge queries"), OptimizedCount, 0);
			}
			if (bExpectSomeQueries)
			{
				TestTrue(Prefix + TEXT(": permitted texels still evaluated"), OptimizedCount > 0);
				TestTrue(Prefix + TEXT(": fixture exercises nonzero wear"), Optimized.MaxMasks.GetMax() > 0.f);
			}
			if (bCheckDilation)
			{
				const int32 Gutter = (Optimized.TextureSize * 13 / 16) * Optimized.TextureSize + Optimized.TextureSize - 1;
				TestTrue(Prefix + TEXT(": test exercises nonzero dilation outside the UV island"), Optimized.TexturePixels[Gutter] != FColor(0, 0, 0, 0));
			}
			const int32 BlackOwner = (Optimized.TextureSize / 4) * Optimized.TextureSize + Optimized.TextureSize / 4;
			TestTrue(Prefix + TEXT(": excluded first owner blocks overlapping admitted triangle"), Optimized.TexturePixels[BlackOwner] == FColor(0, 0, 0, 0));
		};

		Compare(TEXT("Excluded slot without a permission texture"), false, true, true);
		if (!LoadPermission())
		{
			return false;
		}
		Context.Slots[1].Gate = &Gate;
		Context.Slots[1].GateSampler = &Sampler;
		Compare(TEXT("Narrow permission with fractional boundary values"), false, true, true);
		Gate.bInvertGate = true;
		Compare(TEXT("Inverted permission"), false, true);
		Gate.bInvertGate = false;
		PermissionPixels.Init(0, 16 * 16);
		if (!LoadPermission())
		{
			return false;
		}
		Compare(TEXT("All-zero permission"), true, false);
		PermissionPixels.Init(255, 16 * 16);
		if (!LoadPermission())
		{
			return false;
		}
		Gate.bInvertGate = true;
		Compare(TEXT("Inverted all-one permission"), true, false);
		Gate.bInvertGate = false;
		Context.Slots[1].GateSampler = nullptr;
		Gate.bUseFacingGate = true;
		Gate.MinUpFacing = .5f;
		Compare(TEXT("Facing permission"), true, false);
		Gate.bUseFacingGate = false;
		Gate.bUseHeightGate = true;
		Gate.MinHeightCm = 500.f;
		Compare(TEXT("Height permission"), true, false);
		Gate.bUseHeightGate = false;
		Params.ZoneExtent = FVector(1.);
		Compare(TEXT("Outside zone"), true, false);
		Params.ZoneExtent = FVector(500.);
		Context.Slots.Reset();
		Compare(TEXT("Missing slot"), true, false);
		return true;
	}
#endif // WITH_DEV_AUTOMATION_TESTS

	namespace
	{
		/** Area of the triangle clipped to 0..1, not the area of every triangle crossing a boundary. */
		double TriangleUVAreaInsideUnitSquare(const FVector2D (&UV)[3])
		{
			TArray<FVector2D, TInlineAllocator<8>> Polygon;
			TArray<FVector2D, TInlineAllocator<8>> Clipped;
			for (const FVector2D& P : UV)
			{
				Polygon.Add(P);
			}
			// Sutherland-Hodgman clipping against the four sides of the unit square.
			for (int32 Edge = 0; Edge < 4 && Polygon.Num() > 0; ++Edge)
			{
				const int32 Axis = Edge / 2;
				const bool bLower = (Edge % 2) == 0;
				const double Boundary = bLower ? 0.0 : 1.0;
				auto Coordinate = [Axis](const FVector2D& P) { return Axis == 0 ? P.X : P.Y; };
				auto Inside = [&](const FVector2D& P) { return bLower ? Coordinate(P) >= Boundary : Coordinate(P) <= Boundary; };
				Clipped.Reset();
				FVector2D Previous = Polygon.Last();
				bool bPreviousInside = Inside(Previous);
				for (const FVector2D& Current : Polygon)
				{
					const bool bCurrentInside = Inside(Current);
					if (bPreviousInside != bCurrentInside)
					{
						const double T = (Boundary - Coordinate(Previous)) / (Coordinate(Current) - Coordinate(Previous));
						FVector2D Intersection = Previous + (Current - Previous) * T;
						if (Axis == 0) { Intersection.X = Boundary; }
						else { Intersection.Y = Boundary; }
						Clipped.Add(Intersection);
					}
					if (bCurrentInside) { Clipped.Add(Current); }
					Previous = Current;
					bPreviousInside = bCurrentInside;
				}
				Swap(Polygon, Clipped);
			}
			if (Polygon.Num() < 3) { return 0.0; }
			double TwiceArea = 0.0;
			for (int32 K = 1; K + 1 < Polygon.Num(); ++K)
			{
				const FVector2D A = Polygon[K] - Polygon[0];
				const FVector2D B = Polygon[K + 1] - Polygon[0];
				TwiceArea += A.X * B.Y - A.Y * B.X;
			}
			return 0.5 * FMath::Abs(TwiceArea);
		}
	}

	FGrungifyUVReport ValidateMaskUVs(const FStaticMeshLODResources& LOD, const FTransform& Transform, int32 Channel, const FGrungifyGenerationParams* CancelParams)
	{
		FGrungifyUVReport Report;
		Report.Channel = Channel;
		Report.bChecked = true;

		const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
		const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
		const int32 NumChannels = int32(Attributes.GetNumTexCoords());
		if (Channel < 0 || Channel >= NumChannels)
		{
			Report.Verdict = TEXT("failed");
			Report.Problems.Add(FString::Printf(TEXT("mesh has %d UV channel(s); mask channel %d is missing (Project Settings > Plugins > Grungify > Mask UV Channel)"), NumChannels, Channel));
			return Report;
		}
		Report.bPresent = true;

		TArray<uint32> Indices;
		LOD.IndexBuffer.GetCopy(Indices);
		const uint32 NumVerts = Attributes.GetNumVertices();

		// Coarse raster of strict triangle interiors: texels claimed twice mean overlapping islands.
		// Shared edges are never counted twice because points exactly on an edge are not strictly inside.
		constexpr int32 Res = 256;
		TArray<uint8> Claims;
		Claims.Init(0, Res * Res);

		double UVArea = 0.0;
		double OutsideArea = 0.0;
		int32 NumTris = 0;
		int32 NumDegenerate = 0;
		double MinDensity = TNumericLimits<double>::Max();
		double MaxDensity = 0.0;

		for (int32 T = 0; T + 2 < Indices.Num(); T += 3)
		{
			// Long on dense meshes: stop promptly when a background run is cancelled (callers check and discard).
			if (CancelParams && (T % 3072) == 0 && CancelParams->CancelRequested())
			{
				return Report;
			}
			const uint32 I[3] = { Indices[T], Indices[T + 1], Indices[T + 2] };
			if (I[0] >= NumVerts || I[1] >= NumVerts || I[2] >= NumVerts || Positions.GetNumVertices() < NumVerts)
			{
				continue;
			}
			++NumTris;

			FVector2D UV[3];
			FVector P[3];
			for (int32 K = 0; K < 3; ++K)
			{
				const FVector2f Raw = Attributes.GetVertexUV(I[K], uint32(Channel));
				UV[K] = FVector2D(Raw.X, Raw.Y);
				const FVector3f L = Positions.VertexPosition(I[K]);
				P[K] = Transform.TransformPosition(FVector(L.X, L.Y, L.Z));
			}

			const FVector2D A = UV[1] - UV[0], B = UV[2] - UV[0];
			const double TriUVArea = 0.5 * FMath::Abs(A.X * B.Y - A.Y * B.X);
			const double TriWorldArea = 0.5 * FVector::CrossProduct(P[1] - P[0], P[2] - P[0]).Size();
			if (TriUVArea < 1e-12)
			{
				NumDegenerate += TriWorldArea > 1e-4 ? 1 : 0;
				continue;
			}
			UVArea += TriUVArea;
			OutsideArea += FMath::Clamp(TriUVArea - TriangleUVAreaInsideUnitSquare(UV), 0.0, TriUVArea);
			if (TriWorldArea > 1.0) // ignore sub-cm² slivers when measuring density spread
			{
				const double Density = TriWorldArea / TriUVArea;
				MinDensity = FMath::Min(MinDensity, Density);
				MaxDensity = FMath::Max(MaxDensity, Density);
			}

			const FVector2D S0 = UV[0] * Res, S1 = UV[1] * Res, S2 = UV[2] * Res;
			const float Area2 = Cross2(S1 - S0, S2 - S0);
			const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.X, S1.X, S2.X)), 0, Res - 1);
			const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.X, S1.X, S2.X)), 0, Res - 1);
			const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.Y, S1.Y, S2.Y)), 0, Res - 1);
			const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.Y, S1.Y, S2.Y)), 0, Res - 1);
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const FVector2D C(X + 0.5, Y + 0.5);
					const float W0 = Cross2(S2 - S1, C - S1) / Area2;
					const float W1 = Cross2(S0 - S2, C - S2) / Area2;
					const float W2 = 1.f - W0 - W1;
					if (W0 > 1e-4f && W1 > 1e-4f && W2 > 1e-4f)
					{
						uint8& Claim = Claims[Y * Res + X];
						Claim = uint8(FMath::Min<int32>(Claim + 1, 255));
					}
				}
			}
		}

		int32 Covered = 0;
		int32 Overlapped = 0;
		for (const uint8 Claim : Claims)
		{
			Covered += Claim > 0 ? 1 : 0;
			Overlapped += Claim > 1 ? 1 : 0;
		}

		Report.Coverage = float(Covered) / float(Res * Res);
		Report.Overlap = Covered > 0 ? float(Overlapped) / float(Covered) : 0.f;
		Report.OutOfRange = UVArea > 0.0 ? float(OutsideArea / UVArea) : 0.f;
		Report.Degenerate = NumTris > 0 ? float(NumDegenerate) / float(NumTris) : 0.f;
		Report.DensitySpread = (MaxDensity > 0.0 && MinDensity > 0.0) ? float(MaxDensity / MinDensity) : 0.f;

		bool bFailed = false;
		bool bWarn = false;
		auto Problem = [&Report](bool bSerious, bool& bFlag, const FString& Text)
		{
			bFlag = true;
			Report.Problems.Add(FString::Printf(TEXT("%s: %s"), bSerious ? TEXT("failed") : TEXT("warning"), *Text));
		};

		if (Covered == 0)
		{
			Problem(true, bFailed, TEXT("UVs cover no area"));
		}
		if (Report.Overlap > 0.05f)
		{
			Problem(true, bFailed, FString::Printf(TEXT("%.1f%% of covered texels are claimed by overlapping islands; wear would be shared between surfaces"), Report.Overlap * 100.f));
		}
		else if (Report.Overlap > 0.005f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("%.1f%% of covered texels overlap"), Report.Overlap * 100.f));
		}
		if (Report.OutOfRange > 0.02f)
		{
			Problem(true, bFailed, FString::Printf(TEXT("%.1f%% of UV area lies outside 0..1 and would be clamped"), Report.OutOfRange * 100.f));
		}
		else if (Report.OutOfRange > 0.f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("%.1f%% of UV area lies outside 0..1"), Report.OutOfRange * 100.f));
		}
		if (Report.Degenerate > 0.01f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("%.1f%% of triangles have zero UV area and get no wear"), Report.Degenerate * 100.f));
		}
		if (Report.DensitySpread > 8.f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("texel density varies %.0fx across the mesh; wear detail will be uneven"), Report.DensitySpread));
		}
		if (Covered > 0 && Report.Coverage < 0.2f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("UVs use only %.0f%% of the texture; resolution is spent on empty space"), Report.Coverage * 100.f));
		}

		Report.Verdict = bFailed ? TEXT("failed") : bWarn ? TEXT("warning") : TEXT("ok");
		return Report;
	}

	static bool GenerateInstancedSnapshot(const FGrungifyMeshSnapshot& Snapshot, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult)
	{
		if (!Snapshot.LocalBounds.IsValid || !Params.Floor)
		{
			return false;
		}
		const FGrungifySlotInfo* Slot = Context.Slots.FindByPredicate([](const FGrungifySlotInfo& Info) { return !Info.bExcluded; });
		if (!Slot)
		{
			return false;
		}

		OutResult.bInstanced = true;
		OutResult.bNanite = Snapshot.bNanite;
		OutResult.DominantSurface = Slot->Surface;

		// One mask set per instance, from a handful of points around its bounds: the top, the four sides at mid
		// height and near the base. Generic edge and crease values stand in for real convexity.
		const FBox Local = Snapshot.LocalBounds;
		const FVector Center = Local.GetCenter();
		const FVector Extent = Local.GetExtent();
		struct FProbe { FVector Local; FVector Normal; };
		const FProbe Probes[] = {
			{ FVector(Center.X, Center.Y, Local.Max.Z), FVector::UpVector },
			{ FVector(Local.Max.X, Center.Y, Center.Z), FVector::ForwardVector },
			{ FVector(Local.Min.X, Center.Y, Center.Z), -FVector::ForwardVector },
			{ FVector(Center.X, Local.Max.Y, Center.Z), FVector::RightVector },
			{ FVector(Center.X, Local.Min.Y, Center.Z), -FVector::RightVector },
			{ FVector(Local.Max.X, Center.Y, Local.Min.Z + Extent.Z * 0.2), FVector::ForwardVector },
			{ FVector(Local.Min.X, Center.Y, Local.Min.Z + Extent.Z * 0.2), -FVector::ForwardVector } };

		const int32 Count = Snapshot.Instances.Num();
		OutResult.InstanceMasks.SetNumZeroed(Count);
		FMaskStats Stats;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if ((Index & 255) == 0 && Params.CancelRequested())
			{
				OutResult.bCancelled = true;
				return false;
			}
			const FTransform& Instance = Snapshot.Instances[Index];

			FLinearColor Sum(0.f, 0.f, 0.f, 0.f);
			FLinearColor Peak(0.f, 0.f, 0.f, 0.f);
			for (const FProbe& Probe : Probes)
			{
				FPointSample S;
				S.P = Instance.TransformPosition(Probe.Local);
				S.N = Instance.TransformVectorNoScale(Probe.Normal).GetSafeNormal();
				S.Convex = 0.35f;
				S.Concave = 0.1f;
				S.Slot = Slot;
				const FLinearColor M = EvaluatePoint(S, Params);
				Sum += M;
				Peak = FLinearColor(FMath::Max(Peak.R, M.R), FMath::Max(Peak.G, M.G), FMath::Max(Peak.B, M.B), FMath::Max(Peak.A, M.A));
			}
			const FLinearColor Mean = Sum / float(UE_ARRAY_COUNT(Probes));
			const FLinearColor Mask = Mean * 0.5f + Peak * 0.5f;
			OutResult.InstanceMasks[Index] = FVector4f(Mask.R, Mask.G, Mask.B, Mask.A);
			Stats.Add(ToColor(Mask));
		}
		Stats.WriteTo(OutResult);
		OutResult.Vertices = Count;
		OutResult.Notes += TEXT("Instanced: one mask set per instance (per-instance custom data). ");
		return true;
	}

	/** Static meshes, from a snapshot only (no component or render data access, so it is safe on a worker). */
	static bool GenerateStaticSnapshot(const FGrungifyMeshSnapshot& Snapshot, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult)
	{
		if (Snapshot.LODs.Num() == 0 || !Params.Floor)
		{
			return false;
		}
		TArray<const FStaticMeshLODResources*> LODs;
		for (const TUniquePtr<FStaticMeshLODResources>& LOD : Snapshot.LODs)
		{
			LODs.Add(LOD.Get());
		}

		OutResult.bNanite = Snapshot.bNanite;
		const FTransform& Transform = Snapshot.Transform;
		const float SharpRef = 1.f - FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Params.EdgeAngleDeg, 5.f, 170.f) * 0.5f));
		int32 SurfaceVotes[3] = { 0, 0, 0 };

		// Decide the output from LOD0 (for Nanite meshes this is the fallback mesh, which shares the UV layout).
		const FStaticMeshLODResources& LOD0 = *LODs[0];
		FTopology Topo0;
		if (!Topo0.Build(LOD0, Transform, SharpRef))
		{
			return false;
		}

		const bool bHasMaskUV = int32(LOD0.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()) > Params.MaskUVChannel;
		const double AreaM2 = FMath::Max(Topo0.Area / 10000.0, 1e-3);
		const double CellsPerM2 = FMath::Square(100.0 / FMath::Max<double>(Params.FeatureScale, 1.0));
		const bool bSparse = Topo0.NumVerts / AreaM2 < CellsPerM2;

		// Validate the mask UV channel whenever a texture is a candidate, so the report says why it was or wasn't used.
		// Extended layers only exist in the second mask texture, so they also make Auto prefer a texture.
		const bool bWantsTexture = Params.MaskOutput == EGrungifyMaskOutput::Texture
			|| (Params.MaskOutput == EGrungifyMaskOutput::Auto && (OutResult.bNanite || bSparse || Params.bExtended));
		if (bWantsTexture)
		{
			OutResult.MaskUV = ValidateMaskUVs(LOD0, Transform, Params.MaskUVChannel, &Params);
			if (Params.CancelRequested())
			{
				OutResult.bCancelled = true;
				return false;
			}
		}
		const bool bUVsUsable = bHasMaskUV && (!OutResult.MaskUV.bChecked || OutResult.MaskUV.Verdict != TEXT("failed"));

		bool bTexture = false;
		switch (Params.MaskOutput)
		{
		case EGrungifyMaskOutput::Texture:
			// Explicitly requested: honour it whenever the channel exists, but say what's wrong with it.
			bTexture = bHasMaskUV;
			if (!bHasMaskUV)
			{
				OutResult.Notes += FString::Printf(TEXT("No UV channel %d for the mask texture; used vertex colours. "), Params.MaskUVChannel);
			}
			else if (!bUVsUsable)
			{
				OutResult.Notes += FString::Printf(TEXT("UV channel %d failed validation but Texture output was requested; expect artefacts (see Mask UV). "), Params.MaskUVChannel);
			}
			break;
		case EGrungifyMaskOutput::Auto:
			bTexture = bUVsUsable && (OutResult.bNanite || bSparse || Params.bExtended);
			if (!bTexture && (OutResult.bNanite || bSparse || Params.bExtended))
			{
				OutResult.Notes += FString::Printf(TEXT("%s but UV channel %d is %s; used vertex colours. "),
					OutResult.bNanite ? TEXT("Nanite mesh") : bSparse ? TEXT("Sparse mesh") : TEXT("Extended layers need a mask texture"), Params.MaskUVChannel,
					bHasMaskUV ? TEXT("unsuitable (see Mask UV)") : TEXT("missing"));
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
			if (!BakeTexture(LOD0, Topo0, Context, Params, OutResult, SurfaceVotes))
			{
				return false;
			}
		}
		else
		{
			OutResult.Output = EGrungifyMaskOutput::VertexColor;
			BakeVertexColors(LODs, Transform, Context, Params, SharpRef, OutResult, SurfaceVotes);
			if (OutResult.bCancelled)
			{
				return false;
			}
			if (OutResult.bNanite)
			{
				OutResult.Notes += TEXT("Nanite mesh with vertex-colour masks: may not render. ");
			}
			if (Params.bExtended)
			{
				OutResult.Notes += TEXT("Extended layers (moisture, soot, scorch, moss) need texture output; not baked for this mesh. ");
			}
		}

		MeasureSlotCoverage(LOD0, Topo0, Params.MaskUVChannel, OutResult);

		if (Params.ScatterDensity > 0.f)
		{
			CollectScatterSamples(Topo0, LOD0.VertexBuffers.StaticMeshVertexBuffer, Context, Params, OutResult, Params.Seed ^ GetTypeHash(Snapshot.Path));
			if (OutResult.bCancelled)
			{
				return false;
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

	namespace
	{
		/**
		 * CPU copy of a skeletal mesh LOD as static LOD resources (reference pose, render-section materials), so the
		 * static mesh texture bake runs on it unchanged. False if the render data's CPU copy isn't available.
		 */
		bool CopySkinnedLOD(const FSkeletalMeshLODRenderData& Source, const USkinnedAsset& Asset, FStaticMeshLODResources& LOD)
		{
			const FPositionVertexBuffer& Positions = Source.StaticVertexBuffers.PositionVertexBuffer;
			const FStaticMeshVertexBuffer& Attributes = Source.StaticVertexBuffers.StaticMeshVertexBuffer;
			const uint32 NumVerts = Positions.GetNumVertices();
			if (NumVerts == 0 || !Positions.GetVertexData() || !Attributes.GetTangentData() || !Attributes.GetTexCoordData()
				|| Attributes.GetNumVertices() != NumVerts)
			{
				return false;
			}
			TArray<uint32> Indices;
			Source.MultiSizeIndexContainer.GetIndexBuffer(Indices);
			if (Indices.Num() < 3)
			{
				return false;
			}

			const uint32 NumUVs = Attributes.GetNumTexCoords();
			LOD.VertexBuffers.PositionVertexBuffer.Init(NumVerts);
			FStaticMeshVertexBuffer& Copy = LOD.VertexBuffers.StaticMeshVertexBuffer;
			Copy.SetUseFullPrecisionUVs(true);
			Copy.Init(NumVerts, NumUVs);
			for (uint32 V = 0; V < NumVerts; ++V)
			{
				LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V) = Positions.VertexPosition(V);
				Copy.SetVertexTangents(V, Attributes.VertexTangentX(V), Attributes.VertexTangentY(V), Attributes.VertexTangentZ(V));
				for (uint32 UV = 0; UV < NumUVs; ++UV)
				{
					Copy.SetVertexUV(V, UV, Attributes.GetVertexUV(V, UV));
				}
			}
			LOD.IndexBuffer.SetIndices(Indices, EIndexBufferStride::Force32Bit);

			// Render sections index the LOD's material map; LOD0 is normally identity.
			const FSkeletalMeshLODInfo* Info = Asset.GetLODInfo(0);
			for (int32 SectionIndex = 0; SectionIndex < Source.RenderSections.Num(); ++SectionIndex)
			{
				const FSkelMeshRenderSection& Section = Source.RenderSections[SectionIndex];
				if (Section.bDisabled || Section.NumTriangles == 0)
				{
					continue;
				}
				FStaticMeshSection& Out = LOD.Sections.AddDefaulted_GetRef();
				Out.MaterialIndex = Section.MaterialIndex;
				if (Info && Info->LODMaterialMap.IsValidIndex(SectionIndex) && Info->LODMaterialMap[SectionIndex] != INDEX_NONE)
				{
					Out.MaterialIndex = Info->LODMaterialMap[SectionIndex];
				}
				Out.FirstIndex = Section.BaseIndex;
				Out.NumTriangles = Section.NumTriangles;
				Out.MinVertexIndex = Section.BaseVertexIndex;
				Out.MaxVertexIndex = Section.BaseVertexIndex + FMath::Max<uint32>(Section.NumVertices, 1) - 1;
			}
			return LOD.Sections.Num() > 0;
		}
	}

	int32 NumUVChannelsOf(const UMeshComponent* Component)
	{
		if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			const UStaticMesh* Mesh = Static->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			return RenderData && RenderData->LODResources.Num() > 0 ? int32(RenderData->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()) : 0;
		}
		if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
		{
			const USkinnedAsset* Asset = Skinned->GetSkinnedAsset();
			const FSkeletalMeshRenderData* RenderData = Asset ? Asset->GetResourceForRendering() : nullptr;
			return RenderData && RenderData->LODRenderData.Num() > 0 ? int32(RenderData->LODRenderData[0].StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()) : 0;
		}
		return 0;
	}

	uint32 MeshBuildHash(const UMeshComponent* Component)
	{
		if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			const UStaticMesh* Mesh = Static->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			return RenderData ? GetTypeHash(RenderData->DerivedDataKey) : 0;
		}
		if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
		{
			const USkinnedAsset* Asset = Skinned->GetSkinnedAsset();
			const FSkeletalMeshRenderData* RenderData = Asset ? Asset->GetResourceForRendering() : nullptr;
			if (!RenderData || RenderData->LODRenderData.Num() == 0)
			{
				return 0;
			}
			// No public derived-data key for skeletal render data, so hash LOD0's content: positions, every UV channel and the
			// indices. Edits that keep the same vertex count (moved vertices, re-unwrapped UVs) change it too.
			const FSkeletalMeshLODRenderData& LOD0 = RenderData->LODRenderData[0];
			const FPositionVertexBuffer& Positions = LOD0.StaticVertexBuffers.PositionVertexBuffer;
			const FStaticMeshVertexBuffer& Attributes = LOD0.StaticVertexBuffers.StaticMeshVertexBuffer;
			uint32 Hash = HashCombine(GetTypeHash(LOD0.GetNumVertices()), GetTypeHash(LOD0.RenderSections.Num()));
			if (Positions.GetVertexData() && Attributes.GetTexCoordData() && Attributes.GetNumVertices() == Positions.GetNumVertices())
			{
				const uint32 NumUVs = Attributes.GetNumTexCoords();
				for (uint32 V = 0; V < Positions.GetNumVertices(); ++V)
				{
					const FVector3f P = Positions.VertexPosition(V);
					Hash = FCrc::MemCrc32(&P, sizeof(P), Hash);
					for (uint32 UV = 0; UV < NumUVs; ++UV)
					{
						const FVector2f T = Attributes.GetVertexUV(V, UV);
						Hash = FCrc::MemCrc32(&T, sizeof(T), Hash);
					}
				}
			}
			TArray<uint32> Indices;
			LOD0.MultiSizeIndexContainer.GetIndexBuffer(Indices);
			return Indices.Num() > 0 ? FCrc::MemCrc32(Indices.GetData(), Indices.Num() * sizeof(uint32), Hash) : Hash;
		}
		return 0;
	}

	bool GenerateLandscapeMasks(FGrungifyLandscapeResult& InOut, const FGrungifySurfaceProfile& Profile, float ContactStrength, const FGrungifyGenerationParams& Params)
	{
		const int32 Count = InOut.SizeX * InOut.SizeY;
		if (Count <= 0 || InOut.Positions.Num() != Count || InOut.Normals.Num() != Count || InOut.Contact.Num() != Count || InOut.Valid.Num() != Count)
		{
			return false;
		}

		// Terrain is a stone floor: no chips (no edges), no handling (nobody holds it).
		FGrungifySlotInfo Slot;
		Slot.Surface = EGrungifySurfaceType::Stone;
		Slot.Profile = Profile;

		InOut.Pixels.Init(FColor(0, 0, 0, 0), Count);
		if (Params.bExtended)
		{
			InOut.ExtendedPixels.Init(FColor(0, 0, 0, 0), Count);
		}
		FLinearColor Sum = FLinearColor::Transparent;
		InOut.Covered = 0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if ((Index & 1023) == 0 && Params.CancelRequested())
			{
				InOut.bCancelled = true;
				return false;
			}
			if (!InOut.Valid[Index])
			{
				continue;
			}
			FPointSample S;
			S.P = InOut.Positions[Index];
			S.N = FVector(InOut.Normals[Index]);
			S.Convex = 0.f;
			S.Concave = 0.15f;
			S.Slot = &Slot;
			FLinearColor Extended = FLinearColor::Transparent;
			FLinearColor Masks = EvaluatePoint(S, Params, Params.bExtended ? &Extended : nullptr);
			Masks.R = 0.f;
			Masks.A = 0.f;

			// Where terrain meets a wall or prop, dust and grit collect and the ground stays damp.
			const float Contact = InOut.Contact[Index] * ContactStrength;
			if (Contact > 0.f)
			{
				Masks.G = FMath::Clamp(Masks.G + Contact * 0.6f * FMath::Max(Params.DustIntensity, 0.f) * Params.Intensity, 0.f, 1.f);
				if (Params.bExtended)
				{
					Extended.R = FMath::Clamp(Extended.R + Contact * 0.35f * Params.Extended.MoistureIntensity, 0.f, 1.f);
					Extended.A = FMath::Clamp(Extended.A + Contact * 0.25f * Params.Extended.MossIntensity, 0.f, 1.f);
				}
			}
			InOut.Pixels[Index] = ToColor(Masks);
			if (Params.bExtended)
			{
				InOut.ExtendedPixels[Index] = ToColor(Extended);
			}
			Sum += Masks;
			++InOut.Covered;
		}
		InOut.MeanMasks = InOut.Covered > 0 ? Sum / float(InOut.Covered) : FLinearColor::Transparent;
		return true;
	}

	static bool GenerateSkinnedSnapshot(const FGrungifyMeshSnapshot& Snapshot, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& InParams, FGrungifyComponentResult& OutResult)
	{
		if (Snapshot.LODs.Num() == 0 || !InParams.Floor)
		{
			return false;
		}
		OutResult.bNanite = false;
		const FStaticMeshLODResources& LOD = *Snapshot.LODs[0];

		// The reference pose at the component's placement when snapshotted: wear follows where the mesh stood.
		const FTransform& Transform = Snapshot.Transform;
		const float SharpRef = 1.f - FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(InParams.EdgeAngleDeg, 5.f, 170.f) * 0.5f));
		FTopology Topo;
		if (!Topo.Build(LOD, Transform, SharpRef))
		{
			return false;
		}

		// Skeletal meshes have no per-instance vertex colours that survive saving, so masks always go into a texture.
		// Many skinned meshes only have UV0; use it when the configured mask channel is missing or unusable and UV0 is clean.
		FGrungifyGenerationParams Params = InParams;
		const int32 NumUVs = int32(LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords());
		FGrungifyUVReport Report;
		if (NumUVs > Params.MaskUVChannel)
		{
			Report = ValidateMaskUVs(LOD, Transform, Params.MaskUVChannel, &Params);
		}
		if ((NumUVs <= Params.MaskUVChannel || Report.Verdict == TEXT("failed")) && Params.MaskUVChannel != 0 && NumUVs > 0)
		{
			const FGrungifyUVReport UV0 = ValidateMaskUVs(LOD, Transform, 0, &Params);
			if (UV0.Verdict != TEXT("failed"))
			{
				OutResult.Notes += FString::Printf(TEXT("UV%d %s; used UV0 for the mask texture. "), Params.MaskUVChannel,
					NumUVs <= Params.MaskUVChannel ? TEXT("is missing") : TEXT("failed validation"));
				Params.MaskUVChannel = 0;
				Report = UV0;
			}
		}
		OutResult.MaskUV = Report;
		if (NumUVs <= Params.MaskUVChannel || Report.Verdict == TEXT("failed"))
		{
			OutResult.Notes += TEXT("No clean UV channel for a mask texture (skeletal meshes can't use vertex-colour masks); add a non-overlapping UV channel. ");
			return false;
		}
		OutResult.MaskUVChannelUsed = Params.MaskUVChannel;

		int32 SurfaceVotes[3] = { 0, 0, 0 };
		OutResult.Output = EGrungifyMaskOutput::Texture;
		if (!BakeTexture(LOD, Topo, Context, Params, OutResult, SurfaceVotes))
		{
			return false;
		}
		OutResult.Vertices = int32(LOD.GetNumVertices());
		MeasureSlotCoverage(LOD, Topo, Params.MaskUVChannel, OutResult);

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

	namespace
	{
		/** CPU copy of a static mesh LOD (positions, tangents, every UV channel, indices, sections). */
		bool CopyStaticLOD(const FStaticMeshLODResources& Source, FStaticMeshLODResources& LOD)
		{
			const FPositionVertexBuffer& Positions = Source.VertexBuffers.PositionVertexBuffer;
			const FStaticMeshVertexBuffer& Attributes = Source.VertexBuffers.StaticMeshVertexBuffer;
			const uint32 NumVerts = Positions.GetNumVertices();
			if (NumVerts == 0 || !Positions.GetVertexData() || !Attributes.GetTangentData() || !Attributes.GetTexCoordData()
				|| Attributes.GetNumVertices() != NumVerts)
			{
				return false;
			}
			const uint32 NumUVs = Attributes.GetNumTexCoords();
			LOD.VertexBuffers.PositionVertexBuffer.Init(NumVerts);
			FStaticMeshVertexBuffer& Copy = LOD.VertexBuffers.StaticMeshVertexBuffer;
			Copy.SetUseFullPrecisionUVs(true);
			Copy.Init(NumVerts, NumUVs);
			for (uint32 V = 0; V < NumVerts; ++V)
			{
				LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V) = Positions.VertexPosition(V);
				Copy.SetVertexTangents(V, Attributes.VertexTangentX(V), Attributes.VertexTangentY(V), Attributes.VertexTangentZ(V));
				for (uint32 UV = 0; UV < NumUVs; ++UV)
				{
					Copy.SetVertexUV(V, UV, Attributes.GetVertexUV(V, UV));
				}
			}
			TArray<uint32> Indices;
			Source.IndexBuffer.GetCopy(Indices);
			LOD.IndexBuffer.SetIndices(Indices, EIndexBufferStride::Force32Bit);
			LOD.Sections = Source.Sections;
			return Indices.Num() >= 3;
		}
	}

	bool SnapshotMesh(UMeshComponent* Component, FGrungifyMeshSnapshot& Out)
	{
		Out = FGrungifyMeshSnapshot();
		if (!IsValid(Component))
		{
			return false;
		}
		Out.Path = Component->GetPathName();
		Out.Transform = Component->GetComponentTransform();

		if (UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
		{
			const UStaticMesh* Mesh = Instanced->GetStaticMesh();
			if (!Mesh)
			{
				return false;
			}
			Out.Kind = FGrungifyMeshSnapshot::EKind::Instanced;
			Out.bNanite = Mesh->HasValidNaniteData();
			Out.LocalBounds = Mesh->GetBoundingBox();
			Out.Instances.Reserve(Instanced->GetInstanceCount());
			for (int32 Index = 0; Index < Instanced->GetInstanceCount(); ++Index)
			{
				FTransform Instance;
				Instanced->GetInstanceTransform(Index, Instance, /*bWorldSpace*/ true);
				Out.Instances.Add(Instance);
			}
			return true;
		}
		if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			const UStaticMesh* Mesh = Static->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			if (!RenderData || RenderData->LODResources.Num() == 0)
			{
				return false;
			}
			Out.Kind = FGrungifyMeshSnapshot::EKind::Static;
			Out.bNanite = Mesh->HasValidNaniteData();
			for (int32 LODIndex = 0; LODIndex < RenderData->LODResources.Num(); ++LODIndex)
			{
				TUniquePtr<FStaticMeshLODResources> Copy = MakeUnique<FStaticMeshLODResources>(false);
				if (!CopyStaticLOD(RenderData->LODResources[LODIndex], *Copy))
				{
					// A later LOD that isn't readable only costs its vertex colours; LOD0 is required.
					if (LODIndex == 0)
					{
						return false;
					}
					break;
				}
				Out.LODs.Add(MoveTemp(Copy));
			}
			return true;
		}
		if (USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component))
		{
			const USkinnedAsset* Asset = Skeletal->GetSkinnedAsset();
			const FSkeletalMeshRenderData* RenderData = Asset ? Asset->GetResourceForRendering() : nullptr;
			if (!RenderData || RenderData->LODRenderData.Num() == 0)
			{
				return false;
			}
			Out.Kind = FGrungifyMeshSnapshot::EKind::Skeletal;
			TUniquePtr<FStaticMeshLODResources> Copy = MakeUnique<FStaticMeshLODResources>(false);
			if (!CopySkinnedLOD(RenderData->LODRenderData[0], *Asset, *Copy))
			{
				return false;
			}
			Out.LODs.Add(MoveTemp(Copy));
			return true;
		}
		return false;
	}

	bool GenerateFromSnapshot(const FGrungifyMeshSnapshot& Snapshot, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult)
	{
		switch (Snapshot.Kind)
		{
		case FGrungifyMeshSnapshot::EKind::Static: return GenerateStaticSnapshot(Snapshot, Context, Params, OutResult);
		case FGrungifyMeshSnapshot::EKind::Skeletal: return GenerateSkinnedSnapshot(Snapshot, Context, Params, OutResult);
		case FGrungifyMeshSnapshot::EKind::Instanced: return GenerateInstancedSnapshot(Snapshot, Context, Params, OutResult);
		default: return false;
		}
	}

	// Game-thread conveniences for callers holding a component.
	bool GenerateComponent(UStaticMeshComponent* Component, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult)
	{
		FGrungifyMeshSnapshot Snapshot;
		OutResult.Component = Component;
		return SnapshotMesh(Component, Snapshot) && Snapshot.Kind == FGrungifyMeshSnapshot::EKind::Static && GenerateFromSnapshot(Snapshot, Context, Params, OutResult);
	}

	bool GenerateInstanced(UInstancedStaticMeshComponent* Component, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult)
	{
		FGrungifyMeshSnapshot Snapshot;
		OutResult.Component = Component;
		return SnapshotMesh(Component, Snapshot) && GenerateFromSnapshot(Snapshot, Context, Params, OutResult);
	}

	bool GenerateSkinned(USkeletalMeshComponent* Component, const FGrungifyComponentContext& Context, const FGrungifyGenerationParams& Params, FGrungifyComponentResult& OutResult)
	{
		FGrungifyMeshSnapshot Snapshot;
		OutResult.Component = Component;
		if (!SnapshotMesh(Component, Snapshot))
		{
			OutResult.Notes += TEXT("LOD0 vertex data isn't readable on the CPU. ");
			return false;
		}
		return GenerateFromSnapshot(Snapshot, Context, Params, OutResult);
	}

#if WITH_DEV_AUTOMATION_TESTS
	namespace
	{
		/** CPU-only LOD with the given positions, per-vertex UVs (same in channels 0 and 1) and triangles. */
		void InitTestLOD(FStaticMeshLODResources& LOD, const TArray<FVector3f>& Positions, const TArray<FVector2f>& UVs, const TArray<uint32>& Indices)
		{
			const uint32 NumVerts = uint32(Positions.Num());
			LOD.VertexBuffers.PositionVertexBuffer.Init(NumVerts);
			FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
			Attributes.SetUseFullPrecisionUVs(true);
			Attributes.Init(NumVerts, 2);
			for (uint32 V = 0; V < NumVerts; ++V)
			{
				LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V) = Positions[V];
				Attributes.SetVertexTangents(V, FVector3f(1.f, 0.f, 0.f), FVector3f(0.f, 1.f, 0.f), FVector3f(0.f, 0.f, 1.f));
				Attributes.SetVertexUV(V, 0, UVs[V]);
				Attributes.SetVertexUV(V, 1, UVs[V]);
			}
			LOD.IndexBuffer.SetIndices(Indices, EIndexBufferStride::Force32Bit);
		}
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifySlotCoverageTest, "Grungify.Generation.SlotCoverage",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FGrungifySlotCoverageTest::RunTest(const FString& Parameters)
	{
		// A 1 m x 1 m quad (two triangles, slot 0) with UVs covering the whole 0..1 square.
		FStaticMeshLODResources LOD(false);
		InitTestLOD(LOD, { { 0, 0, 0 }, { 100, 0, 0 }, { 100, 100, 0 }, { 0, 100, 0 } },
			{ { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }, { 0, 1, 2, 0, 2, 3 });
		FTopology Topo;
		if (!TestTrue(TEXT("Topology builds"), Topo.Build(LOD, FTransform::Identity, 0.1f)))
		{
			return false;
		}
		Topo.TriSlot.Init(0, 2);

		// Texture output: chips everywhere (1.0), dust nowhere, runoff below the 0.1 threshold, handling on the left half.
		FGrungifyComponentResult Texture;
		Texture.Output = EGrungifyMaskOutput::Texture;
		Texture.TextureSize = 8;
		Texture.TexturePixels.SetNum(64);
		for (int32 Y = 0; Y < 8; ++Y)
		{
			for (int32 X = 0; X < 8; ++X)
			{
				Texture.TexturePixels[Y * 8 + X] = FColor(255, 0, 20, X < 4 ? 255 : 0);
			}
		}
		MeasureSlotCoverage(LOD, Topo, 1, Texture);
		if (!TestEqual(TEXT("One slot measured"), Texture.SlotCoverage.Num(), 1))
		{
			return false;
		}
		const FGrungifyComponentResult::FSlotCoverage& T = Texture.SlotCoverage[0];
		TestTrue(TEXT("Slot area is 1 m2"), FMath::IsNearlyEqual(T.Area, 1.0, 1e-6));
		TestTrue(TEXT("Chips cover the whole slot"), FMath::IsNearlyEqual(T.Covered[0], 1.0, 1e-6));
		TestTrue(TEXT("Chip mean where covered is 1"), FMath::IsNearlyEqual(T.Sum[0] / T.Covered[0], 1.0, 1e-3));
		TestEqual(TEXT("No dust"), T.Covered[1], 0.0);
		TestEqual(TEXT("Runoff below threshold isn't counted"), T.Covered[2], 0.0);
		TestTrue(TEXT("Runoff peak still reported"), FMath::IsNearlyEqual(T.Max[2], 20.f / 255.f, 1e-4f));
		// Seven samples per triangle: the left-half estimate is coarse but must be roughly half.
		TestTrue(TEXT("Handling covers about half"), T.Covered[3] > 0.3 && T.Covered[3] < 0.7);

		// Vertex output: interpolated corner colours.
		FGrungifyComponentResult Vertex;
		Vertex.Output = EGrungifyMaskOutput::VertexColor;
		Vertex.LODColors.SetNum(1);
		Vertex.LODColors[0] = { FColor(0, 255, 0, 0), FColor(0, 255, 0, 0), FColor(0, 255, 0, 0), FColor(0, 255, 0, 0) };
		MeasureSlotCoverage(LOD, Topo, 1, Vertex);
		TestTrue(TEXT("Vertex dust covers the slot"), Vertex.SlotCoverage.Num() == 1 && FMath::IsNearlyEqual(Vertex.SlotCoverage[0].Covered[1], 1.0, 1e-6));
		TestEqual(TEXT("Vertex output has no chips"), Vertex.SlotCoverage.Num() == 1 ? Vertex.SlotCoverage[0].Covered[0] : -1.0, 0.0);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyUVValidationTest, "Grungify.Generation.MaskUVValidation",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FGrungifyUVValidationTest::RunTest(const FString& Parameters)
	{
		// A 100 x 100 cm quad split along its diagonal: the shared edge must not count as overlap.
		const TArray<FVector3f> QuadPos = { { 0.f, 0.f, 0.f }, { 100.f, 0.f, 0.f }, { 100.f, 100.f, 0.f }, { 0.f, 100.f, 0.f } };
		const TArray<FVector2f> QuadUV = { { 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f } };
		{
			FStaticMeshLODResources LOD(false);
			InitTestLOD(LOD, QuadPos, QuadUV, { 0, 1, 2, 0, 2, 3 });
			const FGrungifyUVReport Report = ValidateMaskUVs(LOD, FTransform::Identity, 1);
			TestTrue(TEXT("Clean quad: present"), Report.bPresent);
			TestEqual(TEXT("Clean quad: verdict"), Report.Verdict, FString(TEXT("ok")));
			TestEqual(TEXT("Clean quad: no overlap along the shared diagonal"), Report.Overlap, 0.f);
			TestTrue(TEXT("Clean quad: covers most of the square"), Report.Coverage > 0.95f);
			TestTrue(TEXT("Clean quad: uniform density"), FMath::IsNearlyEqual(Report.DensitySpread, 1.f, 0.01f));
		}
		{
			// Two separate quads stacked on the same UV island (typical of mirrored or tiled UV0).
			TArray<FVector3f> Pos = QuadPos;
			for (const FVector3f& P : QuadPos)
			{
				Pos.Add(P + FVector3f(0.f, 0.f, 200.f));
			}
			TArray<FVector2f> UV = QuadUV;
			UV.Append(QuadUV);
			FStaticMeshLODResources LOD(false);
			InitTestLOD(LOD, Pos, UV, { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 });
			const FGrungifyUVReport Report = ValidateMaskUVs(LOD, FTransform::Identity, 1);
			TestEqual(TEXT("Stacked islands: verdict"), Report.Verdict, FString(TEXT("failed")));
			TestTrue(TEXT("Stacked islands: overlap detected"), Report.Overlap > 0.9f);
		}
		{
			TArray<FVector2f> UV;
			for (const FVector2f& T : QuadUV)
			{
				UV.Add(T * 2.f);
			}
			FStaticMeshLODResources LOD(false);
			InitTestLOD(LOD, QuadPos, UV, { 0, 1, 2, 0, 2, 3 });
			const FGrungifyUVReport Report = ValidateMaskUVs(LOD, FTransform::Identity, 1);
			TestEqual(TEXT("Tiling UVs: verdict"), Report.Verdict, FString(TEXT("failed")));
			TestTrue(TEXT("Tiling UVs: out of range"), Report.OutOfRange > 0.5f);
		}
		{
			// A small boundary excursion affects only its thin outside strip, not both whole triangles.
			TArray<FVector2f> UV = QuadUV;
			for (FVector2f& T : UV) { T.X *= 1.002f; }
			FStaticMeshLODResources LOD(false);
			InitTestLOD(LOD, QuadPos, UV, { 0, 1, 2, 0, 2, 3 });
			const FGrungifyUVReport Report = ValidateMaskUVs(LOD, FTransform::Identity, 1);
			TestTrue(TEXT("Small overflow: actual outside-area fraction"), FMath::IsNearlyEqual(Report.OutOfRange, 0.002f / 1.002f, 1e-5f));
			TestEqual(TEXT("Small overflow: warns without failing usable UVs"), Report.Verdict, FString(TEXT("warning")));
		}
		{
			// A genuinely larger excursion must still fail the unchanged two-percent threshold.
			TArray<FVector2f> UV = QuadUV;
			for (FVector2f& T : UV) { T.X *= 1.05f; }
			FStaticMeshLODResources LOD(false);
			InitTestLOD(LOD, QuadPos, UV, { 0, 1, 2, 0, 2, 3 });
			const FGrungifyUVReport Report = ValidateMaskUVs(LOD, FTransform::Identity, 1);
			TestTrue(TEXT("Large overflow: actual outside-area fraction"), FMath::IsNearlyEqual(Report.OutOfRange, 0.05f / 1.05f, 1e-5f));
			TestEqual(TEXT("Large overflow: fails"), Report.Verdict, FString(TEXT("failed")));
		}
		{
			// Exercise all four clipping boundaries at once, including negative UV coordinates.
			TArray<FVector2f> UV = QuadUV;
			for (FVector2f& T : UV) { T = T * 1.004f - FVector2f(0.002f, 0.002f); }
			FStaticMeshLODResources LOD(false);
			InitTestLOD(LOD, QuadPos, UV, { 0, 1, 2, 0, 2, 3 });
			const FGrungifyUVReport Report = ValidateMaskUVs(LOD, FTransform::Identity, 1);
			TestTrue(TEXT("Four-sided overflow: actual outside-area fraction"), FMath::IsNearlyEqual(Report.OutOfRange, 1.f - 1.f / (1.004f * 1.004f), 1e-5f));
			TestEqual(TEXT("Four-sided small overflow: warns"), Report.Verdict, FString(TEXT("warning")));
		}
		{
			FStaticMeshLODResources LOD(false);
			InitTestLOD(LOD, QuadPos, QuadUV, { 0, 1, 2, 0, 2, 3 });
			const FGrungifyUVReport Report = ValidateMaskUVs(LOD, FTransform::Identity, 3);
			TestFalse(TEXT("Missing channel: not present"), Report.bPresent);
			TestEqual(TEXT("Missing channel: verdict"), Report.Verdict, FString(TEXT("failed")));
			TestEqual(TEXT("Missing channel: explained"), Report.Problems.Num(), 1);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyTextureCancelTest, "Grungify.Generation.TextureBakeCancellation",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FGrungifyTextureCancelTest::RunTest(const FString& Parameters)
	{
		// Same CPU-only quad as above, baked at 256 x 256 so there are several row batches.
		FStaticMeshLODResources LOD(false);
		InitTestLOD(LOD, { { 0.f, 0.f, 0.f }, { 100.f, 0.f, 0.f }, { 100.f, 100.f, 0.f }, { 0.f, 100.f, 0.f } },
			{ { 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f } }, { 0, 1, 2, 0, 2, 3 });
		FTopology Topo;
		if (!TestTrue(TEXT("Topology"), Topo.Build(LOD, FTransform::Identity, 0.13f)))
		{
			return false;
		}

		FGrungifyFloorField Floor;
		Floor.BuildFlat(-50.);
		FGrungifyGenerationParams Params;
		Params.Floor = &Floor;
		Params.Seed = 7;
		Params.ZoneTransform = FTransform::Identity;
		Params.ZoneExtent = FVector(500.);
		Params.MaskUVChannel = 1;
		Params.MaskTexelSize = 0.25f;
		Params.MaxMaskResolution = 256;
		Params.AmbientStreaks = 1.f;

		FGrungifyComponentContext Context;
		Context.Slots.SetNum(1);

		FGrungifyComponentResult Reference;
		int32 ReferenceVotes[3] = { 0, 0, 0 };
		TestTrue(TEXT("Reference bake"), BakeTexture(LOD, Topo, Context, Params, Reference, ReferenceVotes));
		TestEqual(TEXT("Reference resolution"), Reference.TextureSize, 256);

		// A callback that never cancels must not change a single texel.
		int32 Polls = 0;
		Params.ShouldCancel = [&Polls]() { ++Polls; return false; };
		FGrungifyComponentResult Polled;
		int32 PolledVotes[3] = { 0, 0, 0 };
		TestTrue(TEXT("Polled bake"), BakeTexture(LOD, Topo, Context, Params, Polled, PolledVotes));
		TestTrue(TEXT("Polling leaves the output identical"), Polled.TexturePixels == Reference.TexturePixels);
		TestFalse(TEXT("Polled bake not cancelled"), Polled.bCancelled);
		TestTrue(TEXT("Polled between row batches"), Polls >= 256 / 32);

		// Cancelling partway stops the bake and marks the result unusable.
		int32 Remaining = 3;
		Params.ShouldCancel = [&Remaining]() { return --Remaining < 0; };
		FGrungifyComponentResult Cancelled;
		int32 CancelledVotes[3] = { 0, 0, 0 };
		TestFalse(TEXT("Cancelled bake returns false"), BakeTexture(LOD, Topo, Context, Params, Cancelled, CancelledVotes));
		TestTrue(TEXT("Cancelled bake is marked"), Cancelled.bCancelled);
		TestEqual(TEXT("Cancelled bake has no texture size"), Cancelled.TextureSize, 0);
		return true;
	}
#endif // WITH_DEV_AUTOMATION_TESTS
}

#endif // WITH_EDITOR
