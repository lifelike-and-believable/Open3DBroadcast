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

        // /EHsc: kept from when this module linked the prebuilt open3dstreamstatic.lib, which was
        // built with exceptions on. The core is now the Open3DStreamCore module, built without
        // exceptions (BUILD-5).
        bEnableExceptions = true;

        // The o3ds core, FlatBuffers and o3ds_generated.h come from the Open3DStreamCore module,
        // compiled from source in this plugin (docs/adr/0003, WP-F1). Public: O3DReceiverSource.h
        // includes core headers and has core types as members.
        PublicDependencyModuleNames.Add("Open3DStreamCore");

        // Public, checked against Public/ (SHR-20): UObject types (CoreUObject, Engine), the LiveLink
        // source, factory and settings base classes (LiveLinkInterface), and Shared's transport types.
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "LiveLinkInterface",
            "Open3DShared",
            // UO3DControlSettings (Public/O3DControlSettings.h) is a UDeveloperSettings: the
            // project-wide control channel settings saved to DefaultGame.ini (ADR 0011 item 8).
            // DeveloperSettings is a runtime module, so Shipping builds have it.
            "DeveloperSettings"
        });

        // LiveLink (ULiveLinkPreset in O3DReceiverSource.cpp) and LiveLinkAnimationCore are not
        // needed by the public headers. AudioMixer was listed but never used (RCV-30): the remote
        // audio component uses USoundWaveProcedural and UAudioComponent from Engine.
        // No Slate, SlateCore, PropertyEditor or InputCore: the LiveLink creation panel is built by
        // Open3DBroadcastEditor through O3DReceiver::SetSourceFactoryPanelBuilder (ADR 0010 §3,
        // WP-F7). Build/Scripts/check-runtime-editor-deps.py enforces this in CI.
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects",
            "LiveLink",
            "LiveLinkAnimationCore"
        });

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
