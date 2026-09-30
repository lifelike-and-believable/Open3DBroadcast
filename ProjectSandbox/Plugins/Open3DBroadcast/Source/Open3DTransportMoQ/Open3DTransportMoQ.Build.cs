// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8).
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DTransportMoQ : ModuleRules
{
    public Open3DTransportMoQ(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        // Public/ headers (MoQFfiApi.h, Testing/MoQTesting.h) need Core and moq_ffi.h; the
        // consumer (Open3DBroadcastTests) brings the Sender and Receiver headers (SHR-20).
        PublicDependencyModuleNames.Add("Core");

        // O3DBuildFlags turns MoQ off on every platform without a prebuilt moq_ffi (Win64 only), so
        // a target for another platform gets the stub instead of a build error. The .uplugin's
        // PlatformAllowList normally keeps the module out of such targets anyway.
        if (!O3DBuildFlags.IsMoQEnabled(Target))
        {
            // Stub module: every translation unit is inside #if O3D_WITH_TRANSPORT_MOQ (TRF-27).
            O3DBuildFlags.ReportDisabledTransport(Target, "Open3DTransportMoQ", "O3D_WITH_TRANSPORT_MOQ");
            return;
        }

        // /EHsc: the sources compile the o3ds core headers, which the core library is built
        // against with exceptions on (BUILD-5).
        bEnableExceptions = true;

        // Module-level ThirdParty directory (MoQ FFI). IsMoQEnabled is true only for Win64; the
        // Linux and Mac branches that pointed at binaries that do not exist are gone (BUILD-2).
        string moduleThirdPartyDir = Path.Combine(ModuleDirectory, "ThirdParty");

        // MoQ FFI import library
        string moqFfiLibPath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "lib", "Win64", "Release", "moq_ffi.dll.lib");
        if (!File.Exists(moqFfiLibPath))
        {
            throw new BuildException($"Missing required MoQ FFI library at '{moqFfiLibPath}'. " +
                                   $"See ThirdParty/moq-ffi/README.md for instructions.");
        }
        PublicAdditionalLibraries.Add(moqFfiLibPath);

        // MoQ FFI include path (public: MoQFfiApi.h includes moq_ffi.h)
        string moqFfiIncludePath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "include");
        if (!Directory.Exists(moqFfiIncludePath))
        {
            throw new BuildException($"Missing required MoQ FFI include directory at '{moqFfiIncludePath}'. " +
                                   $"See ThirdParty/moq-ffi/README.md for setup instructions.");
        }
        PublicSystemIncludePaths.Add(moqFfiIncludePath); // Third-party headers: system include (BUILD-3)

        // MoQ FFI DLL
        string moqFfiDllPath = Path.Combine(moduleThirdPartyDir, "moq-ffi", "bin", "Win64", "Release", "moq_ffi.dll");
        if (!File.Exists(moqFfiDllPath))
        {
            throw new BuildException($"Missing required MoQ FFI DLL at '{moqFfiDllPath}'. " +
                                   $"See ThirdParty/moq-ffi/README.md for setup instructions.");
        }

        // Delay-loaded: the module loads it from the plugin through FO3DFfiLibrary (Open3DShared) first
        PublicDelayLoadDLLs.Add("moq_ffi.dll");

        // Register the DLL as a runtime dependency for packaging
        RuntimeDependencies.Add(moqFfiDllPath);
        // No moq_ffi.pdb here: debug symbols are not staged into packaged games and are
        // not kept in the plugin tree (FAB-4). See Build/README.md, "Debug symbols".

        // The o3ds core headers (O3DS::SubjectList, audio types) come from Open3DSender and
        // Open3DReceiver, which add them as public system includes. The block that used to add
        // <PluginDirectory>/../../ThirdParty/open3dstream/include here pointed outside the plugin
        // and was skipped because that directory does not exist (BUILD-1).

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            // No "Projects": moq_ffi is located and loaded by FO3DFfiLibrary in Open3DShared (TRF-28).
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
                "AppFramework",
                "InputCore" // EKeys symbols used by SComboBox/SListView's key-handling (see Open3DTransportNNG, the other module using SComboBox directly)
            });
        }
    }
}
