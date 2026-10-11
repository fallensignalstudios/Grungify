#pragma once

#include "CoreMinimal.h"
#include "GrungifyTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GrungifyBlueprintLibrary.generated.h"

class AGrungifyWeatheringZone;
class UGrungifyPreset;
class UPrimitiveComponent;

/**
 * Scripting access to weathering zones from Blueprint (Editor Utility Widgets / Blueprints) and Python.
 *
 * Every function is safe to call in any build: the zone actions are editor-only, so outside the editor they do
 * nothing and the run functions return the zone's existing (usually empty) report.
 *
 * Blueprint: Get Zones -> For Each -> Run Zone (Operation = Dry Run) -> Report To Json -> Print String.
 *
 * Python (editor):
 *
 *     import unreal
 *     world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
 *     lib = unreal.GrungifyBlueprintLibrary
 *     for report in lib.run_all_zones(world, unreal.GrungifyOperation.DRY_RUN):
 *         unreal.log("{}: {} weathered, {} skipped".format(report.zone, report.weathered, report.skipped))
 *
 *     zone = lib.find_zone_by_label(world, "Corridor_A")
 *     lib.set_zone_seed(zone, 42, True)
 *     report = lib.run_zone(zone, unreal.GrungifyOperation.APPLY)
 *     lib.save_report(report, unreal.Paths.project_saved_dir() + "Grungify/Corridor_A.json")
 */

UENUM(BlueprintType)
enum class EGrungifyOperation : uint8
{
	DryRun,
	Preview,
	Apply,
	Clear,
	CancelPreview,
	Bake
};

UCLASS()
class GRUNGIFY_API UGrungifyBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Every weathering zone in the world, including Blueprint subclasses (only loaded actors in World Partition levels). */
	UFUNCTION(BlueprintCallable, Category = "Grungify", meta = (WorldContext = "WorldContextObject"))
	static TArray<AGrungifyWeatheringZone*> GetZones(const UObject* WorldContextObject);

	/** First zone whose label (or object name outside the editor) matches, ignoring case. Null if none. */
	UFUNCTION(BlueprintCallable, Category = "Grungify", meta = (WorldContext = "WorldContextObject"))
	static AGrungifyWeatheringZone* FindZoneByLabel(const UObject* WorldContextObject, const FString& Label);

	/** Runs one action on the zone and returns its Last Report. Clear and Cancel Preview don't make a report, so the previous one is returned. */
	UFUNCTION(BlueprintCallable, Category = "Grungify")
	static FGrungifyRunReport RunZone(AGrungifyWeatheringZone* Zone, EGrungifyOperation Operation);

	/** Runs the action on every zone (one undo step in the editor) and returns one report per zone. */
	UFUNCTION(BlueprintCallable, Category = "Grungify", meta = (WorldContext = "WorldContextObject"))
	static TArray<FGrungifyRunReport> RunAllZones(const UObject* WorldContextObject, EGrungifyOperation Operation);

	/** Spawns a native weathering zone. If Preset is set it is assigned and loaded (editor only). */
	UFUNCTION(BlueprintCallable, Category = "Grungify", meta = (WorldContext = "WorldContextObject"))
	static AGrungifyWeatheringZone* SpawnZone(const UObject* WorldContextObject, const FTransform& Transform, FVector HalfExtent, UGrungifyPreset* Preset);

	/** The report as JSON, in the same format as the files in Saved/Grungify/Reports. Empty on failure. */
	UFUNCTION(BlueprintPure, Category = "Grungify")
	static FString ReportToJson(const FGrungifyRunReport& Report);

	/** Writes the report as JSON to FilePath (relative paths are relative to the project directory), creating folders as needed. */
	UFUNCTION(BlueprintCallable, Category = "Grungify")
	static bool SaveReport(const FGrungifyRunReport& Report, const FString& FilePath);

	/** Zones whose box bounds intersect the component's bounds. */
	UFUNCTION(BlueprintCallable, Category = "Grungify")
	static TArray<AGrungifyWeatheringZone*> GetZonesAffecting(const UPrimitiveComponent* Component);

	/** Sets the zone's seed, optionally refreshing its preview (editor only). */
	UFUNCTION(BlueprintCallable, Category = "Grungify")
	static void SetZoneSeed(AGrungifyWeatheringZone* Zone, int32 Seed, bool bPreview);
};
