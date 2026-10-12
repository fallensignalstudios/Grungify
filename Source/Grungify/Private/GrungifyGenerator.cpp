// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyGenerator.h"

#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkinnedAsset.h"

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
#include "Misc/Crc.h"
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

	FString SlotNameOf(const UMeshComponent* Component, int32 MaterialIndex)
	{
		const TArray<FName> Names = Component ? Component->GetMaterialSlotNames() : TArray<FName>();
		return Names.IsValidIndex(MaterialIndex) ? Names[MaterialIndex].ToString() : FString();
	}

	EGrungifySurfaceType ClassifySlot(const UMeshComponent* Component, int32 MaterialIndex, EGrungifySurfaceType Override, FString* OutSource)
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
		if (const UObject* Mesh = MeshAssetOf(Component))
		{
			const FString SlotName = SlotNameOf(Component, MaterialIndex);
			if (!SlotName.IsEmpty())
			{
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

	bool IsRail(const UMeshComponent* Component, FString* OutSource)
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
		if (const UObject* Mesh = MeshAssetOf(Component))
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
			if (const UMeshComponent* MeshComponent = Cast<UMeshComponent>(Component))
			{
				const UObject* Mesh = MeshAssetOf(MeshComponent);
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

uint32 FGrungifyFloorField::Hash() const
{
	uint32 Hash = HashCombine(GetTypeHash(FlatZ), GetTypeHash(NumX * 131 + NumY));
	Hash = HashCombine(Hash, GetTypeHash(Bounds.Min));
	Hash = HashCombine(Hash, GetTypeHash(Bounds.Max));
	Hash = HashCombine(Hash, GetTypeHash(CellX));
	Hash = HashCombine(Hash, GetTypeHash(CellY));
	for (const TArray<double, TInlineAllocator<4>>& Cell : Heights)
	{
		Hash = HashCombine(Hash, GetTypeHash(Cell.Num()));
		for (const double Height : Cell)
		{
			Hash = HashCombine(Hash, GetTypeHash(Height));
		}
	}
	return Hash;
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
	bool RuleMatchesSlot(const FGrungifySlotRule& Rule, const UMeshComponent* Component, int32 MaterialIndex, FString* OutWhy)
	{
		const bool bHasActor = !Rule.Actor.IsNull();
		const bool bHasPattern = !Rule.NamePattern.IsEmpty();
		const bool bHasMaterial = !Rule.Material.IsNull();
		const bool bHasSlot = Rule.SlotIndex >= 0;
		const bool bHasComponent = !Rule.ComponentName.IsNone();
		if (!Rule.bEnabled || (!bHasActor && !bHasPattern && !bHasMaterial && !bHasSlot && !bHasComponent))
		{
			return false;
		}
		if ((bHasSlot && Rule.SlotIndex != MaterialIndex) || (bHasComponent && Component->GetFName() != Rule.ComponentName))
		{
			return false;
		}

		TArray<FString> Why;
		if (bHasComponent)
		{
			Why.Add(FString::Printf(TEXT("component %s"), *Component->GetName()));
		}
		if (bHasSlot)
		{
			Why.Add(FString::Printf(TEXT("slot %d"), MaterialIndex));
		}
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
			if (const UObject* Mesh = MeshAssetOf(Component))
			{
				const FString SlotName = SlotNameOf(Component, MaterialIndex);
				if (!SlotName.IsEmpty())
				{
					Names.Emplace(TEXT("slot name"), SlotName);
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
	Width = Height = BytesPerPixel = 0;
	Pixels.Reset();
	ContentHash = 0;
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
	ContentHash = HashCombine(HashCombine(GetTypeHash(Width), GetTypeHash(Height)), GetTypeHash(BytesPerPixel));
	for (int64 Offset = 0; Offset < Pixels.Num();)
	{
		const int32 Count = int32(FMath::Min<int64>(Pixels.Num() - Offset, MAX_int32));
		ContentHash = FCrc::MemCrc32(Pixels.GetData() + Offset, Count, ContentHash);
		Offset += Count;
	}
	return true;
}

namespace Grungify
{
	uint32 HashSlotContext(const FGrungifyComponentContext& Context)
	{
		uint32 Hash = GetTypeHash(Context.Slots.Num());
		auto Add = [&Hash](uint32 Value) { Hash = HashCombine(Hash, Value); };
		for (const FGrungifySlotInfo& Slot : Context.Slots)
		{
			Add(uint32(Slot.Surface));
			Add(uint32(Slot.bExcluded));
			Add(uint32(Slot.bRail));
			for (float Value : { Slot.Profile.Dust, Slot.Profile.Runoff, Slot.Profile.Handling, Slot.Profile.EdgeChips })
			{
				Add(GetTypeHash(Value));
			}
			Add(uint32(Slot.Gate != nullptr));
			if (const FGrungifyGate* Gate = Slot.Gate)
			{
				Add(uint32(Gate->bUseFacingGate));
				Add(GetTypeHash(Gate->MinUpFacing));
				Add(GetTypeHash(Gate->MaxUpFacing));
				Add(uint32(Gate->bUseHeightGate));
				Add(GetTypeHash(Gate->MinHeightCm));
				Add(GetTypeHash(Gate->MaxHeightCm));
				Add(GetTypeHash(Gate->GateUVChannel));
				Add(uint32(Gate->GateChannel));
				Add(uint32(Gate->bInvertGate));
				Add(GetTypeHash(Gate->GateTexture ? Gate->GateTexture->GetPathName() : FString()));
			}
			Add(uint32(Slot.GateSampler != nullptr));
			if (Slot.GateSampler) { Add(Slot.GateSampler->Hash()); }
		}
		return Hash;
	}
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

// ------------------------------------------------------------- Sky exposure

bool FGrungifyExposureField::Build(UWorld* World, const FBox& InBounds, double Spacing, int32 NumRays, double RayLength, const AActor* IgnoreActor, const TFunction<bool()>& ShouldCancel)
{
	NumX = NumY = NumZ = 0;
	Values.Reset();
	if (!World || !InBounds.IsValid)
	{
		return true;
	}

	const double Step = FMath::Max(Spacing, 20.0);
	Bounds = InBounds.ExpandBy(Step * 0.5);
	const FVector Size = Bounds.GetSize();
	auto Count = [Step](double Length) { return FMath::Clamp(FMath::CeilToInt(Length / Step) + 1, 2, 64); };
	NumX = Count(Size.X);
	NumY = Count(Size.Y);
	NumZ = Count(Size.Z);
	// Keep the ray count bounded on huge zones by coarsening the grid.
	while (NumX * NumY * NumZ > 32768)
	{
		NumX = FMath::Max(2, NumX * 3 / 4);
		NumY = FMath::Max(2, NumY * 3 / 4);
		NumZ = FMath::Max(2, NumZ * 3 / 4);
	}
	Cell = FVector(Size.X / (NumX - 1), Size.Y / (NumY - 1), Size.Z / (NumZ - 1));

	// Cosine-weighted directions over the upper hemisphere (Fibonacci spiral), so the result reads as sky visibility.
	const int32 Rays = FMath::Clamp(NumRays, 4, 128);
	TArray<FVector> Directions;
	Directions.Reserve(Rays);
	const double Golden = PI * (3.0 - FMath::Sqrt(5.0));
	for (int32 Index = 0; Index < Rays; ++Index)
	{
		const double U = (Index + 0.5) / Rays;
		const double Radius = FMath::Sqrt(U);
		const double Phi = Index * Golden;
		Directions.Add(FVector(Radius * FMath::Cos(Phi), Radius * FMath::Sin(Phi), FMath::Sqrt(1.0 - U)));
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GrungifySkyTrace), /*bTraceComplex*/ false);
	if (IgnoreActor)
	{
		QueryParams.AddIgnoredActor(IgnoreActor);
	}

	Values.SetNumUninitialized(NumX * NumY * NumZ);
	for (int32 Z = 0; Z < NumZ; ++Z)
	{
		if (ShouldCancel && ShouldCancel())
		{
			NumX = NumY = NumZ = 0;
			Values.Reset();
			return false;
		}
		for (int32 Y = 0; Y < NumY; ++Y)
		{
			for (int32 X = 0; X < NumX; ++X)
			{
				const FVector P = Bounds.Min + FVector(X * Cell.X, Y * Cell.Y, Z * Cell.Z);
				int32 Open = 0;
				for (const FVector& Direction : Directions)
				{
					Open += World->LineTraceTestByChannel(P, P + Direction * RayLength, ECC_Visibility, QueryParams) ? 0 : 1;
				}
				Values[(Z * NumY + Y) * NumX + X] = float(Open) / float(Rays);
			}
		}
	}
	return true;
}

float FGrungifyExposureField::At(const FVector& P) const
{
	if (NumX == 0)
	{
		return 1.f;
	}
	const FVector L = (P - Bounds.Min) / Cell;
	const double FX = FMath::Clamp(L.X, 0.0, double(NumX - 1));
	const double FY = FMath::Clamp(L.Y, 0.0, double(NumY - 1));
	const double FZ = FMath::Clamp(L.Z, 0.0, double(NumZ - 1));
	const int32 X0 = FMath::Min(int32(FX), NumX - 2);
	const int32 Y0 = FMath::Min(int32(FY), NumY - 2);
	const int32 Z0 = FMath::Min(int32(FZ), NumZ - 2);
	const float TX = float(FX - X0);
	const float TY = float(FY - Y0);
	const float TZ = float(FZ - Z0);
	auto V = [this](int32 X, int32 Y, int32 Z) { return Values[(Z * NumY + Y) * NumX + X]; };
	const float A = FMath::Lerp(FMath::Lerp(V(X0, Y0, Z0), V(X0 + 1, Y0, Z0), TX), FMath::Lerp(V(X0, Y0 + 1, Z0), V(X0 + 1, Y0 + 1, Z0), TX), TY);
	const float B = FMath::Lerp(FMath::Lerp(V(X0, Y0, Z0 + 1), V(X0 + 1, Y0, Z0 + 1), TX), FMath::Lerp(V(X0, Y0 + 1, Z0 + 1), V(X0 + 1, Y0 + 1, Z0 + 1), TX), TY);
	return FMath::Lerp(A, B, TZ);
}

uint32 FGrungifyExposureField::Hash() const
{
	uint32 Hash = FCrc::MemCrc32(Values.GetData(), Values.Num() * sizeof(float));
	return HashCombine(Hash, GetTypeHash(Bounds.Min) ^ GetTypeHash(Bounds.Max));
}

// ------------------------------------------------------------ Segment field

void FGrungifySegmentField::Reset(double InCellSize)
{
	Segments.Reset();
	Cells.Reset();
	CellSize = FMath::Max(InCellSize, 5.0);
}

void FGrungifySegmentField::Add(const FSegment& Segment)
{
	const int32 Index = Segments.Add(Segment);
	const double Length = FVector::Dist(Segment.A, Segment.B);
	const int32 Steps = FMath::Max(1, FMath::CeilToInt(Length / (CellSize * 0.5)));
	const int32 Reach = FMath::Max(1, FMath::CeilToInt(Segment.Radius / CellSize));
	TSet<FIntVector> Touched;
	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const FIntVector Center = CellOf(FMath::Lerp(Segment.A, Segment.B, double(Step) / Steps));
		for (int32 Z = -Reach; Z <= Reach; ++Z)
		{
			for (int32 Y = -Reach; Y <= Reach; ++Y)
			{
				for (int32 X = -Reach; X <= Reach; ++X)
				{
					const FIntVector Cell = Center + FIntVector(X, Y, Z);
					if (!Touched.Contains(Cell))
					{
						Touched.Add(Cell);
						Cells.FindOrAdd(Cell).Add(Index);
					}
				}
			}
		}
	}
}

uint32 FGrungifySegmentField::Hash() const
{
	uint32 Hash = GetTypeHash(Segments.Num());
	for (const FSegment& Seg : Segments)
	{
		Hash = HashCombine(Hash, GetTypeHash(Seg.A));
		Hash = HashCombine(Hash, GetTypeHash(Seg.B));
		Hash = HashCombine(Hash, GetTypeHash(Seg.Radius));
		Hash = HashCombine(Hash, GetTypeHash(Seg.Value));
		Hash = HashCombine(Hash, GetTypeHash(Seg.AlongA));
		Hash = HashCombine(Hash, GetTypeHash(Seg.AlongB));
	}
	return Hash;
}

namespace Grungify
{
	// ------------------------------------------------------------ Water paths

	void TraceWaterPath(UWorld* World, const FVector& Start, float StepCm, int32 MaxSteps, const AActor* IgnoreActor,
		TArray<FVector>& OutPath, TArray<FGrungifyPool>& OutPools)
	{
		OutPath.Reset();
		OutPath.Add(Start);
		if (!World)
		{
			return;
		}

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GrungifyWaterTrace), /*bTraceComplex*/ true);
		if (IgnoreActor)
		{
			QueryParams.AddIgnoredActor(IgnoreActor);
		}

		const double Step = FMath::Max(StepCm, 2.f);
		const FVector Gravity(0.0, 0.0, -1.0);
		FVector P = Start;
		for (int32 Index = 0; Index < MaxSteps; ++Index)
		{
			FHitResult Hit;
			const FVector Next = P + Gravity * Step;
			if (!World->LineTraceSingleByChannel(Hit, P, Next, ECC_Visibility, QueryParams))
			{
				// Falling, or running down a wall just beside it.
				P = Next;
				OutPath.Add(P);
				continue;
			}

			const FVector N = Hit.ImpactNormal;
			if (N.Z < 0.2)
			{
				// Overhang or steep face: cling to it and keep going down.
				P = Hit.ImpactPoint + N * 1.0 + Gravity * (Step * 0.5);
				OutPath.Add(P);
				continue;
			}

			// On a ledge or slope: run downhill along it; on flat ground, pool.
			FVector Downhill = Gravity - N * FVector::DotProduct(Gravity, N);
			if (Downhill.Size() < 0.15)
			{
				OutPath.Add(Hit.ImpactPoint);
				FGrungifyPool& Pool = OutPools.AddDefaulted_GetRef();
				Pool.Center = Hit.ImpactPoint;
				Pool.Radius = 30.f;
				return;
			}
			Downhill.Normalize();
			P = Hit.ImpactPoint + N * 1.0 + Downhill * Step;
			OutPath.Add(P);
		}
	}

	float StampWeight(const FGrungifyStampData& Stamp, const FVector& P, float FeatureScale)
	{
		// Distance in the stamp's own unit-sphere space, so scaling the actor stretches it into an ellipsoid.
		const FVector Local = Stamp.Transform.InverseTransformPosition(P) / FMath::Max(Stamp.Radius, 1.f);
		float Distance = float(Local.Size());
		if (Stamp.EdgeNoise > 0.f)
		{
			const FVector NP = P / FMath::Max(FeatureScale, 1.f);
			Distance += (Fbm(NP * 1.3, 4242u) - 0.5f) * Stamp.EdgeNoise;
		}
		if (Distance >= 1.f)
		{
			return 0.f;
		}
		const float Inner = 1.f - FMath::Clamp(Stamp.Falloff, 0.f, 1.f);
		return Distance <= Inner ? 1.f : 1.f - FMath::SmoothStep(Inner, 1.f, Distance);
	}
}

#endif // WITH_EDITOR
