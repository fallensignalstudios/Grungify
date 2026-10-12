// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UGrungifyPreset;

/** Base colours of a look's three material families (stone, metal, trim), as the library's material instances use them. */
struct FGrungifyLookColors
{
	FLinearColor Stone = FLinearColor::White;
	FLinearColor Metal = FLinearColor::White;
	float MetalMetallic = 0.f;
	FLinearColor Trim = FLinearColor::White;
};

/**
 * Starter looks in /Game/Grungify/Library: a preset per look (Abandoned Industrial, Old Temple, Wet Sewer) and
 * stone, metal and trim material instances of M_GrungifyExample for each. Project content, so it is never touched
 * by plugin-content rebuilds. Existing assets are kept unless bOverwrite.
 */
struct FGrungifyLibraryBuilder
{
	static bool Build(bool bOverwrite, FString& OutMessage);

	/** The looks Build creates, by name (AbandonedIndustrial, OldTemple, WetSewer), so other builders can reuse them. */
	static TArray<FString> LookNames();

	/**
	 * Resets Preset to the class defaults and writes LookName's settings and notes into it (bIncludeSeed off), exactly as
	 * Build does for GP_<LookName>. Does not mark the package dirty. False (Preset untouched) if the name is unknown.
	 */
	static bool ConfigureLook(const FString& LookName, UGrungifyPreset& Preset);

	/** LookName's material base colours; false if the name is unknown. */
	static bool GetLookColors(const FString& LookName, FGrungifyLookColors& OutColors);
};
