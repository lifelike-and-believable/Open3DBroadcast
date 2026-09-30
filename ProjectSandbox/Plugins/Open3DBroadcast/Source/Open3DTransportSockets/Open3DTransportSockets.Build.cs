// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;

// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8).
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DTransportSockets : ModuleRules
{
    public Open3DTransportSockets(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        // Public/Testing/SocketsTesting.h needs only Core; its consumer (Open3DBroadcastTests)
        // brings the Sender and Receiver headers it includes (SHR-20).
        PublicDependencyModuleNames.Add("Core");

        if (!O3DBuildFlags.IsSocketsEnabled(Target))
        {
            // Stub module: every translation unit is inside #if O3D_WITH_TRANSPORT_SOCKETS (TRB-24).
            O3DBuildFlags.ReportDisabledTransport(Target, "Open3DTransportSockets", "O3D_WITH_TRANSPORT_SOCKETS");
            return;
        }

        // /EHsc: kept from when the o3ds core was a prebuilt library built with exceptions on. The
        // core is now the Open3DStreamCore module, built without exceptions (BUILD-5).
        bEnableExceptions = true;

        // The TCP/UDP framing (tcp_stream_parser, udp_fragment) and model code come from the o3ds
        // core (docs/adr/0003, WP-F1).
        PrivateDependencyModuleNames.Add("Open3DStreamCore");

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "Sockets",
            "Networking",
            "Open3DShared",
            "Open3DSender",
            "Open3DReceiver"
        });

        // No editor or Slate dependencies: the settings panel is built by Open3DBroadcastEditor from
        // this transport's option schema (ADR 0010, WP-F7). Build/Scripts/check-runtime-editor-deps.py
        // enforces this in CI.
    }
}
