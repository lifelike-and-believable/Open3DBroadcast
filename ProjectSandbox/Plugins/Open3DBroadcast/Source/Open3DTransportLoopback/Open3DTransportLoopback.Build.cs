// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;

// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8).
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DTransportLoopback : ModuleRules
{
    public Open3DTransportLoopback(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        // /EHsc: kept from when the o3ds core was a prebuilt library built with exceptions on. The
        // core is now the Open3DStreamCore module, built without exceptions (BUILD-5).
        bEnableExceptions = true;

        // LoopbackSender.cpp uses the o3ds core (docs/adr/0003, WP-F1).
        PrivateDependencyModuleNames.Add("Open3DStreamCore");

        // The module has no Public/ headers, so every dependency is private (SHR-20).
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            // Open3DShared only: the interfaces, registry, send queue, demux and audio sink base
            // live there; no dependency on Open3DSender or Open3DReceiver (ADR 0007 step 4).
            "Open3DShared"
        });

        // No editor or Slate dependencies: the settings panel is built by Open3DBroadcastEditor from
        // this transport's option schema (ADR 0010, WP-F7). Build/Scripts/check-runtime-editor-deps.py
        // enforces this in CI.
    }
}
