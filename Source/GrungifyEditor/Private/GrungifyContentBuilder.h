#pragma once

#include "CoreMinimal.h"

/** What a content build did, and whether everything connected, compiled and saved. */
struct FGrungifyContentReport
{
	TArray<FString> Created;
	TArray<FString> Rebuilt;
	TArray<FString> Errors;
	TArray<FString> Notes;
	bool bSaveAttempted = false;
	bool bSaved = true;
	bool bReady = false;

	bool HasErrors() const { return Errors.Num() > 0 || !bSaved || !bReady; }
	FString ToString() const;
};

/**
 * Creates and maintains the plugin's assets in /Grungify:
 *   BP_WeatheringZone, MF_GrungifyWeathering, MF_GrungifySubstrate, M_GrungifyMaskDebug, M_GrungifyExample, T_GrungifyNoMask.
 * The generated material assets carry a version marker (content version, custom data index, mask UV channel).
 * Readiness is published only after graph validation, compilation and saving all generated packages.
 * A Saved readiness token binds the marker, shader include and saved package bytes. When it doesn't match,
 * they are rebuilt in place, so every material that already uses MF_GrungifyWeathering
 * picks up fixes and settings changes without relinking.
 */
struct FGrungifyContentBuilder
{
	/** Creates missing assets and rebuilds outdated ones (or all generated ones when bForceRebuild). */
	static FGrungifyContentReport Run(bool bForceRebuild);

	static bool NeedsWork();
};
