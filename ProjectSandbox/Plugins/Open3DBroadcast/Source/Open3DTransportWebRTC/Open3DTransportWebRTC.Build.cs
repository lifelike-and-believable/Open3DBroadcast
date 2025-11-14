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
        // Opus library
        var opusLibPath = Path.Combine(PluginDirectory, "ThirdParty", "opus", "lib", platformSubdir, "opus.lib");
        if (!File.Exists(opusLibPath))
        {
            throw new BuildException($"Missing required Open3DTransportWebRTC library 'opus.lib' at '{opusLibPath}'.");
        }
        PublicAdditionalLibraries.Add(opusLibPath);

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Open3DShared",
            "Open3DSender",
            "Open3DReceiver"
        });
    }
}
