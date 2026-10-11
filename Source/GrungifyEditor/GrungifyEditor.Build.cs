using UnrealBuildTool;

public class GrungifyEditor : ModuleRules
{
	public GrungifyEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

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
			"Grungify"
		});
	}
}
