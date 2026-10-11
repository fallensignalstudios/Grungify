#include "GrungifySettings.h"

#include "Materials/MaterialInterface.h"

UGrungifySettings::UGrungifySettings()
{
	StoneKeywords = { TEXT("stone"), TEXT("rock"), TEXT("brick"), TEXT("concrete"), TEXT("cement"), TEXT("marble"), TEXT("granite"),
		TEXT("cobble"), TEXT("masonry"), TEXT("plaster"), TEXT("stucco"), TEXT("slate"), TEXT("sandstone"), TEXT("limestone"), TEXT("tile") };

	MetalKeywords = { TEXT("metal"), TEXT("steel"), TEXT("iron"), TEXT("copper"), TEXT("brass"), TEXT("bronze"), TEXT("alum"),
		TEXT("chrome"), TEXT("zinc"), TEXT("rust"), TEXT("galvan"), TEXT("corrugated") };

	TrimKeywords = { TEXT("trim"), TEXT("molding"), TEXT("moulding"), TEXT("cornice"), TEXT("baseboard"), TEXT("skirting"),
		TEXT("frame"), TEXT("sill"), TEXT("lintel"), TEXT("coping"), TEXT("bevel"), TEXT("border") };

	RailKeywords = { TEXT("rail"), TEXT("handrail"), TEXT("banister"), TEXT("balustrade"), TEXT("handle"), TEXT("grip"),
		TEXT("ladder"), TEXT("lever"), TEXT("knob") };

	VentKeywords = { TEXT("vent"), TEXT("drain"), TEXT("spout"), TEXT("outlet"), TEXT("gutter"), TEXT("downpipe"),
		TEXT("louver"), TEXT("louvre"), TEXT("exhaust"), TEXT("grate"), TEXT("scupper") };

	FireKeywords = { TEXT("fire"), TEXT("torch"), TEXT("brazier"), TEXT("candle"), TEXT("lantern"), TEXT("furnace"), TEXT("forge"),
		TEXT("hearth"), TEXT("fireplace"), TEXT("stove"), TEXT("campfire") };

	FastenerKeywords = { TEXT("bolt"), TEXT("rivet"), TEXT("screw"), TEXT("nut"), TEXT("fastener"), TEXT("bracket"), TEXT("clamp") };

	MaskDebugMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Grungify/Materials/M_GrungifyMaskDebug.M_GrungifyMaskDebug")));
}
