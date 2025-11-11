using UnrealBuildTool;
using System;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DShared : ModuleRules
{
    public Open3DShared(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new string[] {});
    }
}

internal static class O3DBuildFlags
{
    private sealed class Settings
    {
        public bool BuildSender = true;
        public bool BuildReceiver = true;
        public bool WithSockets = true;
        public bool WithNNG = true;
        public bool WithWebRTC = true;
        public bool WebRtcBackendLiveKit = true;
        public bool WebRtcBackendLibDc = true;
        public bool EnableLegacy = false;
    }

    private static Settings Cached;
    private static readonly object CacheGuard = new object();

    private static Settings Get(ReadOnlyTargetRules Target)
    {
        lock (CacheGuard)
        {
            if (Cached != null)
            {
                return Cached;
            }

            Settings Result = new Settings();

            Result.BuildSender = ReadBool("O3D_BUILD_SENDER", Result.BuildSender);
            Result.BuildReceiver = ReadBool("O3D_BUILD_RECEIVER", Result.BuildReceiver);
            Result.WithSockets = ReadBool("O3D_WITH_TRANSPORT_SOCKETS", Result.WithSockets);
            Result.WithNNG = ReadBool("O3D_WITH_TRANSPORT_NNG", Result.WithNNG);
            Result.WithWebRTC = ReadBool("O3D_WITH_TRANSPORT_WEBRTC", Result.WithWebRTC);
            Result.WebRtcBackendLiveKit = ReadBool("O3D_WEBRTC_BACKEND_LIVEKIT", Result.WebRtcBackendLiveKit);
            Result.WebRtcBackendLibDc = ReadBool("O3D_WEBRTC_BACKEND_LIBDC", Result.WebRtcBackendLibDc);
            Result.EnableLegacy = ReadBool("O3D_ENABLE_LEGACY", Result.EnableLegacy);

            if (!Result.WithWebRTC)
            {
                Result.WebRtcBackendLiveKit = false;
                Result.WebRtcBackendLibDc = false;
            }
            else if (!Result.WebRtcBackendLiveKit && !Result.WebRtcBackendLibDc)
            {
                throw new BuildException("O3D_WITH_TRANSPORT_WEBRTC is enabled, but no WebRTC backend was requested. Set O3D_WEBRTC_BACKEND_LIVEKIT or O3D_WEBRTC_BACKEND_LIBDC to 1.");
            }

            Cached = Result;
            return Result;
        }
    }

    private static bool ReadBool(string EnvVar, bool DefaultValue)
    {
        string Raw = Environment.GetEnvironmentVariable(EnvVar);
        if (string.IsNullOrEmpty(Raw))
        {
            return DefaultValue;
        }

        if (int.TryParse(Raw, out int Numeric))
        {
            return Numeric != 0;
        }

        if (bool.TryParse(Raw, out bool Logical))
        {
            return Logical;
        }

        throw new BuildException($"Environment variable {EnvVar} must be 0/1/true/false, but was '{Raw}'.");
    }

    public static void Apply(ReadOnlyTargetRules Target, ModuleRules Rules, bool bRequireSender = false, bool bRequireReceiver = false)
    {
        Settings Flags = Get(Target);

        Rules.PublicDefinitions.Add($"O3D_BUILD_SENDER={(Flags.BuildSender ? 1 : 0)}");
        Rules.PublicDefinitions.Add($"O3D_BUILD_RECEIVER={(Flags.BuildReceiver ? 1 : 0)}");
        Rules.PublicDefinitions.Add($"O3D_WITH_TRANSPORT_SOCKETS={(Flags.WithSockets ? 1 : 0)}");
        Rules.PublicDefinitions.Add($"O3D_WITH_TRANSPORT_NNG={(Flags.WithNNG ? 1 : 0)}");
        Rules.PublicDefinitions.Add($"O3D_WITH_TRANSPORT_WEBRTC={(Flags.WithWebRTC ? 1 : 0)}");
        Rules.PublicDefinitions.Add($"O3D_WEBRTC_BACKEND_LIVEKIT={(Flags.WebRtcBackendLiveKit ? 1 : 0)}");
        Rules.PublicDefinitions.Add($"O3D_WEBRTC_BACKEND_LIBDC={(Flags.WebRtcBackendLibDc ? 1 : 0)}");
        Rules.PublicDefinitions.Add($"O3D_ENABLE_LEGACY={(Flags.EnableLegacy ? 1 : 0)}");

        if (bRequireSender && !Flags.BuildSender)
        {
            throw new BuildException("Open3DSender cannot be compiled when O3D_BUILD_SENDER=0.");
        }

        if (bRequireReceiver && !Flags.BuildReceiver)
        {
            throw new BuildException("Open3DReceiver cannot be compiled when O3D_BUILD_RECEIVER=0.");
        }
    }

    public static bool IsSenderEnabled(ReadOnlyTargetRules Target) => Get(Target).BuildSender;
    public static bool IsReceiverEnabled(ReadOnlyTargetRules Target) => Get(Target).BuildReceiver;
    public static bool IsSocketsEnabled(ReadOnlyTargetRules Target) => Get(Target).WithSockets;
    public static bool IsNNGEnabled(ReadOnlyTargetRules Target) => Get(Target).WithNNG;
    public static bool IsWebRtcEnabled(ReadOnlyTargetRules Target) => Get(Target).WithWebRTC;
    public static bool IsLegacyEnabled(ReadOnlyTargetRules Target) => Get(Target).EnableLegacy;
}

