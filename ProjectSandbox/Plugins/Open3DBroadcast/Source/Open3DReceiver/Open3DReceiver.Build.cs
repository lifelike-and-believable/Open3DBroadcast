using UnrealBuildTool;
//using O3DBroadcastBuild;
using System.IO;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DReceiver : ModuleRules
{
    public Open3DReceiver(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this, bRequireReceiver: true);

        var PluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));

        // Open3DStream headers and library
        var Open3DStreamIncludeDir = Path.Combine(PluginRoot, "ThirdParty", "open3dstream", "include");
        PublicIncludePaths.Add(Open3DStreamIncludeDir);

        // Flatbuffers headers
        var FlatbuffersIncludeDir = Path.Combine(PluginRoot, "ThirdParty", "flatbuffers", "include");
        PublicSystemIncludePaths.Add(FlatbuffersIncludeDir);

        string platformSubdir;
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            platformSubdir = "Win64";
        }
        else
        {
            throw new BuildException($"Open3DReceiver does not define third-party binaries for platform {Target.Platform} yet.");
        }

        // Link libraries
        var Open3DStreamLib = Path.Combine(PluginRoot, "ThirdParty", "open3dstream", "lib", platformSubdir, "open3dstreamstatic.lib");
        var FlatbuffersLib = Path.Combine(PluginRoot, "ThirdParty", "flatbuffers", "lib", platformSubdir, "flatbuffers.lib");

        if (!File.Exists(Open3DStreamLib))
        {
            throw new BuildException($"Missing required library 'open3dstreamstatic.lib' at '{Open3DStreamLib}'.");
        }
        if (!File.Exists(FlatbuffersLib))
        {
            throw new BuildException($"Missing required library 'flatbuffers.lib' at '{FlatbuffersLib}'.");
        }

        PublicAdditionalLibraries.Add(Open3DStreamLib);
        PublicAdditionalLibraries.Add(FlatbuffersLib);

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
			"LiveLink",
            "LiveLinkAnimationCore",
            "LiveLinkInterface",
            "Open3DShared"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects",
            "Slate",
            "SlateCore",
            "AudioMixer"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "PropertyEditor",
                "InputCore"
            });
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
    }
}
