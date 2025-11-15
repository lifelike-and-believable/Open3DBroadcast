using UnrealBuildTool;
//using O3DBroadcastBuild;
using System.IO;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DTransportWebRTC : ModuleRules
{
    public Open3DTransportWebRTC(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        if (!O3DBuildFlags.IsWebRtcEnabled(Target))
        {
            return;
        }

        string platformSubdir;
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            platformSubdir = "Win64";
        }
        else
        {
            throw new BuildException($"Open3DTransportWebRTC does not define third-party binaries for platform {Target.Platform} yet.");
        }

        // Plugin-level ThirdParty directory
        string pluginThirdPartyDir = Path.Combine(PluginDirectory, "..", "..", "ThirdParty");

        // Module-level ThirdParty directory (for LiveKit FFI)
        string moduleThirdPartyDir = Path.Combine(ModuleDirectory, "ThirdParty");

        // LiveKit FFI library
        string livekitFfiLibPath = Path.Combine(moduleThirdPartyDir, "livekit_ffi", "lib", platformSubdir, "livekit_ffi.dll.lib");
        if (!File.Exists(livekitFfiLibPath))
        {
            throw new BuildException($"Missing required LiveKit FFI library at '{livekitFfiLibPath}'.");
        }
        PublicAdditionalLibraries.Add(livekitFfiLibPath);

        // LiveKit FFI include path
        string livekitFfiIncludePath = Path.Combine(moduleThirdPartyDir, "livekit_ffi", "include");
        if (Directory.Exists(livekitFfiIncludePath))
        {
            PublicIncludePaths.Add(livekitFfiIncludePath);
        }

        // Stage LiveKit FFI DLL to Binaries folder
        string livekitFfiDllPath = Path.Combine(moduleThirdPartyDir, "livekit_ffi", "bin", platformSubdir, "livekit_ffi.dll");
        if (File.Exists(livekitFfiDllPath))
        {
            RuntimeDependencies.Add(livekitFfiDllPath);
        }

        // Opus library (plugin-level ThirdParty)
        string opusLibPath = Path.Combine(pluginThirdPartyDir, "opus", "lib", platformSubdir, "opus.lib");
        if (!File.Exists(opusLibPath))
        {
            throw new BuildException($"Missing required Opus library 'opus.lib' at '{opusLibPath}'.");
        }
        PublicAdditionalLibraries.Add(opusLibPath);

        // Opus include path
        string opusIncludePath = Path.Combine(pluginThirdPartyDir, "Include");
        if (Directory.Exists(opusIncludePath))
        {
            PublicIncludePaths.Add(opusIncludePath);
        }

        // Open3DStream core library includes
        string o3dsIncludePath = Path.Combine(pluginThirdPartyDir, "open3dstream", "include");
        if (Directory.Exists(o3dsIncludePath))
        {
            PublicIncludePaths.Add(o3dsIncludePath);
        }

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Projects" // For IPluginManager
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Open3DShared",
            "Open3DSender",
            "Open3DReceiver"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "Slate",
                "SlateCore",
                "AppFramework"
            });
        }
    }
}
