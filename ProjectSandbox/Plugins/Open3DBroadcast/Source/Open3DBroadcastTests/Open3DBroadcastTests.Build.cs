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

        // /EHsc: the tests compile the o3ds core and FlatBuffers headers, like the Sender and
        // Receiver modules (BUILD-5).
        bEnableExceptions = true;

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

        // Open3DBroadcastEditor and UnrealEd: the WP-F7 panel tests build SO3DTransportOptionsPanel
        // and check the editor's transaction buffer (ADR 0010 §8).
        List<string> PrivateModules = new List<string>
        {
            "Sockets",
            "Networking",
            "LiveLinkInterface",
            "Slate",
            "SlateCore",
            "UnrealEd",
            "Open3DBroadcastEditor"
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
