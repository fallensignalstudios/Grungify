// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyShowcase.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Level.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Engine/World.h"
#include "FileHelpers.h" // FEditorFileUtils, UEditorLoadingAndSavingUtils
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "GrungifyLibraryBuilder.h"
#include "GrungifyPreset.h"
#include "GrungifySettings.h"
#include "GrungifyTrafficPath.h"
#include "GrungifyTypes.h"
#include "GrungifyWearStamp.h"
#include "GrungifyWeatheringZone.h"
#include "GrungifyZoneComponent.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "GrungifyShowcase"

namespace
{
	// Plugin content, so the map ships with the plugin. Nothing here may reference /Game.
	const TCHAR* ShowcaseFolder = TEXT("/Grungify/Showcase");
	const TCHAR* MaterialFolder = TEXT("/Grungify/Showcase/Materials");
	const TCHAR* PresetFolder = TEXT("/Grungify/Showcase/Presets");
	const TCHAR* ExampleMaterialPath = TEXT("/Grungify/Materials/M_GrungifyExample");
	const TCHAR* ZoneBlueprintPath = TEXT("/Grungify/BP_WeatheringZone");
	const TCHAR* CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* CylinderPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");

	/** Every actor the builder spawns carries this; a rebuild deletes exactly these. */
	const FName GeneratedTag(TEXT("Grungify.ShowcaseGenerated"));

	constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone | RF_Transactional;

	// Bays sit side by side along +X, BaySpacing apart. Each bay is 12 m wide, its back wall's front face at Y = 380 and
	// facing -Y, towards its camera and the corridor (Y < -440). The 4 m gap keeps one bay's zone off its neighbours.
	constexpr double BaySpacing = 1600.;
	constexpr int32 NumBays = 6;
	constexpr double CorridorMinY = -1050.;
	constexpr double CorridorMaxY = -440.;

	// The basic shapes are 100 cm across with a centred pivot, so a size in cm is a scale of Size / 100.
	constexpr double ShapeSizeCm = 100.;

	FString ObjectPathFor(const FString& PackagePath)
	{
		return FString::Printf(TEXT("%s.%s"), *PackagePath, *FPackageName::GetShortName(PackagePath));
	}

