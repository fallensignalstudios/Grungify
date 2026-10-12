// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "ComponentVisualizer.h"

/** Draws a selected zone's dust height, hand-height band and runoff sources in the viewport. */
class FGrungifyZoneVisualizer : public FComponentVisualizer
{
public:
	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
};
