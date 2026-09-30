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

        // /EHsc: the sources compile the o3ds core headers, which the core library is built
        // against with exceptions on (BUILD-5).
        bEnableExceptions = true;

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
