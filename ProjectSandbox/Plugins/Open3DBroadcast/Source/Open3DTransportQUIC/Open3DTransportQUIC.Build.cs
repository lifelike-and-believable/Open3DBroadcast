using UnrealBuildTool;
using System.IO;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DTransportQUIC : ModuleRules
{
    public Open3DTransportQUIC(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        if (!O3DBuildFlags.IsQuicEnabled(Target))
        {
            return;
        }

        if (Target.Platform != UnrealTargetPlatform.Win64)
        {
            throw new BuildException($"Open3DTransportQUIC does not define third-party binaries for platform {Target.Platform} yet.");
        }

        string moduleThirdPartyRoot = Path.Combine(ModuleDirectory, "ThirdParty", "msquic");

        string includeDir = Path.Combine(moduleThirdPartyRoot, "include");
        if (!Directory.Exists(includeDir))
        {
            throw new BuildException($"Missing MsQuic headers at '{includeDir}'.");
        }
        PublicSystemIncludePaths.Add(includeDir);

        string libPath = Path.Combine(moduleThirdPartyRoot, "lib", "Win64", "msquic.lib");
        if (!File.Exists(libPath))
        {
            throw new BuildException($"Missing MsQuic import library at '{libPath}'.");
        }
        PublicAdditionalLibraries.Add(libPath);

        string dllPath = Path.Combine(moduleThirdPartyRoot, "bin", "Win64", "msquic.dll");
        if (!File.Exists(dllPath))
        {
            throw new BuildException($"Missing MsQuic runtime DLL at '{dllPath}'.");
        }
        PublicDelayLoadDLLs.Add("msquic.dll");
        RuntimeDependencies.Add(dllPath);

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
            "Open3DReceiver",
            "Projects",
            "Sockets",
            "Networking"
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
