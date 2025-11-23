using UnrealBuildTool;
using System.IO;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DTransportMoQ : ModuleRules
{
    public Open3DTransportMoQ(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        // Check if MoQ transport is enabled
        if (!O3DBuildFlags.IsMoQEnabled(Target))
        {
            return;
        }

        // Determine platform subdirectory
        string platformSubdir;
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            platformSubdir = "Win64";
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            platformSubdir = "Linux";
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            platformSubdir = "Mac";
        }
        else
        {
            throw new BuildException($"Open3DTransportMoQ does not define third-party binaries for platform {Target.Platform} yet.");
        }

        // Plugin-level ThirdParty directory (for open3dstream core library)
        string pluginThirdPartyDir = Path.Combine(PluginDirectory, "..", "..", "ThirdParty");

        // Module-level ThirdParty directory (for MoQ FFI)
        string moduleThirdPartyDir = Path.Combine(ModuleDirectory, "ThirdParty");

        // MoQ FFI library path
        string moqFfiLibPath = "";
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            moqFfiLibPath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "lib", platformSubdir, "Release", "moq_ffi.dll.lib");
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            moqFfiLibPath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "lib", platformSubdir, "Release", "libmoq_ffi.so");
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            moqFfiLibPath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "lib", platformSubdir, "Release", "libmoq_ffi.dylib");
        }

        if (!File.Exists(moqFfiLibPath))
        {
            throw new BuildException($"Missing required MoQ FFI library at '{moqFfiLibPath}'. " +
                                   $"Please ensure moq-ffi binaries are built for {Target.Platform}. " +
                                   $"See ThirdParty/moq-ffi/README.md for instructions.");
        }
        PublicAdditionalLibraries.Add(moqFfiLibPath);

        // MoQ FFI include path
        string moqFfiIncludePath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "include");
        if (!Directory.Exists(moqFfiIncludePath))
        {
            throw new BuildException($"Missing required MoQ FFI include directory at '{moqFfiIncludePath}'. " +
                                   $"See ThirdParty/moq-ffi/README.md for setup instructions.");
        }
        PublicIncludePaths.Add(moqFfiIncludePath);

        // MoQ FFI DLL/shared library - use delay-load to allow custom path loading
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string moqFfiDllPath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "bin", platformSubdir, "Release", "moq_ffi.dll");
            if (!File.Exists(moqFfiDllPath))
            {
                throw new BuildException($"Missing required MoQ FFI DLL at '{moqFfiDllPath}'. " +
                                       $"See ThirdParty/moq-ffi/README.md for setup instructions.");
            }
            
            // Delay load to allow custom loading logic in MoQFfiSupport
            PublicDelayLoadDLLs.Add("moq_ffi.dll");
            
            // Register the DLL as a runtime dependency for packaging
            RuntimeDependencies.Add(moqFfiDllPath);

            // Also copy PDB if available for debugging
            string moqFfiPdbPath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "bin", platformSubdir, "Release", "moq_ffi.pdb");
            if (File.Exists(moqFfiPdbPath))
            {
                RuntimeDependencies.Add(moqFfiPdbPath);
            }
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            string moqFfiSoPath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "bin", platformSubdir, "Release", "libmoq_ffi.so");
            if (File.Exists(moqFfiSoPath))
            {
                RuntimeDependencies.Add(moqFfiSoPath);
            }
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            string moqFfiDylibPath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "bin", platformSubdir, "Release", "libmoq_ffi.dylib");
            if (File.Exists(moqFfiDylibPath))
            {
                RuntimeDependencies.Add(moqFfiDylibPath);
            }
        }

        // Open3DStream core library includes (for O3DS::SubjectList and audio types)
        string o3dsIncludePath = Path.Combine(pluginThirdPartyDir, "open3dstream", "include");
        if (Directory.Exists(o3dsIncludePath))
        {
            PublicIncludePaths.Add(o3dsIncludePath);
        }

        // Public dependencies
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Projects" // For IPluginManager (DLL loading)
        });

        // Private dependencies
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Open3DShared",
            "Open3DSender",
            "Open3DReceiver",
            "Sockets",      // For address resolution
            "Networking"    // For network utilities
        });

        // Editor-only dependencies
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
