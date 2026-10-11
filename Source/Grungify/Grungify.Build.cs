// Copyright Fallen Signal Studios. All Rights Reserved.

using UnrealBuildTool;

public class Grungify : ModuleRules
{
	public Grungify(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"PhysicsCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Landscape weathering (component material overrides, heights).
			"Landscape",
			"Json",
			"JsonUtilities",
			"RenderCore",
			"RHI"
		});

		if (Target.bBuildEditor)
		{
			// Transactions (undo/redo) for Preview / Apply / Clear.
			PrivateDependencyModuleNames.Add("UnrealEd");
			// Creating and saving preset assets.
			PrivateDependencyModuleNames.Add("AssetRegistry");
		}
	}
}
