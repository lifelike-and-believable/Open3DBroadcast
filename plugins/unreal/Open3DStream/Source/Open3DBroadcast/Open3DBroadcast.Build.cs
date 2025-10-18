// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

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
				"Open3DStream"  // Now we can properly depend on Open3DStream since it's in the same plugin
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"LiveLinkInterface",
				"Slate",
				"SlateCore",
				"UnrealEd"
			}
			);
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}