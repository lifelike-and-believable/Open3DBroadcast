// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8).
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DReceiver : ModuleRules
{
    public Open3DReceiver(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        // /EHsc: this module compiles the o3ds core and FlatBuffers headers and links
        // open3dstreamstatic.lib, which is built with exceptions on (BUILD-5).
        bEnableExceptions = true;

        var PluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));

        // Open3DStream headers and library
        var Open3DStreamIncludeDir = Path.Combine(PluginRoot, "ThirdParty", "open3dstream", "include");
        PublicSystemIncludePaths.Add(Open3DStreamIncludeDir); // Third-party headers: system include (BUILD-3)

        // Flatbuffers headers
        var FlatbuffersIncludeDir = Path.Combine(PluginRoot, "ThirdParty", "flatbuffers", "include");
        PublicSystemIncludePaths.Add(FlatbuffersIncludeDir);

        string platformSubdir;
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            platformSubdir = "Win64";
        }
        else
        {
            // Not reached while the .uplugin keeps "PlatformAllowList": [ "Win64" ] on this module:
            // UBT then leaves the module out of other platforms' targets (ADR 0001). WP-F1 compiles
            // the core from source and deletes this branch.
            throw new BuildException($"Open3DReceiver has prebuilt o3ds libraries for Win64 only, but was configured for {Target.Platform}. Keep \"PlatformAllowList\": [ \"Win64\" ] on its entry in Open3DBroadcast.uplugin.");
        }

        // Link libraries
        var Open3DStreamLib = Path.Combine(PluginRoot, "ThirdParty", "open3dstream", "lib", platformSubdir, "open3dstreamstatic.lib");
        var FlatbuffersLib = Path.Combine(PluginRoot, "ThirdParty", "flatbuffers", "lib", platformSubdir, "flatbuffers.lib");

        if (!File.Exists(Open3DStreamLib))
        {
            throw new BuildException($"Missing required library 'open3dstreamstatic.lib' at '{Open3DStreamLib}'.");
        }
        if (!File.Exists(FlatbuffersLib))
        {
            throw new BuildException($"Missing required library 'flatbuffers.lib' at '{FlatbuffersLib}'.");
        }

        PublicAdditionalLibraries.Add(Open3DStreamLib);
        PublicAdditionalLibraries.Add(FlatbuffersLib);

        // Public, checked against Public/ (SHR-20): UObject types (CoreUObject, Engine), the LiveLink
        // source, factory and settings base classes (LiveLinkInterface), and Shared's transport types.
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "LiveLinkInterface",
            "Open3DShared"
        });

        // LiveLink (ULiveLinkPreset in O3DReceiverSource.cpp) and LiveLinkAnimationCore are not
        // needed by the public headers. AudioMixer was listed but never used (RCV-30): the remote
        // audio component uses USoundWaveProcedural and UAudioComponent from Engine.
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects",
            "LiveLink",
            "LiveLinkAnimationCore",
            "Slate",
            "SlateCore"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "PropertyEditor",
                "InputCore"
            });
        }

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.AddRange(new string[]
            {
                "ws2_32.lib",
                "iphlpapi.lib",
                "secur32.lib",
                "crypt32.lib",
                "winmm.lib",
                "bcrypt.lib"
            });
        }
    }
}
