// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The demo level shipped with the plugin: /Grungify/Showcase/L_GrungifyShowcase, a corridor of lit bays (Before / After,
 * the three library looks, Fire & Soot, Instanced & Stamps), each with a title, a caption, a framing camera (Cam_<Bay>)
 * and a fixed-seed zone applied and saved into the map. It lives in plugin content so it travels with the plugin, and
 * only references plugin and /Engine content. Its material instances and presets are written to /Grungify/Showcase.
 *
 * Every actor it spawns carries the Grungify.ShowcaseGenerated tag; rebuilding deletes those and spawns them again, so
 * the result is the same every time and hand-added actors without the tag survive.
 */
struct FGrungifyShowcase
{
	static const TCHAR* MapPackageName();

	/**
	 * Creates (or rebuilds) the showcase map and its assets, applies every bay's zone, and saves. Refuses during Play In
	 * Editor, and when the plugin's content folder is read-only (e.g. installed into the engine by the launcher).
	 */
	static bool Build(FString& OutMessage);
};
