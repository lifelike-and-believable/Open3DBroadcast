// Copyright Lifelike & Believable. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

// Editor, Game and Client; Server and Program are excluded (ADR 0001, FAB-8).
[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]
public class Open3DTransportWebRTC : ModuleRules
{
    public Open3DTransportWebRTC(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        O3DBuildFlags.Apply(Target, this);

        // The module has no Public/ headers (SHR-20).
        PublicDependencyModuleNames.Add("Core");

        // O3DBuildFlags turns WebRTC off on every platform without a prebuilt livekit_ffi (Win64
        // only), so a target for another platform gets the stub instead of a build error (FAB-3,
        // TRF-27). The .uplugin's PlatformAllowList normally keeps the module out of such targets.
        if (!O3DBuildFlags.IsWebRtcEnabled(Target))
        {
            // Stub module: every translation unit is inside #if O3D_WITH_TRANSPORT_WEBRTC (TRF-27).
            O3DBuildFlags.ReportDisabledTransport(Target, "Open3DTransportWebRTC", "O3D_WITH_TRANSPORT_WEBRTC");
            return;
        }

        // /EHsc: the sources compile the o3ds core headers, which the core library is built
        // against with exceptions on (BUILD-5).
        bEnableExceptions = true;

        // Module-level ThirdParty directory (LiveKit FFI). IsWebRtcEnabled is true only for Win64.
        string moduleThirdPartyDir = Path.Combine(ModuleDirectory, "ThirdParty");

        // LiveKit FFI library
        string livekitFfiLibPath = Path.Combine(moduleThirdPartyDir, "livekit_ffi", "lib", "Win64", "livekit_ffi.dll.lib");
        if (!File.Exists(livekitFfiLibPath))
        {
            throw new BuildException($"Missing required LiveKit FFI library at '{livekitFfiLibPath}'.");
        }
        PublicAdditionalLibraries.Add(livekitFfiLibPath);

        // LiveKit FFI include path
        string livekitFfiIncludePath = Path.Combine(moduleThirdPartyDir, "livekit_ffi", "include");
        if (!Directory.Exists(livekitFfiIncludePath))
        {
            throw new BuildException($"Missing required LiveKit FFI include directory at '{livekitFfiIncludePath}'.");
        }
        PublicSystemIncludePaths.Add(livekitFfiIncludePath); // Third-party headers: system include (BUILD-3)

        // LiveKit FFI DLL - use delay-load to allow custom path loading
        string livekitFfiDllPath = Path.Combine(moduleThirdPartyDir, "livekit_ffi", "bin", "Win64", "livekit_ffi.dll");
        if (!File.Exists(livekitFfiDllPath))
        {
            throw new BuildException($"Missing required LiveKit FFI DLL at '{livekitFfiDllPath}'.");
        }
        PublicDelayLoadDLLs.Add("livekit_ffi.dll");

        // Register the DLL as a runtime dependency for packaging
        RuntimeDependencies.Add(livekitFfiDllPath);

        // Note: Opus library NOT needed - LiveKit FFI handles Opus encoding/decoding internally.
        // We only provide/receive PCM16 audio at the API boundary.

        // The o3ds core headers (O3DS::SubjectList, audio types) come from Open3DSender and
        // Open3DReceiver, which add them as public system includes. The block that used to add
        // <PluginDirectory>/../../ThirdParty/open3dstream/include here pointed outside the plugin
        // and was skipped because that directory does not exist (BUILD-1).

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "HTTP", // For HTTP token fetching
            "Json", // For JSON parsing
            "JsonUtilities", // For JSON serialization utilities
            "Open3DShared",
            "Open3DSender",
            "Open3DReceiver"
        });

        // No editor or Slate dependencies: the settings panel is built by Open3DBroadcastEditor from
        // this transport's option schema (ADR 0010, WP-F7). Build/Scripts/check-runtime-editor-deps.py
        // enforces this in CI.
    }
}
