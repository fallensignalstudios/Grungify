// Copyright Fallen Signal Studios. All Rights Reserved.

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

/**
 * Maps the plugin's Shaders folder to /Plugin/Grungify so the custom nodes in
 * MF_GrungifyWeathering can include /Plugin/Grungify/Private/Grungify.ush.
 * Lives in its own module because it has to load at PostConfigInit, before any
 * material is compiled.
 */
class FGrungifyShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Grungify"));
		if (!Plugin.IsValid())
		{
			return;
		}

		const FString VirtualPath = TEXT("/Plugin/Grungify");
		if (!AllShaderSourceDirectoryMappings().Contains(VirtualPath))
		{
			AddShaderSourceDirectoryMapping(VirtualPath, FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
	}
};

IMPLEMENT_MODULE(FGrungifyShadersModule, GrungifyShaders)
