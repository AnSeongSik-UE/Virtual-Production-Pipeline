// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class VPPipeline : ModuleRules
{
	public VPPipeline(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"UMG",
			"VPTrackerReceiver",
			"Spout2_DX12",
			"VPBroadcastRenderer",
			"VRM4U",
			"VRM4ULoader"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Slate",
			"SlateCore",
			"Sockets",
			"Json",
			"RenderCore"
		});
		
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new string[] {
				"Ole32.lib",
				"Shell32.lib",
				"Uuid.lib"
			});
		}

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
