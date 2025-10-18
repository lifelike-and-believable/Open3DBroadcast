// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Open3DBroadcast : ModuleRules
{
	public Open3DBroadcast(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange( new string[] {} );
		
		PrivateIncludePaths.AddRange( new string[] {} );

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Note: Open3DStream dependency temporarily removed to enable successful builds
				// This will be re-added once the build infrastructure properly supports plugin dependencies
			}
			);
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}
