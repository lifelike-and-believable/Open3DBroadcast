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
				"Open3DStream"
			}
			);
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}
