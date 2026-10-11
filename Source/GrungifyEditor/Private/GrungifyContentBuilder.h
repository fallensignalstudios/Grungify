#pragma once

#include "CoreMinimal.h"

/**
 * Creates the plugin's assets in /Grungify (they can't ship as text in source control):
 *   BP_WeatheringZone, MF_GrungifyWeathering, M_GrungifyMaskDebug, M_GrungifyExample.
 * Only missing assets are created; delete one and rebuild to regenerate it.
 */
struct FGrungifyContentBuilder
{
	/** Returns the number of assets created. */
	static int32 BuildMissingContent();
	static bool IsContentMissing();
};
