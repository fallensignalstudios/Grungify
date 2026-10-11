// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

/**
 * Engine version compatibility helpers for Grungify (UE 5.5 - 5.8).
 *
 * Header-only and deliberately light: no engine headers beyond CoreMinimal and Version.h. Wrappers that need an
 * engine type are templates (the type is only required where they are instantiated, and the call sites already
 * include it) or macros (expanded at the call site, so globals such as GMaxRHIShaderPlatform resolve there).
 *
 * Nothing here was compiled against an engine when it was written. Each switch below names the behaviour it
 * selects so it can be flipped from Grungify.Build.cs (PublicDefinitions.Add("GRUNGIFY_...=0/1")) if the first
 * compile on a given engine version disagrees. See Docs/EngineCompatibility.md for where each wrapper belongs.
 */

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

/** True when compiling against UE Major.Minor or newer. */
#define GRUNGIFY_UE_AT_LEAST(Major, Minor) \
	((ENGINE_MAJOR_VERSION > (Major)) || (ENGINE_MAJOR_VERSION == (Major) && ENGINE_MINOR_VERSION >= (Minor)))

/** True when compiling against an engine older than Major.Minor. */
#define GRUNGIFY_UE_BELOW(Major, Minor) (!GRUNGIFY_UE_AT_LEAST(Major, Minor))

static_assert(GRUNGIFY_UE_AT_LEAST(5, 5), "Grungify supports Unreal Engine 5.5 and newer.");

// ---------------------------------------------------------------------------------------------------------------
// Switches (override from Build.cs if the first compile on an engine version says otherwise)
// ---------------------------------------------------------------------------------------------------------------

/**
 * 1: UMaterial::GetMaterialResource(EShaderPlatform) (what the 5.8 code uses).
 * 0: UMaterial::GetMaterialResource(ERHIFeatureLevel::Type), the older overload.
 * Confidence medium that the shader-platform overload exists on 5.5; verify on first compile.
 */
#ifndef GRUNGIFY_MATERIAL_RESOURCE_BY_SHADER_PLATFORM
	#define GRUNGIFY_MATERIAL_RESOURCE_BY_SHADER_PLATFORM 1
#endif

/**
 * 0: write UStaticMesh::NaniteSettings directly, with deprecation warnings suppressed (works for every version
 *    where the member is still accessible, deprecated or not).
 * 1: use UStaticMesh::GetNaniteSettings()/SetNaniteSettings(). Switch to this only if the member stops being
 *    accessible; the accessors' existence on 5.5 is not verified.
 */
#ifndef GRUNGIFY_STATICMESH_NANITE_ACCESSORS
	#define GRUNGIFY_STATICMESH_NANITE_ACCESSORS 0
#endif

// ---------------------------------------------------------------------------------------------------------------
// Automation tests
// ---------------------------------------------------------------------------------------------------------------

/**
 * UE 5.5 made EAutomationTestFlags an enum class and moved the combined masks to free constants
 * (EAutomationTestFlags_ApplicationContextMask, ...). Individual flags OR'ed together (all Grungify uses today)
 * compile unchanged on 5.5 - 5.8. Use these only if code starts testing masks.
 */
#if GRUNGIFY_UE_AT_LEAST(5, 5)
	#define GRUNGIFY_AUTOMATION_APPLICATION_CONTEXT_MASK EAutomationTestFlags_ApplicationContextMask
	#define GRUNGIFY_AUTOMATION_FILTER_MASK EAutomationTestFlags_FilterMask
#else
	#define GRUNGIFY_AUTOMATION_APPLICATION_CONTEXT_MASK EAutomationTestFlags::ApplicationContextMask
	#define GRUNGIFY_AUTOMATION_FILTER_MASK EAutomationTestFlags::FilterMask
#endif

// ---------------------------------------------------------------------------------------------------------------
// Editor-only wrappers
// ---------------------------------------------------------------------------------------------------------------

#if WITH_EDITOR

/**
 * The material resource for the running RHI's max shader platform (or feature level), or nullptr.
 * A macro so GMaxRHIShaderPlatform / GMaxRHIFeatureLevel resolve at the call site, which already includes
 * "RHIGlobals.h" (add "RHI.h" there if a strict-includes build cannot find the symbol).
 */
#if GRUNGIFY_MATERIAL_RESOURCE_BY_SHADER_PLATFORM
	#define GRUNGIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material) ((Material)->GetMaterialResource(GMaxRHIShaderPlatform))
#else
	#define GRUNGIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material) ((Material)->GetMaterialResource(GMaxRHIFeatureLevel))
#endif

namespace Grungify::Compat
{
	/**
	 * Enables or disables Nanite on a static mesh's build settings. The caller still calls PostEditChange()
	 * (or Build) afterwards to rebuild render data. TStaticMesh is UStaticMesh; the call site includes
	 * "Engine/StaticMesh.h".
	 */
	template <typename TStaticMesh>
	inline void SetNaniteEnabled(TStaticMesh* Mesh, bool bEnabled)
	{
		if (!Mesh)
		{
			return;
		}
#if GRUNGIFY_STATICMESH_NANITE_ACCESSORS
		auto Settings = Mesh->GetNaniteSettings();
		Settings.bEnabled = bEnabled;
		Mesh->SetNaniteSettings(Settings);
#else
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Mesh->NaniteSettings.bEnabled = bEnabled;
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
	}

	/** Whether the mesh's Nanite build setting is on (the setting, not whether Nanite data was built). */
	template <typename TStaticMesh>
	inline bool IsNaniteSettingEnabled(const TStaticMesh* Mesh)
	{
		if (!Mesh)
		{
			return false;
		}
#if GRUNGIFY_STATICMESH_NANITE_ACCESSORS
		return Mesh->GetNaniteSettings().bEnabled;
#else
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return Mesh->NaniteSettings.bEnabled;
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
	}

	/**
	 * Encodes 8-bit BGRA pixels as PNG bytes without any gamma conversion (the masks are linear data).
	 * PNGCompressImageArray(int32, int32, TArrayView64<const FColor>, TArray64<uint8>&) exists from 4.26 to at
	 * least 5.8; deprecation warnings are suppressed in case a version marks it deprecated.
	 * Call as CompressPNG<FImageUtils>(...) from a file that includes "ImageUtils.h". (FImageUtils is a template
	 * argument so this header needs neither that include nor a forward declaration whose class/struct key could
	 * mismatch and warn.)
	 */
	template <typename TImageUtils>
	inline bool CompressPNG(int32 Width, int32 Height, TArrayView64<const FColor> Pixels, TArray64<uint8>& OutPng)
	{
		OutPng.Reset();
		if (Width <= 0 || Height <= 0 || Pixels.Num() != int64(Width) * int64(Height))
		{
			return false;
		}
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		TImageUtils::PNGCompressImageArray(Width, Height, Pixels, OutPng);
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		return OutPng.Num() > 0;
	}
}

#endif // WITH_EDITOR
