// Copyright 2026 Lifelike & Believable. All Rights Reserved.

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

        // The Open3DStream core (Open3DStreamCore) is a private dependency, included only by .cpp
        // files (below); the test-only include paths that used to be here moved to
        // Open3DBroadcastTests (WP-T2, SHR-4, SHR-20).

        // Opus library and headers, under Source/ThirdParty/opus so that BuildPlugin and the Fab
        // zip carry them without a FilterPlugin.ini entry (WP-F1, FAB-2).
        bool bWithOpus = false;
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            var OpusDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "ThirdParty", "opus"));
            var OpusIncludeDir = Path.Combine(OpusDir, "include");
            var OpusLib = Path.Combine(OpusDir, "lib", "Win64", "opus.lib");
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
        // UBlueprintFunctionLibrary, which needs CoreUObject and Engine. Open3DBroadcastSettings.h
        // declares a UDeveloperSettings (WP-U1); DeveloperSettings is a runtime module, so
        // Shipping builds have it.
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "DeveloperSettings"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects", // IPluginManager: FO3DFfiLibrary finds FFI DLLs relative to the owning plugin (TRF-28)
            "Sockets", // O3DTransportOptions::ResolveHostPort resolves host names (TRB-26, WP-A1 step 4)
            // Private: O3DControlConvert.cpp (o3ds/control.h), O3DUnifiedMessage.cpp (the envelope
            // codec) and O3DHelpers.cpp (the name hash; both o3ds/wire_format.h) include the core; Open3DShared's public headers stay free
            // of it (docs/adr/0011-control-channel.md, docs/adr/0009-protocol-versioning.md).
            "Open3DStreamCore"
        });
    }
}
