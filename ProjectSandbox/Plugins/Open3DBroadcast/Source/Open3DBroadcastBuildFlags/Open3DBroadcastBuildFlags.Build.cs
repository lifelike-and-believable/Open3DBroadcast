// Copyright Lifelike & Believable. All Rights Reserved.

using System;
using UnrealBuildTool;

// Holds O3DBuildFlags, the developer-only build switches shared by every module rules file of
// this plugin (WP-F2, SHR-22). UBT compiles all *.Build.cs files of the plugin into one rules
// assembly, so the other modules' rules can call O3DBuildFlags directly.
//
// The file needs a *.Build.cs name to be compiled at all, which makes UBT treat it as the rules of
// a module called Open3DBroadcastBuildFlags. That module is not listed in the .uplugin and no
// module depends on it, so it is never built. It is declared External (no sources, no binary) so
// that it stays harmless if a tool ever instantiates it.
public class Open3DBroadcastBuildFlags : ModuleRules
{
    public Open3DBroadcastBuildFlags(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;
    }
}

/// <summary>
/// Build switches read from environment variables (developer builds only; a Fab user cannot set
/// them). Documented in the plugin README, "Build flags".
///
///   O3D_WITH_TRANSPORT_SOCKETS  (default 1)
///   O3D_WITH_TRANSPORT_NNG      (default 1; forced to 0 on every platform except Win64)
///   O3D_WITH_TRANSPORT_MOQ      (default 1; forced to 0 on every platform except Win64)
///
/// O3D_WITH_TRANSPORT_WEBRTC is not here: the WebRTC transport lives in the Open3DBroadcastWebRTC
/// add-on plugin, whose Open3DTransportWebRTC.Build.cs reads it (WP-F11, SHR-19).
///
/// A transport switched off still has its module entry in the .uplugin, so UBT still builds it.
/// Its Build.cs then adds only Core, and every translation unit of the module is wrapped in
/// #if O3D_WITH_TRANSPORT_&lt;X&gt;, so the module compiles to a stub that registers nothing.
///
/// Nothing is cached: every call reads the environment again and applies the platform overrides
/// for the target being configured. One UBT process can configure several targets (for example a
/// Win64 editor and a Linux game), and each must get its own answer (SHR-22).
/// </summary>
internal static class O3DBuildFlags
{
    private sealed class Settings
    {
        public bool WithSockets;
        public bool WithNNG;
        public bool WithMoQ;
    }

    /// <summary>
    /// Flags that no longer exist. O3D_BUILD_SENDER=0 and O3D_BUILD_RECEIVER=0 never produced a
    /// build: Open3DSender and Open3DReceiver declare UObject types, which UHT generates code for
    /// whatever the preprocessor says, and every transport links both. Setting either to 0 is an
    /// error, so nobody gets a sender or receiver they asked to leave out.
    /// </summary>
    private static readonly string[] RemovedModuleFlags =
    {
        "O3D_BUILD_SENDER",
        "O3D_BUILD_RECEIVER"
    };

    /// <summary>
    /// Flags that selected code which has been removed (BUILD-4): no source reads O3D_ENABLE_LEGACY.
    /// They are ignored. The removed WebRTC backend flags are reported by the add-on (WP-F11).
    /// </summary>
    private static readonly string[] IgnoredFlags =
    {
        "O3D_ENABLE_LEGACY"
    };

    /// <summary>NNG and MoQ link prebuilt binaries that exist for Win64 only (ADR 0001).</summary>
    public static bool HasPrebuiltTransportBinaries(ReadOnlyTargetRules Target)
    {
        return Target.Platform == UnrealTargetPlatform.Win64;
    }

    private static Settings Get(ReadOnlyTargetRules Target)
    {
        CheckRemovedFlags();

        bool bBinaries = HasPrebuiltTransportBinaries(Target);
        Settings Result = new Settings();
        Result.WithSockets = ReadBool("O3D_WITH_TRANSPORT_SOCKETS", true);
        Result.WithNNG = ReadBool("O3D_WITH_TRANSPORT_NNG", true) && bBinaries;
        Result.WithMoQ = ReadBool("O3D_WITH_TRANSPORT_MOQ", true) && bBinaries;
        return Result;
    }

    private static void CheckRemovedFlags()
    {
        foreach (string Name in RemovedModuleFlags)
        {
            if (!ReadBool(Name, true))
            {
                throw new BuildException($"{Name}=0 is not supported. Open3DSender and Open3DReceiver are always built; only the transports can be switched off, with O3D_WITH_TRANSPORT_<NAME>=0 (see the plugin README, \"Build flags\"). Unset {Name}.");
            }
        }
    }

    /// <summary>
    /// Prints a notice for each ignored flag that is set. Called from Open3DShared's rules only, so
    /// the notice appears once per target rather than once per module.
    /// </summary>
    public static void ReportIgnoredFlags()
    {
        foreach (string Name in IgnoredFlags)
        {
            if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable(Name)))
            {
                Console.WriteLine($"Open3DBroadcast: environment variable {Name} is no longer used and is ignored.");
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

    /// <summary>
    /// Adds the O3D_WITH_TRANSPORT_* definitions. Every module of the plugin calls this, so the
    /// values are the same in every translation unit of one target.
    /// </summary>
    public static void Apply(ReadOnlyTargetRules Target, ModuleRules Rules)
    {
        Settings Flags = Get(Target);
        Rules.PublicDefinitions.Add($"O3D_WITH_TRANSPORT_SOCKETS={(Flags.WithSockets ? 1 : 0)}");
        Rules.PublicDefinitions.Add($"O3D_WITH_TRANSPORT_NNG={(Flags.WithNNG ? 1 : 0)}");
        Rules.PublicDefinitions.Add($"O3D_WITH_TRANSPORT_MOQ={(Flags.WithMoQ ? 1 : 0)}");
    }

    /// <summary>
    /// Called by a transport's own Build.cs when the transport is off, so the reason is printed
    /// once per target by the module concerned.
    /// </summary>
    public static void ReportDisabledTransport(ReadOnlyTargetRules Target, string ModuleName, string EnvVar)
    {
        string Reason = HasPrebuiltTransportBinaries(Target)
            ? $"{EnvVar}=0"
            : $"no prebuilt binaries for {Target.Platform}";
        Console.WriteLine($"{ModuleName}: building a stub module without the transport ({Reason}).");
    }

    public static bool IsSocketsEnabled(ReadOnlyTargetRules Target) => Get(Target).WithSockets;
    public static bool IsNNGEnabled(ReadOnlyTargetRules Target) => Get(Target).WithNNG;
    public static bool IsMoQEnabled(ReadOnlyTargetRules Target) => Get(Target).WithMoQ;
}
