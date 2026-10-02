// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;
using System;
using System.IO;

// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8).
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DTransportWebRTC : ModuleRules
{
    public Open3DTransportWebRTC(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // This module lives in the Open3DBroadcastWebRTC add-on plugin (WP-F11, ADR 0002). It must
        // not use O3DBuildFlags from Open3DBroadcast's rules: when Open3DBroadcast is installed
        // from Fab it sits under the engine, its rules are compiled into a different rules
        // assembly, and O3DBuildFlags is internal to that assembly. The add-on reads its own flag.
        bool bWithWebRtc = O3DWebRtcBuildFlags.IsEnabled(Target);
        PrivateDefinitions.Add($"O3D_WITH_TRANSPORT_WEBRTC={(bWithWebRtc ? 1 : 0)}");
        O3DWebRtcBuildFlags.ReportIgnoredFlags();

        // The module has no Public/ headers (SHR-20).
        PublicDependencyModuleNames.Add("Core");

        // O3DWebRtcBuildFlags turns WebRTC off on every platform without a prebuilt livekit_ffi
        // (Win64 only), so a target for another platform gets the stub instead of a build error
        // (FAB-3, TRF-27). The .uplugin's PlatformAllowList normally keeps the module out of such
        // targets.
        if (!bWithWebRtc)
        {
            // Stub module: every translation unit is inside #if O3D_WITH_TRANSPORT_WEBRTC (TRF-27).
            O3DWebRtcBuildFlags.ReportDisabled(Target);
            return;
        }

        // /EHsc: kept from when the o3ds core was a prebuilt library built with exceptions on. The
        // core is now the Open3DStreamCore module, built without exceptions (BUILD-5).
        bEnableExceptions = true;

        // Module-level ThirdParty directory (LiveKit FFI). The flag is on for Win64 only.
        string moduleThirdPartyDir = Path.Combine(ModuleDirectory, "ThirdParty");

        // LiveKit FFI library
        string livekitFfiLibPath = Path.Combine(moduleThirdPartyDir, "livekit_ffi", "lib", "Win64", "livekit_ffi.dll.lib");
        if (!File.Exists(livekitFfiLibPath))
        {
            throw new BuildException($"Missing required LiveKit FFI library at '{livekitFfiLibPath}'.");
        }
        PublicAdditionalLibraries.Add(livekitFfiLibPath);

        // LiveKit FFI include path
        string livekitFfiIncludePath = Path.Combine(moduleThirdPartyDir, "livekit_ffi", "include");
        if (!Directory.Exists(livekitFfiIncludePath))
        {
            throw new BuildException($"Missing required LiveKit FFI include directory at '{livekitFfiIncludePath}'.");
        }
        PublicSystemIncludePaths.Add(livekitFfiIncludePath); // Third-party headers: system include (BUILD-3)

        // LiveKit FFI DLL. Delay-loaded: StartupModule loads it from this plugin's own folder
        // through FO3DFfiLibrary before the first call (TRF-28).
        string livekitFfiDllPath = Path.Combine(moduleThirdPartyDir, "livekit_ffi", "bin", "Win64", "livekit_ffi.dll");
        if (!File.Exists(livekitFfiDllPath))
        {
            throw new BuildException($"Missing required LiveKit FFI DLL at '{livekitFfiDllPath}'.");
        }
        PublicDelayLoadDLLs.Add("livekit_ffi.dll");

        // Register the DLL as a runtime dependency for packaging
        RuntimeDependencies.Add(livekitFfiDllPath);

        // Note: Opus library NOT needed - LiveKit FFI handles Opus encoding/decoding internally.
        // We only provide/receive PCM16 audio at the API boundary.

        // The o3ds core (O3DS::SubjectList) comes from Open3DBroadcast's
        // Open3DStreamCore module, whose API is exported with O3DS_API (ADR 0003, BUILD-1). No
        // path into the other plugin's folders is used, so this works wherever Open3DBroadcast is
        // installed.
        PrivateDependencyModuleNames.Add("Open3DStreamCore");

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "HTTP", // For HTTP token fetching
            "Json", // For JSON parsing
            "JsonUtilities", // For JSON serialization utilities
            "Projects", // IPluginManager: the add-on tests check where livekit_ffi is found (WP-F11)
            // Only Open3DShared's exported API (send queue, receive demux, transport options and
            // their view, the registry), gated by O3D_TRANSPORT_API_VERSION. No Open3DSender or
            // Open3DReceiver since WP-A1 PR 5a: the configure functions take an options view, and
            // the tests' lifetime helpers moved to Open3DShared (Testing/O3DTransportLifetimeTestUtils.h).
            "Open3DShared"
        });

        // No editor or Slate dependencies: the settings panel is built by Open3DBroadcastEditor from
        // this transport's option schema (ADR 0010, WP-F7). Build/Scripts/check-runtime-editor-deps.py
        // enforces this in CI.
    }
}

/// <summary>
/// The add-on's developer-only build switch, read from the environment like Open3DBroadcast's
/// O3DBuildFlags (documented in the add-on README, "Build flags"). A separate class with its own
/// name: both plugins' rules can end up in one rules assembly (two project plugins), and a second
/// O3DBuildFlags there would not compile.
///
///   O3D_WITH_TRANSPORT_WEBRTC  (default 1; forced to 0 on every platform except Win64)
///
/// Nothing is cached, so each target configured by one UBT process gets its own answer (SHR-22).
/// </summary>
internal static class O3DWebRtcBuildFlags
{
    private const string FlagName = "O3D_WITH_TRANSPORT_WEBRTC";

    /// <summary>Flags that selected removed code (BUILD-4): LiveKit is the only WebRTC backend.</summary>
    private static readonly string[] IgnoredFlags =
    {
        "O3D_WEBRTC_BACKEND_LIVEKIT",
        "O3D_WEBRTC_BACKEND_LIBDC"
    };

    /// <summary>livekit_ffi exists for Win64 only (ADR 0001).</summary>
    private static bool HasPrebuiltBinaries(ReadOnlyTargetRules Target)
    {
        return Target.Platform == UnrealTargetPlatform.Win64;
    }

    public static bool IsEnabled(ReadOnlyTargetRules Target)
    {
        return ReadBool(FlagName, true) && HasPrebuiltBinaries(Target);
    }

    public static void ReportDisabled(ReadOnlyTargetRules Target)
    {
        string Reason = HasPrebuiltBinaries(Target) ? $"{FlagName}=0" : $"no prebuilt binaries for {Target.Platform}";
        Console.WriteLine($"Open3DTransportWebRTC: building a stub module without the transport ({Reason}).");
    }

    public static void ReportIgnoredFlags()
    {
        foreach (string Name in IgnoredFlags)
        {
            if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable(Name)))
            {
                Console.WriteLine($"Open3DBroadcastWebRTC: environment variable {Name} is no longer used and is ignored.");
            }
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
}
