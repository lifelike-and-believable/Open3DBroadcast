// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;
using System;
using System.IO;

// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8). The .uplugin says
// the same with "TargetDenyList": [ "Server", "Program" ].
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DShared : ModuleRules
{
    public Open3DShared(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // O3DBuildFlags lives in Open3DBroadcastBuildFlags/Open3DBroadcastBuildFlags.Build.cs.
        O3DBuildFlags.Apply(Target, this);
        O3DBuildFlags.ReportIgnoredFlags();

        // No bEnableExceptions: this module has no try/catch and includes no C++ third-party
        // headers (opus.h is C). See BUILD-5.

        var PluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));

        // Open3DShared does not use the Open3DStream core. The core include paths and libraries
        // that used to be here existed only for tests, which now live in Open3DBroadcastTests
        // (WP-T2, SHR-4, SHR-20). Open3DSender and Open3DReceiver add the core themselves.

        // Opus library and headers
        bool bWithOpus = false;
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            var OpusIncludeDir = Path.Combine(PluginRoot, "ThirdParty", "opus", "include");
            var OpusLib = Path.Combine(PluginRoot, "ThirdParty", "opus", "lib", "Win64", "opus.lib");
            if (File.Exists(OpusLib))
            {
                PublicSystemIncludePaths.Add(OpusIncludeDir);
                PublicAdditionalLibraries.Add(OpusLib);
                bWithOpus = true;
            }
        }
        if (!bWithOpus)
        {
            // SHR-21: say so at build time instead of switching Opus off silently.
            Console.WriteLine($"Warning: Open3DShared is built without Opus for {Target.Platform} (O3D_WITH_OPUS=0): Opus audio encoding and decoding are unavailable.");
        }
        PublicDefinitions.Add($"O3D_WITH_OPUS={(bWithOpus ? 1 : 0)}");

        // Public, checked against Public/ (SHR-20): O3DCredentialLibrary.h declares a
        // UBlueprintFunctionLibrary, which needs CoreUObject and Engine.
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects" // IPluginManager: FO3DFfiLibrary finds FFI DLLs relative to the owning plugin (TRF-28)
        });
    }
}
