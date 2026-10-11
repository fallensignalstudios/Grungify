#pragma once

#include "CoreMinimal.h"

/**
 * Starter looks in /Game/Grungify/Library: a preset per look (Abandoned Industrial, Old Temple, Wet Sewer) and
 * stone, metal and trim material instances of M_GrungifyExample for each. Project content, so it is never touched
 * by plugin-content rebuilds. Existing assets are kept unless bOverwrite.
 */
struct FGrungifyLibraryBuilder
{
	static bool Build(bool bOverwrite, FString& OutMessage);
};
