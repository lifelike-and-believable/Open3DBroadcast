// Copyright 2026 Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8).
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DTransportNNG : ModuleRules
{
    public Open3DTransportNNG(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        // Public/Testing/NngTesting.h needs only Core; its consumer (Open3DBroadcastTests) brings
        // the Open3DShared transport headers it includes (SHR-20).
        PublicDependencyModuleNames.Add("Core");

        // O3DBuildFlags turns NNG off on every platform without a prebuilt nng.lib (Win64 only),
        // so a target for another platform gets the stub instead of a build error (FAB-3, TRB-41).
        // The .uplugin's PlatformAllowList normally keeps the module out of such targets anyway.
        if (!O3DBuildFlags.IsNNGEnabled(Target))
        {
            // Stub module: every translation unit is inside #if O3D_WITH_TRANSPORT_NNG (TRB-24).
            O3DBuildFlags.ReportDisabledTransport(Target, "Open3DTransportNNG", "O3D_WITH_TRANSPORT_NNG");
            return;
        }

        // /EHsc: kept from when the o3ds core was a prebuilt library built with exceptions on. The
        // core is now the Open3DStreamCore module, built without exceptions (BUILD-5).
        bEnableExceptions = true;

        // NngSender.cpp uses the o3ds core (docs/adr/0003, WP-F1). NNG itself stays a prebuilt
        // Win64 library in ThirdParty/nng (ADR 0003, decision 8).
        PrivateDependencyModuleNames.Add("Open3DStreamCore");

        var moduleThirdPartyRoot = Path.Combine(ModuleDirectory, "ThirdParty", "nng");

        // NNG headers
        var nngIncludeDir = Path.Combine(moduleThirdPartyRoot, "include");
        if (!Directory.Exists(nngIncludeDir))
        {
            throw new BuildException($"Missing NNG headers at '{nngIncludeDir}'.");
        }
        PublicSystemIncludePaths.Add(nngIncludeDir);

        // NNG library (IsNNGEnabled is true only for Win64)
        var nngLibPath = Path.Combine(moduleThirdPartyRoot, "lib", "Win64", "nng.lib");
        if (!File.Exists(nngLibPath))
        {
            throw new BuildException($"Missing required Open3DTransportNNG library 'nng.lib' at '{nngLibPath}'.");
        }
        PublicAdditionalLibraries.Add(nngLibPath);

        PublicDefinitions.Add("NNG_STATIC_LIB");

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            // The transport blocks, option parsing and interfaces live in Open3DShared; no
            // dependency on Open3DSender or Open3DReceiver (ADR 0007 step 4, WP-A1 PR 4d).
            "Open3DShared",
            "HTTP", // FGenericPlatformHttp::UrlEncode/UrlDecode in NngHelpers.cpp
            "Sockets",
            "Networking"
        });

        // InputCore and ApplicationCore were listed for the old Slate panels only (TRB-46).
        // No editor or Slate dependencies: the settings panel is built by Open3DBroadcastEditor from
        // this transport's option schema (ADR 0010, WP-F7). Build/Scripts/check-runtime-editor-deps.py
        // enforces this in CI.
    }
}
