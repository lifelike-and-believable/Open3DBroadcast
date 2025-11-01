// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Linq; // For IEnumerable<T>.Contains extension
using System.IO;

public class Open3DBroadcast : ModuleRules
{
	public Open3DBroadcast(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		// libdatachannel and parts of our code use C++ exceptions
		bEnableExceptions = true;
		
		// Allow conditional compilation via O3DS_WITH_BROADCAST flag
		bool bWithBroadcast = true;
		if (Target.GlobalDefinitions.Contains("O3DS_WITH_BROADCAST=0"))
		{
			bWithBroadcast = false;
		}
		
		if (!bWithBroadcast)
		{
			// When broadcast module is disabled, make this a stub module
			PublicDependencyModuleNames.AddRange(new string[] { "Core" });
			PublicDefinitions.Add("O3DS_WITH_BROADCAST=0");
			return;
		}
		
		PublicDefinitions.Add("O3DS_WITH_BROADCAST=1");

		// ThirdParty roots no longer needed here; Shared owns WebRTC linkage
		
		PrivateIncludePaths.AddRange( new string[] {} );

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Open3DShared",
				"Open3DStream"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"LiveLinkInterface",
				"LiveLink",
				"AnimGraphRuntime",
				"Open3DShared"
			}
			);

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"Sockets",
			"Networking",
			"AudioCaptureCore",
			"WebSockets",
			"Json",
			"JsonUtilities",
		});

		// WebRTC libraries are linked by Open3DShared
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}