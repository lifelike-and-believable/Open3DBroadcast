// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8).
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DSender : ModuleRules
{
    public Open3DSender(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        // /EHsc: kept from when this module linked the prebuilt open3dstreamstatic.lib, which was
        // built with exceptions on. The core is now the Open3DStreamCore module, built without
        // exceptions (BUILD-5).
        bEnableExceptions = true;

        // The o3ds core, FlatBuffers and o3ds_generated.h come from the Open3DStreamCore module,
        // compiled from source in this plugin (docs/adr/0003, WP-F1). Public: O3DSenderSerializer.h
        // includes o3ds/sender_sync.h.
        PublicDependencyModuleNames.Add("Open3DStreamCore");

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

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Open3DShared"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects",
            "AnimGraphRuntime",
            "AudioCaptureCore",
            "AudioMixer"
        });

        // No editor or Slate dependencies: the Details customization is in Open3DBroadcastEditor
        // (ADR 0010, WP-F7). Build/Scripts/check-runtime-editor-deps.py enforces this in CI.
    }
}
