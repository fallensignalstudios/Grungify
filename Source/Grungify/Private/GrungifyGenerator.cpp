#include "GrungifyGenerator.h"

#if WITH_EDITOR

#include "CollisionQueryParams.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GrungifySettings.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
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

	bool NameMatches(const FString& Name, const TArray<FString>& Keywords, FString* OutKeyword)
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
					if (OutKeyword)
					{
						*OutKeyword = Keyword;
					}
					return true;
				}
			}
		}
		return false;
	}

	static EGrungifySurfaceType SurfaceFromTags(const TArray<FName>& Tags, const UGrungifySettings* Settings, FName* OutTag = nullptr)
	{
		for (const FName& Tag : Tags)
		{
			EGrungifySurfaceType Found = EGrungifySurfaceType::Auto;
			if (Tag == Settings->TrimTag)
			{
				Found = EGrungifySurfaceType::Trim;
			}
			else if (Tag == Settings->MetalTag)
			{
				Found = EGrungifySurfaceType::Metal;
			}
			else if (Tag == Settings->StoneTag)
			{
				Found = EGrungifySurfaceType::Stone;
			}
			if (Found != EGrungifySurfaceType::Auto)
			{
				if (OutTag)
				{
					*OutTag = Tag;
				}
				return Found;
			}
		}
		return EGrungifySurfaceType::Auto;
	}

	static EGrungifySurfaceType SurfaceFromName(const FString& Name, const UGrungifySettings* Settings, FString* OutKeyword = nullptr)
	{
		if (NameMatches(Name, Settings->TrimKeywords, OutKeyword))
		{
			return EGrungifySurfaceType::Trim;
		}
		if (NameMatches(Name, Settings->MetalKeywords, OutKeyword))
		{
			return EGrungifySurfaceType::Metal;
		}
		if (NameMatches(Name, Settings->StoneKeywords, OutKeyword))
		{
			return EGrungifySurfaceType::Stone;
		}
		return EGrungifySurfaceType::Auto;
	}

	EGrungifySurfaceType ClassifySlot(const UStaticMeshComponent* Component, int32 MaterialIndex, EGrungifySurfaceType Override, FString* OutSource)
	{
		auto Explain = [OutSource](const FString& Why)
		{
			if (OutSource)
			{
				*OutSource = Why;
			}
		};

		if (Override != EGrungifySurfaceType::Auto)
		{
			Explain(TEXT("zone Surface Type override"));
			return Override;
		}

		const UGrungifySettings* Settings = UGrungifySettings::Get();
		const AActor* Owner = Component->GetOwner();
		FName Tag;
		FString Keyword;

		// 1. Explicit tags win.
		EGrungifySurfaceType Result = SurfaceFromTags(Component->ComponentTags, Settings, &Tag);
		if (Result != EGrungifySurfaceType::Auto)
		{
			Explain(FString::Printf(TEXT("component tag '%s'"), *Tag.ToString()));
			return Result;
		}
		if (Owner)
		{
			Result = SurfaceFromTags(Owner->Tags, Settings, &Tag);
			if (Result != EGrungifySurfaceType::Auto)
			{
				Explain(FString::Printf(TEXT("actor tag '%s'"), *Tag.ToString()));
				return Result;
			}
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
						Explain(FString::Printf(TEXT("physical material %s (surface type %d) in Physical Surface Map"), *PhysMat->GetName(), int32(PhysMat->SurfaceType.GetValue())));
						return *Mapped;
					}
				}
			}

			for (const UMaterialInterface* It = Material; It; )
			{
				Result = SurfaceFromName(It->GetName(), Settings, &Keyword);
				if (Result != EGrungifySurfaceType::Auto)
				{
					Explain(FString::Printf(TEXT("keyword '%s' in material %s%s"), *Keyword, *It->GetName(), It == Material ? TEXT("") : TEXT(" (parent)")));
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
				const FString SlotName = StaticMaterials[MaterialIndex].MaterialSlotName.ToString();
				Result = SurfaceFromName(SlotName, Settings, &Keyword);
				if (Result != EGrungifySurfaceType::Auto)
				{
					Explain(FString::Printf(TEXT("keyword '%s' in slot name %s"), *Keyword, *SlotName));
					return Result;
				}
			}

			Result = SurfaceFromName(Mesh->GetName(), Settings, &Keyword);
			if (Result != EGrungifySurfaceType::Auto)
			{
				Explain(FString::Printf(TEXT("keyword '%s' in mesh %s"), *Keyword, *Mesh->GetName()));
				return Result;
			}
		}

		if (Owner)
		{
			Result = SurfaceFromName(Owner->GetActorLabel(), Settings, &Keyword);
			if (Result != EGrungifySurfaceType::Auto)
			{
				Explain(FString::Printf(TEXT("keyword '%s' in actor label %s"), *Keyword, *Owner->GetActorLabel()));
				return Result;
			}
		}

		Explain(TEXT("no tag, physical material or keyword matched; Fallback Surface Type"));
		return Settings->FallbackSurfaceType == EGrungifySurfaceType::Auto ? EGrungifySurfaceType::Stone : Settings->FallbackSurfaceType;
	}

	bool IsRail(const UStaticMeshComponent* Component, FString* OutSource)
	{
		const UGrungifySettings* Settings = UGrungifySettings::Get();
		const AActor* Owner = Component->GetOwner();
		FString Keyword;
		auto Found = [OutSource](const FString& Why)
		{
			if (OutSource)
			{
				*OutSource = Why;
			}
			return true;
		};

		if (Component->ComponentHasTag(Settings->RailTag))
		{
			return Found(FString::Printf(TEXT("component tag '%s'"), *Settings->RailTag.ToString()));
		}
		if (Owner && Owner->ActorHasTag(Settings->RailTag))
		{
			return Found(FString::Printf(TEXT("actor tag '%s'"), *Settings->RailTag.ToString()));
		}
		if (const UStaticMesh* Mesh = Component->GetStaticMesh())
		{
			if (NameMatches(Mesh->GetName(), Settings->RailKeywords, &Keyword))
			{
				return Found(FString::Printf(TEXT("keyword '%s' in mesh %s"), *Keyword, *Mesh->GetName()));
			}
		}
		if (Owner && NameMatches(Owner->GetActorLabel(), Settings->RailKeywords, &Keyword))
		{
			return Found(FString::Printf(TEXT("keyword '%s' in actor label %s"), *Keyword, *Owner->GetActorLabel()));
		}
		return false;
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

bool FGrungifyFloorField::BuildTraced(UWorld* World, const FBox& InBounds, double Spacing, const AActor* IgnoreActor, const TFunction<bool()>& ShouldCancel)
{
	BuildFlat(InBounds.Min.Z);
	if (!World || !InBounds.IsValid)
	{
		return true;
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
		// One poll per row of cells keeps the dialog responsive without slowing the traces.
		if (ShouldCancel && ShouldCancel())
		{
			BuildFlat(InBounds.Min.Z);
			return false;
		}
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
	return true;
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

// -------------------------------------------------------------- Slot rules

namespace Grungify
{
	bool RuleMatchesSlot(const FGrungifySlotRule& Rule, const UStaticMeshComponent* Component, int32 MaterialIndex, FString* OutWhy)
	{
		const bool bHasActor = !Rule.Actor.IsNull();
		const bool bHasPattern = !Rule.NamePattern.IsEmpty();
		const bool bHasMaterial = !Rule.Material.IsNull();
		if (!Rule.bEnabled || (!bHasActor && !bHasPattern && !bHasMaterial))
		{
			return false;
		}

		TArray<FString> Why;
		const AActor* Owner = Component->GetOwner();
		if (bHasActor)
		{
			if (Rule.Actor.Get() != Owner)
			{
				return false;
			}
			Why.Add(FString::Printf(TEXT("actor %s"), Owner ? *Owner->GetActorLabel() : TEXT("?")));
		}

		const UMaterialInterface* SlotMaterial = Component->GetMaterial(MaterialIndex);

		if (bHasMaterial)
		{
			const UMaterialInterface* Wanted = Rule.Material.Get();
			const UMaterialInterface* Matched = nullptr;
			for (const UMaterialInterface* It = SlotMaterial; It && !Matched; )
			{
				if (Wanted ? It == Wanted : It->GetPathName() == Rule.Material.ToSoftObjectPath().ToString())
				{
					Matched = It;
				}
				const UMaterialInstance* Instance = Cast<UMaterialInstance>(It);
				It = Instance ? Instance->Parent.Get() : nullptr;
			}
			if (!Matched)
			{
				return false;
			}
			Why.Add(FString::Printf(TEXT("material %s%s"), *Matched->GetName(), Matched == SlotMaterial ? TEXT("") : TEXT(" (parent)")));
		}

		if (bHasPattern)
		{
			TArray<TPair<const TCHAR*, FString>> Names;
			if (const UStaticMesh* Mesh = Component->GetStaticMesh())
			{
				const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
				if (StaticMaterials.IsValidIndex(MaterialIndex))
				{
					Names.Emplace(TEXT("slot name"), StaticMaterials[MaterialIndex].MaterialSlotName.ToString());
				}
				Names.Emplace(TEXT("mesh"), Mesh->GetName());
			}
			for (const UMaterialInterface* It = SlotMaterial; It; )
			{
				Names.Emplace(It == SlotMaterial ? TEXT("material") : TEXT("parent material"), It->GetName());
				const UMaterialInstance* Instance = Cast<UMaterialInstance>(It);
				It = Instance ? Instance->Parent.Get() : nullptr;
			}
			Names.Emplace(TEXT("component"), Component->GetName());
			if (Owner)
			{
				Names.Emplace(TEXT("actor label"), Owner->GetActorLabel());
			}

			const TPair<const TCHAR*, FString>* Hit = Names.FindByPredicate([&Rule](const TPair<const TCHAR*, FString>& Name)
			{
				return Name.Value.MatchesWildcard(Rule.NamePattern, ESearchCase::IgnoreCase);
			});
			if (!Hit)
			{
				return false;
			}
			Why.Add(FString::Printf(TEXT("%s '%s' matches '%s'"), Hit->Key, *Hit->Value, *Rule.NamePattern));
		}

		if (OutWhy)
		{
			*OutWhy = FString::Join(Why, TEXT(", "));
		}
		return true;
	}
}

// ------------------------------------------------------------ Gate sampler

bool FGrungifyGateSampler::Init(UTexture2D* Texture, FString& OutError)
{
	if (!Texture)
	{
		OutError = TEXT("no gate texture");
		return false;
	}

	FTextureSource& Source = Texture->Source;
	const ETextureSourceFormat Format = Source.GetFormat();
	if (Format == TSF_BGRA8)
	{
		BytesPerPixel = 4;
	}
	else if (Format == TSF_G8)
	{
		BytesPerPixel = 1;
	}
	else
	{
		OutError = FString::Printf(TEXT("gate texture %s must have an 8-bit source (BGRA8 or G8)"), *Texture->GetName());
		return false;
	}

	if (!Source.GetMipData(Pixels, 0))
	{
		OutError = FString::Printf(TEXT("could not read source data of gate texture %s"), *Texture->GetName());
		return false;
	}

	Width = Source.GetSizeX();
	Height = Source.GetSizeY();
	if (Width <= 0 || Height <= 0 || Pixels.Num() < int64(Width) * Height * BytesPerPixel)
	{
		OutError = FString::Printf(TEXT("gate texture %s has unexpected source size"), *Texture->GetName());
		return false;
	}
	return true;
}

float FGrungifyGateSampler::Sample(const FVector2f& UV, EGrungifyChannel Channel) const
{
	if (Width <= 0)
	{
		return 1.f;
	}

	const float U = UV.X - FMath::FloorToFloat(UV.X);
	const float V = UV.Y - FMath::FloorToFloat(UV.Y);
	const int32 X = FMath::Clamp(int32(U * Width), 0, Width - 1);
	const int32 Y = FMath::Clamp(int32(V * Height), 0, Height - 1);
	const uint8* Pixel = Pixels.GetData() + (int64(Y) * Width + X) * BytesPerPixel;

	if (BytesPerPixel == 1)
	{
		return Pixel[0] / 255.f;
	}

	// BGRA byte order.
	switch (Channel)
	{
	case EGrungifyChannel::R: return Pixel[2] / 255.f;
	case EGrungifyChannel::G: return Pixel[1] / 255.f;
	case EGrungifyChannel::B: return Pixel[0] / 255.f;
	default:                  return Pixel[3] / 255.f;
	}
}

#endif // WITH_EDITOR
