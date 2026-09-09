using UnrealBuildTool;

public class VPBroadcastRenderer : ModuleRules
{
	public VPBroadcastRenderer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"Engine",
			"RHI"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Projects",
			"RenderCore",
			"Renderer"
		});
	}
}
