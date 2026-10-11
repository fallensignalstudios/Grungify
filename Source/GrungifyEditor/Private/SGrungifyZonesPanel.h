// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Dockable "Grungify Zones" tab: every weathering zone in the editor world, its status and last report,
 * with per-zone and all-zone actions. The widget itself lives in the .cpp; the module only needs these.
 */
namespace GrungifyZonesPanel
{
	extern const FName TabId;

	/** Registers the nomad tab spawner with the global tab manager. Call from StartupModule. */
	void Register();

	/** Call from ShutdownModule. Safe if Slate has already gone. */
	void Unregister();

	/** Opens (or focuses) the tab. */
	void Open();
}
