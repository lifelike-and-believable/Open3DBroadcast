// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Open3DBroadcastEditor : ModuleRules
{
    public Open3DBroadcastEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Editor-only module, only build when editor is enabled
        if (!Target.bBuildEditor)
        {
            // Build a minimal stub to satisfy references if ever pulled in accidentally
            PublicDependencyModuleNames.AddRange(new string[] { "Core" });
            return;
        }

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Open3DStream",
            "Open3DBroadcast"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
            "UnrealEd",
            "LiveLinkInterface"
        });
    }
}
