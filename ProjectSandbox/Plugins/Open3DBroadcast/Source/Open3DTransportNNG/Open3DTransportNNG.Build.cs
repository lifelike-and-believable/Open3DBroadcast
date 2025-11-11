using UnrealBuildTool;
//using O3DBroadcastBuild;
using System.IO;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DTransportNNG : ModuleRules
{
    public Open3DTransportNNG(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        if (!O3DBuildFlags.IsNNGEnabled(Target))
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
            throw new BuildException($"Open3DTransportNNG does not define third-party binaries for platform {Target.Platform} yet.");
        }

        var ModuleThirdPartyLibDir = Path.Combine(ModuleDirectory, "ThirdParty", "Lib", platformSubdir);

        var nngLibPath = Path.Combine(ModuleThirdPartyLibDir, "nng.lib");
        if (!File.Exists(nngLibPath))
        {
            throw new BuildException($"Missing required Open3DTransportNNG library 'nng.lib' at '{nngLibPath}'.");
        }
        PublicAdditionalLibraries.Add(nngLibPath);

        PublicDefinitions.Add("NNG_STATIC_LIB");

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
