// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A fixed visual reference for Grungify: a small lit room in project content (/Game/Grungify/Reference) with stone,
 * metal, trim, floor, a one-quad wall and a Nanite mesh, weathered by one zone with a fixed seed, plus screenshot
 * functional tests framing each case. Run the tests via "Automation RunTests Project.Functional Tests" and approve
 * the first images in the Screenshot Comparison tool; later runs then catch visual regressions.
 */
struct FGrungifyReferenceScene
{
	static const TCHAR* MapPackageName();

	/** Creates (or rebuilds) the map, its materials and screenshot tests, applies the zone with a fixed seed, and saves. */
	static bool Build(FString& OutMessage);
};
