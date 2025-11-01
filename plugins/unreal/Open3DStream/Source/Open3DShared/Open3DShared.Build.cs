// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Open3DShared : ModuleRules
{
    public Open3DShared(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
        });

        PublicIncludePaths.AddRange(new string[]
        {
        });

        PrivateIncludePaths.AddRange(new string[]
        {
        });
    }
}
