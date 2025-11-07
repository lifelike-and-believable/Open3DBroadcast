// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Linq; // For IEnumerable<T>.Contains extension
using System.IO;

[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DShared : ModuleRules
{
    public Open3DShared(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bEnableExceptions = true; // libdatachannel and some Shared code use C++ exceptions

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "WebSockets",
            "Json",
            "JsonUtilities",
            "Networking",
            "Sockets",
            "AudioMixer",
            "AudioCaptureCore",
            // Needed for IPluginManager used by LiveKitConnector to locate plugin binaries
            "Projects",
        });

        // ThirdParty layout (shared with other modules)
        string ThirdPartyDir = System.IO.Path.GetFullPath(System.IO.Path.Combine(ModuleDirectory, "..", "..", "ThirdParty"));
        string WebRTCDir = System.IO.Path.GetFullPath(System.IO.Path.Combine(ThirdPartyDir, "webrtc"));
        string LiveKitDir = System.IO.Path.GetFullPath(System.IO.Path.Combine(ThirdPartyDir, "livekit_ffi"));
        string LiveKitInclude = System.IO.Path.Combine(LiveKitDir, "include");
        string LiveKitBinWin64 = System.IO.Path.Combine(LiveKitDir, "bin", "Win64", "Release");
        string PluginBinariesWin64 = System.IO.Path.GetFullPath(System.IO.Path.Combine(ModuleDirectory, "..", "..", "..", "..", "Binaries", "Win64"));
        string[] LiveKitLibCandidates = new string[]
        {
            System.IO.Path.Combine(LiveKitDir, "lib", "Win64", "Release", "livekit_ffi.dll.lib"),
            System.IO.Path.Combine(LiveKitDir, "lib", "Win64", "Release", "livekit-ffi.dll.lib"),
            System.IO.Path.Combine(LiveKitDir, "lib", "Win64", "Release", "livekit_ffi.lib"),
            System.IO.Path.Combine(LiveKitDir, "lib", "Win64", "Release", "livekit-ffi.lib"),
        };
        string LiveKitImportLib = LiveKitLibCandidates.FirstOrDefault(p => System.IO.File.Exists(p)) ?? string.Empty;

        // Expose WebRTC (libdatachannel) headers and ThirdParty includes (e.g., opus) to dependents
        PublicIncludePaths.AddRange(new string[]
        {
            System.IO.Path.Combine(WebRTCDir, "include"),
            System.IO.Path.Combine(ThirdPartyDir, "include"),
        });
        if (Directory.Exists(LiveKitInclude))
        {
            PublicIncludePaths.Add(LiveKitInclude);
        }

        // Link libdatachannel and required deps from this module so others don't need to
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicDefinitions.Add("RTC_STATIC=1");
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "datachannel.lib"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "usrsctp.lib"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "juice.lib"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "srtp2.lib"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "libcrypto.lib"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "libssl.lib"));
            // Opus for audio encode path
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdPartyDir, "opus.lib"));

            // LiveKit FFI (optional; define O3DS_WITH_LIVEKIT if present)
            bool hasLiveKit = !string.IsNullOrEmpty(LiveKitImportLib);
            if (hasLiveKit)
            {
                PublicAdditionalLibraries.Add(LiveKitImportLib);
                PublicDefinitions.Add("O3DS_WITH_LIVEKIT=1");
                if (Directory.Exists(LiveKitBinWin64))
                {
                    foreach (var dllPath in Directory.GetFiles(LiveKitBinWin64, "*.dll"))
                    {
                        var dllName = Path.GetFileName(dllPath);
                        if (!PublicDelayLoadDLLs.Contains(dllName))
                        {
                            PublicDelayLoadDLLs.Add(dllName);
                        }
                        var stagedDest = Path.Combine("$(PluginDir)", "Binaries", "Win64", dllName);
                        RuntimeDependencies.Add(stagedDest, dllPath, StagedFileType.NonUFS);
                        try
                        {
                            Directory.CreateDirectory(PluginBinariesWin64);
                            var copyDest = Path.Combine(PluginBinariesWin64, dllName);
                            if (!File.Exists(copyDest) || File.GetLastWriteTimeUtc(dllPath) > File.GetLastWriteTimeUtc(copyDest))
                            {
                                File.Copy(dllPath, copyDest, true);
                            }
                        }
                        catch {}
                    }
                }
            }
            else
            {
                PublicDefinitions.Add("O3DS_WITH_LIVEKIT=0");
            }

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
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "libdatachannel.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "libusrsctp.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "libjuice.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "libsrtp2.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "libcrypto.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(WebRTCDir, "libssl.a"));
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            string MacDir = System.IO.Path.Combine(WebRTCDir, "macos");
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(MacDir, "libdatachannel.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(MacDir, "libusrsctp.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(MacDir, "libjuice.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(MacDir, "libsrtp2.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(MacDir, "libcrypto.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(MacDir, "libssl.a"));
        }

        PublicDefinitions.Add("O3DS_WITH_OPUS=1");
        PublicDefinitions.Add("O3DS_WITH_WEBRTC=1");
    }
}
