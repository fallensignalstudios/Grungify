// Copyright Fallen Signal Studios. All Rights Reserved.

using UnrealBuildTool;

public class GrungifyShaders : ModuleRules
{
	public GrungifyShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"Projects",
			"RenderCore"
		});
	}
}