	/** The asset at PackagePath, loading it if needed; null if there is none. */
	UObject* FindAsset(const FString& PackagePath)
	{
		const FString ObjectPath = ObjectPathFor(PackagePath);
		if (UObject* Loaded = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Loaded;
		}
		if (!FPackageName::DoesPackageExist(PackagePath))
		{
			return nullptr;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** The T at PackagePath, created if missing. Null (with OutError) if something else already lives there. */
	template <typename T>
	T* FindOrCreateAsset(const FString& PackagePath, FString& OutError)
	{
		UObject* Existing = FindAsset(PackagePath);
		T* Asset = Cast<T>(Existing);
		if (Existing && !Asset)
		{
			OutError = FString::Printf(TEXT("%s exists but is not a %s; delete or rename it and build again"), *PackagePath, *T::StaticClass()->GetName());
			return nullptr;
		}
		if (!Asset)
		{
			Asset = NewObject<T>(CreatePackage(*PackagePath), *FPackageName::GetShortName(PackagePath), AssetFlags);
			FAssetRegistryModule::AssetCreated(Asset);
		}
		return Asset;
	}

	struct FMaterialLook
	{
		FString Name;
		FLinearColor BaseColor;
		float Roughness;
		float Metallic;
	};

	UMaterialInstanceConstant* MakeMaterial(const FMaterialLook& Look, UMaterialInterface* Parent, TArray<UPackage*>& Packages, FString& OutError)
	{
		UMaterialInstanceConstant* Instance = FindOrCreateAsset<UMaterialInstanceConstant>(FString::Printf(TEXT("%s/%s"), MaterialFolder, *Look.Name), OutError);
		if (!Instance)
		{
			return nullptr;
		}
		// Overwrite rather than merge, so a rebuild always gives the same look whatever was hand-edited since.
		Instance->Modify();
		Instance->ClearParameterValuesEditorOnly();
		Instance->SetParentEditorOnly(Parent);
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Base Color")), Look.BaseColor);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), Look.Roughness);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Metallic")), Look.Metallic);
		// The actor tags classify the surfaces (that's part of what the showcase demonstrates); -1 keeps that classification.
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Grungify::SurfaceOverrideParameterName()), -1.f);
		Instance->PostEditChange();
		Instance->MarkPackageDirty();
		Packages.AddUnique(Instance->GetPackage());
		return Instance;
	}

	/** GP_Showcase_<Look>: the library look, written into plugin content so the showcase doesn't depend on /Game. */
	UGrungifyPreset* MakePreset(const FString& LookName, TArray<UPackage*>& Packages, FString& OutError)
	{
		UGrungifyPreset* Preset = FindOrCreateAsset<UGrungifyPreset>(FString::Printf(TEXT("%s/GP_Showcase_%s"), PresetFolder, *LookName), OutError);
		if (!Preset)
		{
			return nullptr;
		}
		Preset->Modify();
		if (!FGrungifyLibraryBuilder::ConfigureLook(LookName, *Preset))
		{
			OutError = FString::Printf(TEXT("the material library has no look named %s"), *LookName);
			return nullptr;
		}
		Preset->MarkPackageDirty();
		Packages.AddUnique(Preset->GetPackage());
		return Preset;
	}

	/** Stone, metal and trim instances in a library look's colours. */
	struct FLookMaterials
	{
		UMaterialInstanceConstant* Stone = nullptr;
		UMaterialInstanceConstant* Metal = nullptr;
		UMaterialInstanceConstant* Trim = nullptr;
		UGrungifyPreset* Preset = nullptr;

		bool IsValid() const { return Stone && Metal && Trim && Preset; }
	};

	FLookMaterials MakeLook(const FString& LookName, UMaterialInterface* Parent, TArray<UPackage*>& Packages, FString& OutError)
	{
		FLookMaterials Result;
		FGrungifyLookColors Colors;
		if (!FGrungifyLibraryBuilder::GetLookColors(LookName, Colors))
		{
			OutError = FString::Printf(TEXT("the material library has no look named %s"), *LookName);
			return Result;
		}
		// Same roughness and metallic per family as the library's MI_Grungify_<Look>_* instances.
		Result.Stone = MakeMaterial({ FString::Printf(TEXT("MI_Showcase_%s_Stone"), *LookName), Colors.Stone, 0.75f, 0.f }, Parent, Packages, OutError);
		Result.Metal = Result.Stone ? MakeMaterial({ FString::Printf(TEXT("MI_Showcase_%s_Metal"), *LookName), Colors.Metal, 0.45f, Colors.MetalMetallic }, Parent, Packages, OutError) : nullptr;
		Result.Trim = Result.Metal ? MakeMaterial({ FString::Printf(TEXT("MI_Showcase_%s_Trim"), *LookName), Colors.Trim, 0.6f, 0.f }, Parent, Packages, OutError) : nullptr;
		Result.Preset = Result.Trim ? MakePreset(LookName, Packages, OutError) : nullptr;
		return Result;
	}

	/**
	 * The map is saved into the plugin's own Content folder. A plugin the launcher installed into the engine is often
	 * read-only (Program Files), and a source-controlled one may have read-only (checked-in) files; fail up front, before
	 * the open level is switched, rather than after building everything.
	 */
	bool CheckPluginContentWritable(FString& OutError)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Grungify"));
		if (!Plugin || !Plugin->CanContainContent())
		{
			OutError = TEXT("the Grungify plugin or its content folder was not found");
			return false;
		}
		const bool bEnginePlugin = Plugin->GetLoadedFrom() == EPluginLoadedFrom::Engine;
		const FString Hint = bEnginePlugin
			? TEXT(" The plugin is installed in the engine; copy it into your project's Plugins folder (or make the engine's copy writable) and build again.")
			: TEXT(" Check it out of source control or make it writable, then build again.");

		IFileManager& FileManager = IFileManager::Get();
		const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetContentDir(), TEXT("Showcase")));
		if (!FileManager.DirectoryExists(*Directory) && !FileManager.MakeDirectory(*Directory, /*Tree*/ true))
		{
			OutError = FString::Printf(TEXT("cannot create %s; the plugin's content folder is read-only.%s"), *Directory, *Hint);
			return false;
		}

		// Directory attributes don't reliably say whether files can be created (Windows ignores read-only on folders), so try.
		const FString Probe = FPaths::Combine(Directory, TEXT(".GrungifyWriteProbe"));
		if (!FFileHelper::SaveStringToFile(TEXT("probe"), *Probe))
		{
			OutError = FString::Printf(TEXT("cannot write to %s; the plugin's content folder is read-only.%s"), *Directory, *Hint);
			return false;
		}
		FileManager.Delete(*Probe, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);

		// Previously built packages are overwritten in place, so each must be writable too.
		TArray<FString> Files;
		FileManager.FindFilesRecursive(Files, *Directory, TEXT("*.*"), /*Files*/ true, /*Directories*/ false);
		for (const FString& File : Files)
		{
			if (FileManager.IsReadOnly(*File))
			{
				OutError = FString::Printf(TEXT("%s is read-only.%s"), *File, *Hint);
				return false;
			}
		}
		return true;
	}

	FActorSpawnParameters SpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	/**
	 * Text render geometry is laid out along the component's local +Y with +Z up, so it reads correctly from its local -X
	 * side. Facing a viewer that looks along +Y (every bay camera) therefore takes a yaw of +90.
	 */
	const FRotator TextFacingCameras(0.f, 90.f, 0.f);

	struct FShowcaseAssets
	{
		UStaticMesh* Cube = nullptr;
		UStaticMesh* Cylinder = nullptr;
		UMaterialInstanceConstant* Stone = nullptr;
		UMaterialInstanceConstant* Metal = nullptr;
		UMaterialInstanceConstant* Trim = nullptr;
		UMaterialInstanceConstant* Floor = nullptr;
		UMaterialInstanceConstant* Sign = nullptr;
		UMaterialInstanceConstant* Crate = nullptr;
		UMaterialInstanceConstant* Ember = nullptr;
		FLookMaterials Industrial;
		FLookMaterials Temple;
		FLookMaterials Sewer;
	};

	/** Spawns the showcase actors bay by bay. Every helper tags, labels and files the actor under the current bay's folder. */
	class FShowcaseBuilder
	{
	public:
		FShowcaseBuilder(UWorld* InWorld, const FShowcaseAssets& InAssets)
			: World(InWorld)
			, Assets(InAssets)
			, Settings(UGrungifySettings::Get())
		{
		}

		/** Zones in build order, with their bay names, applied once every bay is in place. */
		TArray<TPair<FString, AGrungifyWeatheringZone*>> Zones;

		FString Error;

		void SetupLighting();
		void SetupCorridor();
		bool BuildBeforeAfter(int32 Index);
		bool BuildIndustrial(int32 Index);
		bool BuildTemple(int32 Index);
		bool BuildSewer(int32 Index);
		bool BuildFire(int32 Index);
		bool BuildInstancedAndStamps(int32 Index);

	private:
		UWorld* World;
		const FShowcaseAssets& Assets;
		const UGrungifySettings* Settings;

		FVector Origin = FVector::ZeroVector;
		FName Folder;
		bool bFailed = false;

		FVector At(double X, double Y, double Z) const { return Origin + FVector(X, Y, Z); }

		void SetFolder(const FString& Path) { Folder = FName(*(TEXT("Showcase/") + Path)); }

		template <typename T>
		T* Finish(T* Actor, const FString& Label)
		{
			if (!Actor)
			{
				if (!bFailed)
				{
					Error = FString::Printf(TEXT("could not spawn %s"), *Label);
				}
				bFailed = true;
				return nullptr;
			}
			Actor->SetActorLabel(Label);
			Actor->Tags.AddUnique(GeneratedTag);
			if (!Folder.IsNone())
			{
				Actor->SetFolderPath(Folder);
			}
			return Actor;
		}

		AStaticMeshActor* Mesh(const FString& Label, UStaticMesh* StaticMesh, UMaterialInterface* Material, const FTransform& Transform, const TArray<FName>& Tags)
		{
			AStaticMeshActor* Actor = Finish(World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, SpawnParams()), Label);
			if (Actor)
			{
				UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
				// Static, as the zone expects; World Settings force no precomputed lighting, so no lighting build is needed.
				Component->SetMobility(EComponentMobility::Static);
				Component->SetStaticMesh(StaticMesh);
				Component->SetMaterial(0, Material);
				Actor->Tags.Append(Tags);
			}
			return Actor;
		}

		/** A cube SizeCm across, centred on Center. */
		AStaticMeshActor* Box(const FString& Label, UMaterialInterface* Material, const FVector& Center, const FVector& SizeCm,
			const TArray<FName>& Tags, const FRotator& Rotation = FRotator::ZeroRotator)
		{
			return Mesh(Label, Assets.Cube, Material, FTransform(Rotation, Center, SizeCm / ShapeSizeCm), Tags);
		}

		/** A cylinder along its local Z: pass FRotator(90, 0, 0) to lay it along X, FRotator(0, 0, 90) along Y. */
		AStaticMeshActor* Cylinder(const FString& Label, UMaterialInterface* Material, const FVector& Center, double DiameterCm, double LengthCm,
			const TArray<FName>& Tags, const FRotator& Rotation = FRotator::ZeroRotator)
		{
			const FVector Scale(DiameterCm / ShapeSizeCm, DiameterCm / ShapeSizeCm, LengthCm / ShapeSizeCm);
			return Mesh(Label, Assets.Cylinder, Material, FTransform(Rotation, Center, Scale), Tags);
		}

		ATextRenderActor* Text(const FString& Label, const FString& String, const FVector& Location, float WorldSize, const FColor& Color)
		{
			ATextRenderActor* Actor = Finish(World->SpawnActor<ATextRenderActor>(Location, TextFacingCameras, SpawnParams()), Label);
			if (Actor)
			{
				UTextRenderComponent* Component = Actor->GetTextRender();
				Component->SetText(FText::FromString(String));
				Component->SetWorldSize(WorldSize);
				Component->SetHorizontalAlignment(EHTA_Center);
				Component->SetVerticalAlignment(EVRTA_TextCenter);
				Component->SetTextRenderColor(Color);
			}
			return Actor;
		}

		APointLight* PointLight(const FString& Label, const FVector& Location, float Candelas, float Radius, const FLinearColor& Color)
		{
			APointLight* Light = Finish(World->SpawnActor<APointLight>(Location, FRotator::ZeroRotator, SpawnParams()), Label);
			if (Light)
			{
				if (UPointLightComponent* Component = Cast<UPointLightComponent>(Light->GetLightComponent()))
				{
					// Movable renders identically without a lighting build.
					Component->SetMobility(EComponentMobility::Movable);
					Component->SetIntensityUnits(ELightUnits::Candelas);
					Component->SetIntensity(Candelas);
					Component->SetAttenuationRadius(Radius);
					Component->SetLightColor(Color);
				}
			}
			return Light;
		}

		/** Starts a bay: its folder, sign (title + caption) above the back wall, and the camera framing it. */
		void BeginBay(int32 Index, const FString& Name, const FString& Title, const FString& Caption)
		{
			Origin = FVector(Index * BaySpacing, 0., 0.);
			SetFolder(FString::Printf(TEXT("%d_%s"), Index + 1, *Name));

			// The board is ignored by the zones, so the titles stay clean and readable whatever the weathering does.
			Box(TEXT("Sign_") + Name, Assets.Sign, At(0., 400., 440.), FVector(1150., 10., 130.), { Settings->IgnoreTag });
			Text(TEXT("Title_") + Name, Title, At(0., 392., 470.), 52.f, FColor(240, 236, 226));
			Text(TEXT("Caption_") + Name, Caption, At(0., 392., 412.), 21.f, FColor(200, 198, 190));

			const FVector CameraLocation = At(0., -950., 260.);
			const FRotator CameraRotation = (At(0., 150., 190.) - CameraLocation).Rotation();
			if (ACameraActor* Camera = Finish(World->SpawnActor<ACameraActor>(CameraLocation, CameraRotation, SpawnParams()), TEXT("Cam_") + Name))
			{
				Camera->GetCameraComponent()->SetFieldOfView(70.f);
			}
		}

		/** The standard bay shell: a 12 x 8.6 m floor and a 3.2 m back wall. */
		void FloorAndBackWall(const FString& Name, UMaterialInterface* FloorMaterial, UMaterialInterface* WallMaterial)
		{
			Box(Name + TEXT("_Floor"), FloorMaterial, At(0., 0., -10.), FVector(1200., 860., 20.), { Settings->FloorTag, Settings->StoneTag });
			Box(Name + TEXT("_BackWall"), WallMaterial, At(0., 400., 160.), FVector(1200., 40., 320.), { Settings->StoneTag });
		}

		/** A zone sized to the bay (or to Extent), with a fixed seed, live preview off so nothing regenerates behind our back. */
		AGrungifyWeatheringZone* Zone(const FString& Name, int32 Seed, const FVector& Center, const FVector& Extent, UGrungifyPreset* Preset = nullptr)
		{
			UClass* ZoneClass = AGrungifyWeatheringZone::StaticClass();
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(FindAsset(ZoneBlueprintPath)))
			{
				if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(ZoneClass))
				{
					ZoneClass = Blueprint->GeneratedClass;
				}
			}
			AGrungifyWeatheringZone* NewZone = World->SpawnActor<AGrungifyWeatheringZone>(ZoneClass, Center, FRotator::ZeroRotator, SpawnParams());
			if (NewZone)
			{
				// Labels and preset edits can invoke PostEditChange; retire any spawn preview first.
				NewZone->bLivePreview = false;
				NewZone->CancelPreview();
			}
			NewZone = Finish(NewZone, TEXT("WeatheringZone_") + Name);
			if (!NewZone || !NewZone->GetZoneComponent())
			{
				Error = FString::Printf(TEXT("could not spawn the %s weathering zone"), *Name);
				bFailed = true;
				return nullptr;
			}
			if (Preset)
			{
				// Assigning Preset only loads it through the Details panel, so load it explicitly.
				NewZone->Preset = Preset;
				NewZone->LoadPreset();
			}
			// After the preset, which could carry a seed of its own (the showcase presets don't).
			NewZone->Seed = Seed;
			NewZone->MaskOutput = EGrungifyMaskOutput::Auto;
			NewZone->GetZoneComponent()->SetBoxExtent(Extent);
			Zones.Add({ Name, NewZone });
			return NewZone;
		}

		AGrungifyWeatheringZone* BayZone(const FString& Name, int32 Seed, UGrungifyPreset* Preset = nullptr)
		{
			// 12.4 x 8.5 m: the whole bay, short of the corridor (Y < -440) and 3.6 m clear of the next bay's zone.
			return Zone(Name, Seed, At(0., 0., 150.), FVector(620., 425., 260.), Preset);
		}

		/** One of the two identical Before / After rooms, centred on X: floor, three walls, skirting, a hand rail and a vent. */
		void Room(const FString& Prefix, double X)
		{
			Box(Prefix + TEXT("_Floor"), Assets.Floor, At(X, 0., -10.), FVector(450., 400., 20.), { Settings->FloorTag, Settings->StoneTag });
			Box(Prefix + TEXT("_BackWall"), Assets.Stone, At(X, 185., 125.), FVector(450., 30., 250.), { Settings->StoneTag });
			Box(Prefix + TEXT("_SideWall_L"), Assets.Stone, At(X - 210., 0., 125.), FVector(30., 340., 250.), { Settings->StoneTag });
			Box(Prefix + TEXT("_SideWall_R"), Assets.Stone, At(X + 210., 0., 125.), FVector(30., 340., 250.), { Settings->StoneTag });
			Box(Prefix + TEXT("_Skirting"), Assets.Trim, At(X, 165., 7.5), FVector(390., 10., 15.), { Settings->TrimTag });
			// A 6 cm pipe rail at hand height, 20 cm off the wall on two brackets.
			Cylinder(Prefix + TEXT("_Rail"), Assets.Metal, At(X, 150., 100.), 6., 360., { Settings->MetalTag, Settings->RailTag }, FRotator(90.f, 0.f, 0.f));
			Box(Prefix + TEXT("_RailBracket_L"), Assets.Metal, At(X - 150., 160., 100.), FVector(4., 20., 4.), { Settings->MetalTag });
			Box(Prefix + TEXT("_RailBracket_R"), Assets.Metal, At(X + 150., 160., 100.), FVector(4., 20., 4.), { Settings->MetalTag });
			Box(Prefix + TEXT("_Vent"), Assets.Metal, At(X + 120., 165., 200.), FVector(50., 10., 30.), { Settings->VentTag, Settings->MetalTag });
		}
	};

	void FShowcaseBuilder::SetupLighting()
	{
		SetFolder(TEXT("Lighting"));
		const FVector Center((NumBays - 1) * BaySpacing * 0.5, 0., 0.);

		// The sun travels towards +X+Y and down, so it lights every back wall (they face -Y) at a raking angle that
		// shows the relief. Movable lights render identically without a lighting build.
		if (ADirectionalLight* Sun = Finish(World->SpawnActor<ADirectionalLight>(Center + FVector(0., 0., 1000.), FRotator(-40.f, 60.f, 0.f), SpawnParams()), TEXT("Sun")))
		{
			if (UDirectionalLightComponent* Component = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				Component->SetMobility(EComponentMobility::Movable);
				Component->SetIntensity(8.f);
				Component->SetLightColor(FLinearColor(1.f, 0.96f, 0.9f));
				Component->SetAtmosphereSunLight(true);
			}
		}
		if (ASkyLight* Sky = Finish(World->SpawnActor<ASkyLight>(Center + FVector(0., 0., 900.), FRotator::ZeroRotator, SpawnParams()), TEXT("SkyLight")))
		{
			if (USkyLightComponent* Component = Sky->GetLightComponent())
			{
				Component->SetMobility(EComponentMobility::Movable);
				Component->bRealTimeCapture = true;
				Component->SetIntensity(1.f);
				Component->MarkRenderStateDirty();
			}
		}
		Finish(World->SpawnActor<ASkyAtmosphere>(Center, FRotator::ZeroRotator, SpawnParams()), TEXT("SkyAtmosphere"));

		// Fixed exposure, so every bay reads the same and walking between them doesn't pump the brightness.
		if (APostProcessVolume* Volume = Finish(World->SpawnActor<APostProcessVolume>(Center, FRotator::ZeroRotator, SpawnParams()), TEXT("PostProcess_FixedExposure")))
		{
			Volume->bUnbound = true;
			FPostProcessSettings& PostProcess = Volume->Settings;
			PostProcess.bOverride_AutoExposureMethod = true;
			PostProcess.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
			PostProcess.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
			PostProcess.AutoExposureApplyPhysicalCameraExposure = false;
			PostProcess.bOverride_AutoExposureBias = true;
			PostProcess.AutoExposureBias = 0.f;
			PostProcess.bOverride_MotionBlurAmount = true;
			PostProcess.MotionBlurAmount = 0.f;
			PostProcess.bOverride_VignetteIntensity = true;
			PostProcess.VignetteIntensity = 0.f;
			PostProcess.bOverride_BloomIntensity = true;
			PostProcess.BloomIntensity = 0.3f;
		}
	}

	void FShowcaseBuilder::SetupCorridor()
	{
		SetFolder(TEXT("Corridor"));
		Origin = FVector::ZeroVector;
		const double MinX = -BaySpacing * 0.5;
		const double MaxX = (NumBays - 0.5) * BaySpacing;
		// Ignored by the zones (and outside their boxes anyway), so it stays a clean walkway that frames the bays.
		Box(TEXT("Corridor_Floor"), Assets.Floor, FVector((MinX + MaxX) * 0.5, (CorridorMinY + CorridorMaxY) * 0.5, -10.),
			FVector(MaxX - MinX, CorridorMaxY - CorridorMinY, 20.), { Settings->IgnoreTag });

		if (APlayerStart* Start = Finish(World->SpawnActor<APlayerStart>(FVector(MinX + 150., -800., 100.), FRotator::ZeroRotator, SpawnParams()), TEXT("PlayerStart")))
		{
			Start->Tags.AddUnique(Settings->IgnoreTag);
		}

		const FVector OverviewTarget((MinX + MaxX) * 0.5, 0., 0.);
		const FVector OverviewLocation = OverviewTarget + FVector(0., -6500., 2600.);
		if (ACameraActor* Camera = Finish(World->SpawnActor<ACameraActor>(OverviewLocation, (OverviewTarget - OverviewLocation).Rotation(), SpawnParams()), TEXT("Cam_Overview")))
		{
			Camera->GetCameraComponent()->SetFieldOfView(70.f);
		}
	}

	// ------------------------------------------------------------- 1 Before / After
	bool FShowcaseBuilder::BuildBeforeAfter(int32 Index)
	{
		const FString Name = TEXT("BeforeAfter");
		BeginBay(Index, Name, TEXT("Before / After"),
			TEXT("Two identical rooms: stone, a hand rail, skirting and a vent. Only the right one has a zone."));

		Room(TEXT("Before"), -300.);
		Room(TEXT("After"), 300.);
		Text(TEXT("Label_Before"), TEXT("BEFORE - no zone"), At(-300., 185., 285.), 30.f, FColor(255, 210, 80));
		Text(TEXT("Label_After"), TEXT("AFTER - Grungify zone, seed 1101"), At(300., 185., 285.), 30.f, FColor(255, 210, 80));

		// Only the right room: X 50..550 stays clear of the left room (X up to -75).
		if (AGrungifyWeatheringZone* BayZoneActor = Zone(Name, 1101, At(300., 0., 130.), FVector(250., 230., 190.)))
		{
			BayZoneActor->Intensity = 1.2f;
			BayZoneActor->AmbientStreaks = 0.3f;
		}
		return !bFailed;
	}

	// ----------------------------------------------------- 2 Abandoned Industrial
	bool FShowcaseBuilder::BuildIndustrial(int32 Index)
	{
		const FString Name = TEXT("AbandonedIndustrial");
		const FLookMaterials& Look = Assets.Industrial;
		BeginBay(Index, Name, TEXT("Abandoned Industrial"),
			TEXT("Rust blooms round the bolts, runoff below the vents, scuffs along the traffic path, heavy dust."));

		FloorAndBackWall(Name, Look.Stone, Look.Stone);
		Box(Name + TEXT("_SideWall"), Look.Stone, At(-580., 0., 160.), FVector(40., 760., 320.), { Settings->StoneTag });

		// Three bolted metal panels on the back wall; their front faces are at Y = 372.
		const double PanelX[] = { -400., 0., 400. };
		// Bolt X offsets from the panel centre and heights, inside the panel's edges (it spans Z 20..280).
		const double BoltOffsets[][2] = { { -170., 40. }, { 0., 40. }, { 170., 40. }, { -170., 150. }, { 170., 150. }, { -170., 260. }, { 0., 260. }, { 170., 260. } };
		for (int32 Panel = 0; Panel < UE_ARRAY_COUNT(PanelX); ++Panel)
		{
			Box(FString::Printf(TEXT("%s_Panel_%d"), *Name, Panel + 1), Look.Metal, At(PanelX[Panel], 375., 150.), FVector(380., 6., 260.), { Settings->MetalTag });
			for (int32 Bolt = 0; Bolt < UE_ARRAY_COUNT(BoltOffsets); ++Bolt)
			{
				// Small enough (well under 25 cm) to count as fasteners; axis along Y, standing 3 cm proud of the panel.
				Cylinder(FString::Printf(TEXT("%s_Bolt_%d_%02d"), *Name, Panel + 1, Bolt + 1), Look.Metal,
					At(PanelX[Panel] + BoltOffsets[Bolt][0], 370.5, BoltOffsets[Bolt][1]), 4., 3.,
					{ Settings->FastenerTag, Settings->MetalTag }, FRotator(0.f, 0.f, 90.f));
			}
		}
		Box(Name + TEXT("_Vent_1"), Look.Metal, At(-400., 368., 230.), FVector(60., 8., 30.), { Settings->VentTag, Settings->MetalTag });
		Box(Name + TEXT("_Vent_2"), Look.Metal, At(250., 368., 230.), FVector(60., 8., 30.), { Settings->VentTag, Settings->MetalTag });

		// A machine block for dust on top and kick chips at the base, and a guard rail on two posts.
		Box(Name + TEXT("_Machine"), Look.Metal, At(350., 60., 60.), FVector(160., 120., 120.), { Settings->MetalTag });
		Cylinder(Name + TEXT("_GuardRail"), Look.Metal, At(-150., -150., 100.), 6., 500., { Settings->MetalTag, Settings->RailTag }, FRotator(90.f, 0.f, 0.f));
		Cylinder(Name + TEXT("_GuardPost_1"), Look.Trim, At(-380., -150., 50.), 6., 100., { Settings->TrimTag });
		Cylinder(Name + TEXT("_GuardPost_2"), Look.Trim, At(80., -150., 50.), 6., 100., { Settings->TrimTag });

		// The route workers took: across the bay in front of the guard rail (at Y = -150, X -400..100), widening towards the machine.
		if (AGrungifyTrafficPath* Path = Finish(World->SpawnActor<AGrungifyTrafficPath>(At(0., 0., 2.), FRotator::ZeroRotator, SpawnParams()), Name + TEXT("_TrafficPath")))
		{
			Path->WidthCm = 120.f;
			Path->Traffic = 1.2f;
			if (USplineComponent* Spline = Path->GetSpline())
			{
				Spline->ClearSplinePoints(/*bUpdateSpline*/ false);
				for (const FVector& Point : { At(-560., -320., 2.), At(-200., -300., 2.), At(150., -270., 2.), At(560., -160., 2.) })
				{
					Spline->AddSplinePoint(Point, ESplineCoordinateSpace::World, /*bUpdateSpline*/ false);
				}
				Spline->UpdateSpline();
			}
		}

		if (AGrungifyWeatheringZone* BayZoneActor = BayZone(Name, 2202, Look.Preset))
		{
			// Scene-specific: the look has no traffic, this bay has a path.
			BayZoneActor->Extended.ScuffIntensity = 1.f;
		}
		return !bFailed;
	}

	// --------------------------------------------------------------- 3 Old Temple
	bool FShowcaseBuilder::BuildTemple(int32 Index)
	{
		const FString Name = TEXT("OldTemple");
		const FLookMaterials& Look = Assets.Temple;
		BeginBay(Index, Name, TEXT("Old Temple"),
			TEXT("Sky exposure: the roofed left half stays dusty and damp; the open right half weathers harder."));

		FloorAndBackWall(Name, Look.Stone, Look.Stone);
		Box(Name + TEXT("_Ledge"), Look.Trim, At(0., 365., 200.), FVector(1200., 30., 15.), { Settings->TrimTag });
		Box(Name + TEXT("_Step_1"), Look.Stone, At(0., 300., 10.), FVector(520., 160., 20.), { Settings->StoneTag });
		Box(Name + TEXT("_Step_2"), Look.Stone, At(0., 320., 30.), FVector(440., 120., 20.), { Settings->StoneTag });
		Box(Name + TEXT("_Step_3"), Look.Stone, At(0., 340., 50.), FVector(360., 80., 20.), { Settings->StoneTag });

		// Roof over the left half only, low enough (Z 300..320) that the cameras still see the sign over it.
		Box(Name + TEXT("_Roof"), Look.Stone, At(-300., 190., 310.), FVector(600., 380., 20.), { Settings->StoneTag });
		Cylinder(Name + TEXT("_Column_Roofed_1"), Look.Stone, At(-520., 20., 150.), 50., 300., { Settings->StoneTag });
		Cylinder(Name + TEXT("_Column_Roofed_2"), Look.Stone, At(-80., 20., 150.), 50., 300., { Settings->StoneTag });
		Cylinder(Name + TEXT("_Column_Open_1"), Look.Stone, At(150., 20., 150.), 50., 300., { Settings->StoneTag });
		// A broken column, shorter than the rest.
		Cylinder(Name + TEXT("_Column_Open_2"), Look.Stone, At(450., 20., 110.), 50., 220., { Settings->StoneTag });

		// The same plinth under the roof and under the open sky, to compare side by side.
		Box(Name + TEXT("_Plinth_Sheltered"), Look.Stone, At(-300., 150., 50.), FVector(120., 120., 100.), { Settings->StoneTag });
		Box(Name + TEXT("_Plinth_Exposed"), Look.Stone, At(300., 150., 50.), FVector(120., 120., 100.), { Settings->StoneTag });
		Text(Name + TEXT("_Label_Sheltered"), TEXT("sheltered"), At(-300., 150., 130.), 24.f, FColor(255, 210, 80));
		Text(Name + TEXT("_Label_Exposed"), TEXT("open sky"), At(300., 150., 130.), 24.f, FColor(255, 210, 80));

		if (AGrungifyWeatheringZone* BayZoneActor = BayZone(Name, 3303, Look.Preset))
		{
			// Scene-specific: "north" is towards the cameras (-Y), so the moss grows on the faces the visitor sees.
			BayZoneActor->Extended.NorthYawDegrees = -90.f;
		}
		return !bFailed;
	}

	// ---------------------------------------------------------------- 4 Wet Sewer
	bool FShowcaseBuilder::BuildSewer(int32 Index)
	{
		const FString Name = TEXT("WetSewer");
		const FLookMaterials& Look = Assets.Sewer;
		BeginBay(Index, Name, TEXT("Wet Sewer"),
			TEXT("Traced water flow: from the outlet and the leak down the wall, along the ledge, into pools."));

		// Two walkways either side of a channel 60 cm deep.
		Box(Name + TEXT("_Walkway_Back"), Look.Stone, At(0., 240., -30.), FVector(1200., 280., 60.), { Settings->FloorTag, Settings->StoneTag });
		Box(Name + TEXT("_Walkway_Front"), Look.Stone, At(0., -265., -30.), FVector(1200., 330., 60.), { Settings->FloorTag, Settings->StoneTag });
		Box(Name + TEXT("_ChannelBed"), Look.Stone, At(0., 0., -70.), FVector(1200., 200., 20.), { Settings->FloorTag, Settings->StoneTag });
		Box(Name + TEXT("_BackWall"), Look.Stone, At(0., 400., 120.), FVector(1200., 40., 400.), { Settings->StoneTag });
		Box(Name + TEXT("_SideWall"), Look.Stone, At(580., 0., 120.), FVector(40., 760., 400.), { Settings->StoneTag });

		// Water source 1: an outlet pipe high on the wall (a vent, so it seeds runoff and traced flow).
		Cylinder(Name + TEXT("_OutletPipe"), Look.Metal, At(-300., 355., 240.), 30., 50., { Settings->VentTag, Settings->MetalTag }, FRotator(0.f, 0.f, 90.f));
		Box(Name + TEXT("_OutletFlange"), Look.Trim, At(-300., 377., 240.), FVector(50., 6., 50.), { Settings->TrimTag });
		// A ledge falling 6 degrees towards -X, under the leak: the water runs along it and drops off its low end.
		Box(Name + TEXT("_SlopedLedge"), Look.Trim, At(275., 365., 180.), FVector(350., 30., 12.), { Settings->TrimTag }, FRotator(6.f, 0.f, 0.f));

		AGrungifyWeatheringZone* BayZoneActor = Zone(Name, 4404, At(0., 0., 120.), FVector(620., 425., 290.), Look.Preset);
		if (BayZoneActor)
		{
			// Water source 2: a leak on the wall face above the ledge's high end.
			FGrungifyLeak Leak;
			Leak.Location = BayZoneActor->GetActorTransform().InverseTransformPosition(At(400., 376., 290.));
			Leak.RadiusCm = 10.f;
			Leak.Strength = 1.2f;
			BayZoneActor->Leaks.Add(Leak);
			// The look already traces water; this bay is the one that shows it, so make sure.
			BayZoneActor->bTraceWaterFlow = true;
		}
		return !bFailed;
	}

	// -------------------------------------------------------------- 5 Fire & Soot
	bool FShowcaseBuilder::BuildFire(int32 Index)
	{
		const FString Name = TEXT("FireAndSoot");
		BeginBay(Index, Name, TEXT("Fire & Soot"),
			TEXT("A brazier tagged Grungify.Fire: soot plume and scorch. The wall lamp adds soot from lights only."));

		FloorAndBackWall(Name, Assets.Floor, Assets.Stone);
		Box(Name + TEXT("_SideWall"), Assets.Stone, At(-580., 0., 160.), FVector(40., 760., 320.), { Settings->StoneTag });

		// Brazier: a bowl on a stand, 80 cm off the wall so the plume climbs it.
		Cylinder(Name + TEXT("_BrazierStand"), Assets.Metal, At(-250., 300., 35.), 16., 70., { Settings->MetalTag });
		Cylinder(TEXT("Brazier"), Assets.Metal, At(-250., 300., 82.5), 80., 25., { Settings->FireTag, Settings->MetalTag });
		// Glowing coals: ignored, so the zone doesn't weather the "fire" itself.
		Box(Name + TEXT("_Embers"), Assets.Ember, At(-250., 300., 96.), FVector(60., 60., 4.), { Settings->IgnoreTag });
		PointLight(Name + TEXT("_FireLight"), At(-250., 300., 150.), 30.f, 700.f, FLinearColor(1.f, 0.55f, 0.2f));

		// Wall lamp: a bracket and shade with a warm point light under it.
		Box(Name + TEXT("_LampBracket"), Assets.Metal, At(300., 370., 230.), FVector(20., 20., 30.), { Settings->MetalTag });
		Box(Name + TEXT("_LampShade"), Assets.Metal, At(300., 355., 248.), FVector(30., 20., 8.), { Settings->MetalTag });
		PointLight(Name + TEXT("_WallLamp"), At(300., 352., 228.), 8.f, 500.f, FLinearColor(1.f, 0.8f, 0.6f));

		if (AGrungifyWeatheringZone* BayZoneActor = BayZone(Name, 5505))
		{
			BayZoneActor->DustIntensity = 0.6f;
			BayZoneActor->Extended.SootIntensity = 1.3f;
			BayZoneActor->Extended.bSootFromLights = true;
			BayZoneActor->Extended.SootRadiusCm = 120.f;
			BayZoneActor->Extended.SootPlumeHeightCm = 300.f;
			BayZoneActor->Extended.ScorchIntensity = 1.2f;
			BayZoneActor->Extended.ScorchRadiusCm = 90.f;
		}
		return !bFailed;
	}

	// ------------------------------------------------------ 6 Instanced & Stamps
	bool FShowcaseBuilder::BuildInstancedAndStamps(int32 Index)
	{
		const FString Name = TEXT("InstancedAndStamps");
		BeginBay(Index, Name, TEXT("Instanced & Stamps"),
			TEXT("Crates in one instanced mesh, each weathered differently. Wear stamps add dust and clean a patch."));

		FloorAndBackWall(Name, Assets.Floor, Assets.Stone);

		// One instanced static mesh of 80 cm crates: a 3 x 2 row with two more stacked on top.
		AActor* Holder = Finish(World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams()), Name + TEXT("_Crates_ISM"));
		if (Holder)
		{
			UInstancedStaticMeshComponent* Crates = NewObject<UInstancedStaticMeshComponent>(Holder, TEXT("Crates"), RF_Transactional);
			// An instance component (as if added in the Details panel), so it is saved with the actor and shows in the editor.
			Crates->CreationMethod = EComponentCreationMethod::Instance;
			Crates->SetMobility(EComponentMobility::Static);
			Crates->SetStaticMesh(Assets.Cube);
			Crates->SetMaterial(0, Assets.Crate);
			Crates->ComponentTags.Add(Settings->TrimTag);
			Holder->SetRootComponent(Crates);
			Holder->AddInstanceComponent(Crates);
			Crates->RegisterComponent();
			Holder->SetActorLocation(At(-300., 200., 0.));

			const FVector CrateScale(0.8);
			int32 Count = 0;
			auto AddCrate = [&](double X, double Y, double Z)
			{
				// Small fixed yaw offsets so the stack doesn't look machine-placed; no randomness, so rebuilds match.
				const float Yaw = static_cast<float>((Count * 7) % 11) - 5.f;
				Crates->AddInstance(FTransform(FRotator(0.f, Yaw, 0.f), FVector(X, Y, Z), CrateScale));
				++Count;
			};
			for (const double Y : { -45., 45. })
			{
				for (const double X : { -90., 0., 90. })
				{
					AddCrate(X, Y, 40.);
				}
			}
			AddCrate(-45., 0., 120.);
			AddCrate(45., 0., 120.);
		}
		Text(Name + TEXT("_Label_Crates"), TEXT("instanced crates: per-instance wear"), At(-300., 200., 215.), 22.f, FColor(255, 210, 80));

		// Stamp 1 adds dust (and a few chips) over the floor and the base of the wall.
		if (AGrungifyWearStamp* Stamp = Finish(World->SpawnActor<AGrungifyWearStamp>(At(150., 280., 30.), FRotator::ZeroRotator, SpawnParams()), Name + TEXT("_Stamp_AddDust")))
		{
			Stamp->RadiusCm = 120.f;
			Stamp->Falloff = 0.6f;
			Stamp->WearLayers.Dust = 1.f;
			Stamp->WearLayers.EdgeChips = 0.4f;
			// Syncs the sphere to the radius.
			Stamp->PostEditChange();
		}
		Text(Name + TEXT("_Label_AddDust"), TEXT("wear stamp: + dust"), At(150., 280., 175.), 22.f, FColor(255, 210, 80));

		// Stamp 2 erases every layer from a patch of the wall, as if someone scrubbed it.
		if (AGrungifyWearStamp* Stamp = Finish(World->SpawnActor<AGrungifyWearStamp>(At(400., 380., 160.), FRotator::ZeroRotator, SpawnParams()), Name + TEXT("_Stamp_Clean")))
		{
			Stamp->RadiusCm = 90.f;
			Stamp->Falloff = 0.4f;
			Stamp->WearLayers.EdgeChips = -1.f;
			Stamp->WearLayers.Dust = -1.f;
			Stamp->WearLayers.Runoff = -1.f;
			Stamp->WearLayers.Handling = -1.f;
			Stamp->WearLayers.Moisture = -1.f;
			Stamp->WearLayers.Soot = -1.f;
			Stamp->WearLayers.Scorch = -1.f;
			Stamp->WearLayers.Moss = -1.f;
			Stamp->PostEditChange();
		}
		Text(Name + TEXT("_Label_Clean"), TEXT("wear stamp: clean patch"), At(400., 360., 285.), 22.f, FColor(255, 210, 80));

		if (AGrungifyWeatheringZone* BayZoneActor = BayZone(Name, 6606))
		{
			// Heavy all-round grime, so both the instances and the cleaned patch stand out.
			BayZoneActor->Intensity = 1.2f;
			BayZoneActor->DustIntensity = 1.4f;
			BayZoneActor->DustHeightCm = 120.f;
			BayZoneActor->LedgeDust = 1.f;
			BayZoneActor->AmbientStreaks = 0.6f;
			BayZoneActor->ChipIntensity = 1.2f;
			BayZoneActor->bWeatherInstancedMeshes = true;
			BayZoneActor->bApplyWearStamps = true;
		}
		return !bFailed;
	}
}

