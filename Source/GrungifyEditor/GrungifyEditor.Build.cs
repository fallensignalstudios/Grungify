// Copyright Fallen Signal Studios. All Rights Reserved.

using UnrealBuildTool;

public class GrungifyEditor : ModuleRules
{
	public GrungifyEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Grungify is public: GrungifyEditorLibrary.h exposes its types (channels, presets) to Blueprint and Python.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Grungify" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
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
			"Landscape"
		});
	}
}
