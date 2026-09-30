// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

// Automation tests, fakes and the transport conformance suite (ADR 0006, WP-T2).
// Type "Editor" in the .uplugin, so it loads only in editor targets. The Fab package excludes it
// (Build/Fab/exclude-modules.txt). Runtime modules never depend on this module.
[SupportedTargetTypes(TargetType.Editor)]
public class Open3DBroadcastTests : ModuleRules
{
    public Open3DBroadcastTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Same O3D_* definitions as the modules under test.
        O3DBuildFlags.Apply(Target, this);

        // /EHsc: kept from when the o3ds core was a prebuilt library built with exceptions on. The
        // core is now the Open3DStreamCore module, built without exceptions (BUILD-5).
        bEnableExceptions = true;

        // The tests call the o3ds core directly (model, capture, replay; docs/adr/0003, WP-F1).
        PrivateDependencyModuleNames.Add("Open3DStreamCore");

        // Public: the harness headers (conformance registry, fakes, fixtures) expose these types,
        // and the WebRTC add-on test module (WP-F11) builds on them.
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Open3DShared",
            "Open3DSender",
            "Open3DReceiver"
        });

        List<string> PrivateModules = new List<string>
        {
            "Sockets",
            "Networking",
            "LiveLinkInterface",
            "Slate",
            "SlateCore"
        };
        // Loopback has no Testing header: its tests create it through the registry by name.

        if (O3DBuildFlags.IsSocketsEnabled(Target))
        {
            PrivateModules.Add("Open3DTransportSockets");
        }
        if (O3DBuildFlags.IsNNGEnabled(Target))
        {
            PrivateModules.Add("Open3DTransportNNG");
        }
        if (O3DBuildFlags.IsMoQEnabled(Target))
        {
            PrivateModules.Add("Open3DTransportMoQ");
        }
        // Open3DTransportWebRTC is deliberately absent: its tests stay in that module until WP-F11
        // moves them to the add-on's Open3DBroadcastWebRTCTests module.

        PrivateDependencyModuleNames.AddRange(PrivateModules);
    }
}
