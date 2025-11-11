using UnrealBuildTool;
//using O3DBroadcastBuild;
using System.IO;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DSender : ModuleRules
{
    public Open3DSender(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this, bRequireSender: true);

        var RepoRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", "..", ".."));
        PublicIncludePaths.AddRange(new string[]
        {
            Path.Combine(RepoRoot, "src"),
            Path.Combine(RepoRoot, "thirdparty", "flatbuffers", "include")
        });

        string platformSubdir;
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            platformSubdir = "Win64";
        }
        else
        {
            throw new BuildException($"Open3DSender does not define third-party binaries for platform {Target.Platform} yet.");
        }

        var PluginThirdPartyLibDir = Path.Combine(ModuleDirectory, "..", "..", "ThirdParty", "Lib", platformSubdir);
        var ModuleThirdPartyLibDir = Path.Combine(ModuleDirectory, "ThirdParty", "Lib", platformSubdir);

        string[] commonLibs =
        {
            "open3dstreamstatic.lib",
            "flatbuffers.lib"
        };

        string[] moduleLibs =
        {
        };

        foreach (var lib in commonLibs)
        {
            var fullPath = Path.Combine(PluginThirdPartyLibDir, lib);
            if (!File.Exists(fullPath))
            {
                throw new BuildException($"Missing required common library '{lib}' at '{fullPath}'.");
            }
            PublicAdditionalLibraries.Add(fullPath);
        }

        foreach (var lib in moduleLibs)
        {
            var fullPath = Path.Combine(ModuleThirdPartyLibDir, lib);
            if (!File.Exists(fullPath))
            {
                throw new BuildException($"Missing required Open3DSender library '{lib}' at '{fullPath}'.");
            }
            PublicAdditionalLibraries.Add(fullPath);
        }

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.AddRange(new string[]
            {
                "ws2_32.lib",
                "iphlpapi.lib",
                "secur32.lib",
                "crypt32.lib",
                "winmm.lib",
                "bcrypt.lib"
            });
        }

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Open3DShared"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects",
            "AnimGraphRuntime",
            "AudioCaptureCore",
            "AudioMixer"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "Slate",
                "SlateCore",
                "PropertyEditor",
                "EditorStyle",
                "InputCore"
            });
        }
    }
}
