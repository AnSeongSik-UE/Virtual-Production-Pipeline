#include "VPBroadcastRenderer.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

void FVPBroadcastRendererModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VPBroadcastRenderer"));
	check(Plugin.IsValid());
	AddShaderSourceDirectoryMapping(
		TEXT("/Plugin/VPBroadcastRenderer"),
		FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
}

IMPLEMENT_MODULE(FVPBroadcastRendererModule, VPBroadcastRenderer)
