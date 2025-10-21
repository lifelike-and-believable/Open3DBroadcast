// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Linq; // For IEnumerable<T>.Contains extension

public class Open3DBroadcast : ModuleRules
{
	public Open3DBroadcast(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
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
		
		PublicIncludePaths.AddRange( new string[] {} );
		
		PrivateIncludePaths.AddRange( new string[] {} );

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Open3DStream"  // Depend on Open3DStream to access protocol headers and connectors
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"LiveLinkInterface",
				"LiveLink",
				"AnimGraphRuntime"
			}
			);
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}