// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

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
        });

        // ThirdParty layout (shared with other modules)
        string ThirdPartyDir = System.IO.Path.GetFullPath(System.IO.Path.Combine(ModuleDirectory, "..", "..", "ThirdParty"));
        string WebRTCDir = System.IO.Path.GetFullPath(System.IO.Path.Combine(ThirdPartyDir, "webrtc"));

        // Expose WebRTC (libdatachannel) headers and ThirdParty includes (e.g., opus) to dependents
        PublicIncludePaths.AddRange(new string[]
        {
            System.IO.Path.Combine(WebRTCDir, "include"),
            System.IO.Path.Combine(ThirdPartyDir, "include"),
        });

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
    }
}
