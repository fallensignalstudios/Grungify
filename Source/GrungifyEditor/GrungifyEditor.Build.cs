// Copyright Fallen Signal Studios. All Rights Reserved.

using UnrealBuildTool;

public class GrungifyEditor : ModuleRules
{
	public GrungifyEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"ToolMenus",
			"MaterialEditor",
			"Projects",
			"RHI",
			"AssetRegistry",
			"DeveloperSettings",
			// AScreenshotFunctionalTest for the reference scene's screenshot tests.
			"FunctionalTesting",
			"RenderCore",
			"ContentBrowser",
			"AssetTools",
			"Landscape",
			"Grungify"
		});
	}
}