const TCHAR* FGrungifyShowcase::MapPackageName()
{
	return TEXT("/Grungify/Showcase/L_GrungifyShowcase");
}

bool FGrungifyShowcase::Build(FString& OutMessage)
{
	auto Fail = [&OutMessage](const FString& Message)
	{
		OutMessage = TEXT("Grungify showcase: ") + Message;
		UE_LOG(LogGrungify, Error, TEXT("%s"), *OutMessage);
		return false;
	};

	if (!GEditor)
	{
		return Fail(TEXT("needs the editor"));
	}
	if (GEditor->PlayWorld)
	{
		return Fail(TEXT("stop Play In Editor first"));
	}

	constexpr float BayFrames = 2.f * NumBays; // Spawning, then applying each bay.
	FScopedSlowTask Progress(4.f + BayFrames, LOCTEXT("Building", "Grungify: building the showcase map..."));
	Progress.MakeDialog();

	// ------------------------------------------------------------- Checks
	Progress.EnterProgressFrame(1.f, LOCTEXT("Checks", "Checking the plugin's content folder..."));
	{
		FString Error;
		if (!CheckPluginContentWritable(Error))
		{
			return Fail(Error);
		}
	}

	// ------------------------------------------------------------- Assets
	Progress.EnterProgressFrame(1.f, LOCTEXT("Assets", "Creating showcase materials and presets..."));
	UMaterialInterface* Parent = Cast<UMaterialInterface>(FindAsset(ExampleMaterialPath));
	if (!Parent)
	{
		return Fail(TEXT("M_GrungifyExample is missing; run Tools > Grungify > Update Plugin Content first"));
	}
	{
		FLinearColor Color;
		float Scalar = 0.f;
		const bool bHasParameters = Parent->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Base Color")), Color)
			&& Parent->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Roughness")), Scalar)
			&& Parent->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Metallic")), Scalar)
			&& Parent->GetScalarParameterValue(FMaterialParameterInfo(Grungify::SurfaceOverrideParameterName()), Scalar);
		if (!bHasParameters)
		{
			return Fail(TEXT("M_GrungifyExample lacks Base Color / Roughness / Metallic / Grungify Surface Override; rebuild plugin content"));
		}
	}

	FShowcaseAssets Assets;
	Assets.Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	Assets.Cylinder = LoadObject<UStaticMesh>(nullptr, CylinderPath);
	if (!Assets.Cube || !Assets.Cylinder)
	{
		return Fail(TEXT("engine basic shapes (/Engine/BasicShapes) are missing"));
	}

	TArray<UPackage*> Packages;
	FString Error;
	Assets.Stone = MakeMaterial({ TEXT("MI_Showcase_Stone"), FLinearColor(0.42f, 0.38f, 0.33f), 0.85f, 0.f }, Parent, Packages, Error);
	Assets.Metal = Assets.Stone ? MakeMaterial({ TEXT("MI_Showcase_Metal"), FLinearColor(0.10f, 0.12f, 0.14f), 0.45f, 0.2f }, Parent, Packages, Error) : nullptr;
	Assets.Trim = Assets.Metal ? MakeMaterial({ TEXT("MI_Showcase_Trim"), FLinearColor(0.78f, 0.72f, 0.58f), 0.6f, 0.f }, Parent, Packages, Error) : nullptr;
	Assets.Floor = Assets.Trim ? MakeMaterial({ TEXT("MI_Showcase_Floor"), FLinearColor(0.35f, 0.35f, 0.33f), 0.9f, 0.f }, Parent, Packages, Error) : nullptr;
	Assets.Sign = Assets.Floor ? MakeMaterial({ TEXT("MI_Showcase_Sign"), FLinearColor(0.02f, 0.02f, 0.025f), 0.9f, 0.f }, Parent, Packages, Error) : nullptr;
	Assets.Crate = Assets.Sign ? MakeMaterial({ TEXT("MI_Showcase_Crate"), FLinearColor(0.36f, 0.24f, 0.13f), 0.8f, 0.f }, Parent, Packages, Error) : nullptr;
	// Not emissive (the parent has no emissive input to drive), but bright enough under the fire light to read as coals.
	Assets.Ember = Assets.Crate ? MakeMaterial({ TEXT("MI_Showcase_Ember"), FLinearColor(1.f, 0.32f, 0.04f), 1.f, 0.f }, Parent, Packages, Error) : nullptr;
	if (!Assets.Ember)
	{
		return Fail(Error);
	}
	Assets.Industrial = MakeLook(TEXT("AbandonedIndustrial"), Parent, Packages, Error);
	Assets.Temple = Assets.Industrial.IsValid() ? MakeLook(TEXT("OldTemple"), Parent, Packages, Error) : FLookMaterials();
	Assets.Sewer = Assets.Temple.IsValid() ? MakeLook(TEXT("WetSewer"), Parent, Packages, Error) : FLookMaterials();
	if (!Assets.Sewer.IsValid())
	{
		return Fail(Error);
	}
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false))
	{
		return Fail(FString::Printf(TEXT("could not save the showcase materials or presets in %s"), ShowcaseFolder));
	}

	// -------------------------------------------------------------- Level
	Progress.EnterProgressFrame(1.f, LOCTEXT("Level", "Opening the showcase level..."));
	// The editor switches to the showcase map, so the user's open level is offered for saving first (cancel aborts).
	if (!FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave*/ true, /*bSaveMapPackages*/ true, /*bSaveContentPackages*/ false))
	{
		return Fail(TEXT("cancelled; the open level was left as it was"));
	}

	// Rebuilding reuses the existing package rather than saving a new world over it, which a loaded package would block.
	const FString MapPackage = MapPackageName();
	const bool bExistingMap = FPackageName::DoesPackageExist(MapPackage);
	UWorld* World = bExistingMap
		? UEditorLoadingAndSavingUtils::LoadMap(FPackageName::LongPackageNameToFilename(MapPackage, FPackageName::GetMapPackageExtension()))
		: UEditorLoadingAndSavingUtils::NewBlankMap(/*bSaveExistingMap*/ false);
	if (!World || !World->PersistentLevel)
	{
		return Fail(bExistingMap ? TEXT("could not open the existing showcase map") : TEXT("could not create a new map"));
	}
	int32 Removed = 0;
	if (bExistingMap)
	{
		// Only what an earlier build generated; anything added by hand (untagged) is kept.
		TArray<AActor*> Generated;
		for (AActor* Actor : World->PersistentLevel->Actors)
		{
			if (Actor && Actor->ActorHasTag(GeneratedTag))
			{
				Generated.Add(Actor);
			}
		}
		// Zones first, so none is left holding records for meshes that are already gone.
		Generated.StableSort([](const AActor& A, const AActor& B) { return A.IsA<AGrungifyWeatheringZone>() && !B.IsA<AGrungifyWeatheringZone>(); });
		for (AActor* Actor : Generated)
		{
			Removed += World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true) ? 1 : 0;
		}
	}
	if (AWorldSettings* WorldSettings = World->GetWorldSettings())
	{
		// Static meshes stay Static (as the zones expect) without a lighting build or an "unbuilt lighting" overlay.
		WorldSettings->Modify();
		WorldSettings->bForceNoPrecomputedLighting = true;
	}

	FShowcaseBuilder Builder(World, Assets);
	Builder.SetupLighting();
	Builder.SetupCorridor();

	// -------------------------------------------------------------- Bays
	struct FBayStep
	{
		FText Description;
		bool (FShowcaseBuilder::*Build)(int32);
	};
	const FBayStep Bays[NumBays] = {
		{ LOCTEXT("BayBeforeAfter", "Placing the Before / After bay..."), &FShowcaseBuilder::BuildBeforeAfter },
		{ LOCTEXT("BayIndustrial", "Placing the Abandoned Industrial bay..."), &FShowcaseBuilder::BuildIndustrial },
		{ LOCTEXT("BayTemple", "Placing the Old Temple bay..."), &FShowcaseBuilder::BuildTemple },
		{ LOCTEXT("BaySewer", "Placing the Wet Sewer bay..."), &FShowcaseBuilder::BuildSewer },
		{ LOCTEXT("BayFire", "Placing the Fire & Soot bay..."), &FShowcaseBuilder::BuildFire },
		{ LOCTEXT("BayInstanced", "Placing the Instanced & Stamps bay..."), &FShowcaseBuilder::BuildInstancedAndStamps } };
	for (int32 Index = 0; Index < NumBays; ++Index)
	{
		Progress.EnterProgressFrame(1.f, Bays[Index].Description);
		if (!(Builder.*Bays[Index].Build)(Index))
		{
			return Fail(Builder.Error);
		}
	}

	// ------------------------------------------------------------- Zones
	// Applied only once every bay is in place and always in the same order, so sky-exposure rays and source searches see
	// the same scene on every build and the result is identical.
	TArray<FString> Applied;
	for (const TPair<FString, AGrungifyWeatheringZone*>& Entry : Builder.Zones)
	{
		Progress.EnterProgressFrame(BayFrames * 0.5f / FMath::Max(1, Builder.Zones.Num()),
			FText::Format(LOCTEXT("ApplyZone", "Applying the {0} zone..."), FText::FromString(Entry.Key)));
		AGrungifyWeatheringZone* Zone = Entry.Value;
		Zone->Apply();
		if (Zone->GetAppliedCount() == 0)
		{
			return Fail(FString::Printf(TEXT("the %s zone weathered no meshes (%s)"), *Entry.Key, *Zone->Status));
		}
		Applied.Add(FString::Printf(TEXT("%s %d (seed %d)"), *Entry.Key, Zone->GetAppliedCount(), Zone->Seed));
	}

	// --------------------------------------------------------------- Save
	Progress.EnterProgressFrame(1.f, LOCTEXT("Saving", "Saving the showcase map..."));
	// The zones' owned material instances and mask textures live in their components, inside the map package.
	const bool bSaved = bExistingMap
		? FEditorFileUtils::SaveLevel(World->PersistentLevel)
		: UEditorLoadingAndSavingUtils::SaveMap(World, MapPackage);
	if (!bSaved)
	{
		return Fail(FString::Printf(TEXT("could not save %s"), *MapPackage));
	}
	// Undo would otherwise step back through the half-built showcase after it was saved.
	GEditor->ResetTransaction(LOCTEXT("ResetTransaction", "Grungify showcase built"));

	OutMessage = FString::Printf(TEXT("Grungify showcase: %s %s (now open), %d bays; weathered meshes per zone: %s. Cameras Cam_<Bay> frame each bay."),
		bExistingMap ? TEXT("rebuilt") : TEXT("built"), *MapPackage, NumBays, *FString::Join(Applied, TEXT(", ")));
	if (Removed > 0)
	{
		OutMessage += FString::Printf(TEXT(" Replaced %d previously generated actor(s)."), Removed);
	}
	UE_LOG(LogGrungify, Log, TEXT("%s"), *OutMessage);
	return true;
}

#undef LOCTEXT_NAMESPACE
