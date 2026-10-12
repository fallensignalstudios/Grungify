// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class AGrungifyWeatheringZone;
class UWorld;

/** Weather Selected Actors: one step from a selection to a previewed, correctly sized zone. */
struct FGrungifyQuickStart
{
	/** The zone box for a selection's bounds: padded on every side, and further down so the floor under it is traced. */
	static FBox ZoneBoxFor(const FBox& Bounds);

	/**
	 * Spawns a weathering zone fitted to Actors, limited to exactly those actors, and optionally previews it.
	 * One undo step. Returns null (and why, in OutMessage) when none of the actors has a static or skeletal mesh.
	 */
	static AGrungifyWeatheringZone* WeatherActors(UWorld* World, const TArray<AActor*>& Actors, bool bPreview, FString& OutMessage);

	/** The menu action: the level selection, an offer to hook up its materials, then WeatherActors and select the zone. */
	static void WeatherSelected();
};
