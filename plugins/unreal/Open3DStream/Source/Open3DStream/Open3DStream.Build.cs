// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;
using System.Linq; // For IEnumerable<T>.Contains extension

public class Open3DStream : ModuleRules
{
	public Open3DStream(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		// Enable C++ exceptions for libdatachannel (which throws std::exception)
		bEnableExceptions = true;
		
		// Support for conditional compilation of broadcast features
		// By default, broadcast features are enabled unless explicitly disabled
		// To disable broadcast module, add -D O3DS_WITH_BROADCAST=0 to UBT command line
		// or define O3DS_WITH_BROADCAST=0 in your project's Target.cs GlobalDefinitions
		bool bWithBroadcast = true;
		if (Target.GlobalDefinitions.Contains("O3DS_WITH_BROADCAST=0"))
		{
			bWithBroadcast = false;
		}
		PublicDefinitions.Add(bWithBroadcast ? "O3DS_WITH_BROADCAST=1" : "O3DS_WITH_BROADCAST=0");
		
		PublicIncludePaths.AddRange( new string[] {} );
		
		// Resolve ThirdParty roots robustly and combine paths safely (avoid relying on trailing separators)
        string ThirdPartyDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "ThirdParty"));
        string WebRTCDir = Path.GetFullPath(Path.Combine(ThirdPartyDir, "webrtc"));

        // Backward-compatibility alias: legacy code may have referenced 'LibDir' expecting a base path with a trailing separator.
        // Keep it defined to avoid Rules compile errors if cached/staged files still reference it.
        string LibDir = ThirdPartyDir + Path.DirectorySeparatorChar;

		PrivateIncludePaths.AddRange(new string[]
        {
            Path.Combine(ThirdPartyDir, "include"),
            Path.Combine(WebRTCDir, "include")
        });

        // Preflight: ensure expected ThirdParty libraries are present for the active platform.
        // This provides an immediate, actionable error in CI/UAT staging environments.
        string[] RequiredLibs;
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            RequiredLibs = new string[]
            {
                Path.Combine(ThirdPartyDir, "open3dstreamstatic.lib"),
                Path.Combine(ThirdPartyDir, "nng.lib"),
                Path.Combine(ThirdPartyDir, "flatbuffers.lib"),
                Path.Combine(WebRTCDir, "datachannel.lib"),
                Path.Combine(WebRTCDir, "usrsctp.lib"),
                Path.Combine(WebRTCDir, "juice.lib"),
                Path.Combine(WebRTCDir, "mbedtls.lib"),
                Path.Combine(WebRTCDir, "mbedx509.lib"),
                Path.Combine(WebRTCDir, "mbedcrypto.lib"),
            };
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            RequiredLibs = new string[]
            {
                Path.Combine(WebRTCDir, "libdatachannel.a"),
                Path.Combine(WebRTCDir, "libusrsctp.a"),
                Path.Combine(WebRTCDir, "libjuice.a"),
                Path.Combine(WebRTCDir, "libmbedtls.a"),
                Path.Combine(WebRTCDir, "libmbedx509.a"),
                Path.Combine(WebRTCDir, "libmbedcrypto.a"),
            };
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            string MacDir = Path.Combine(WebRTCDir, "macos");
            RequiredLibs = new string[]
            {
                Path.Combine(MacDir, "libdatachannel.a"),
                Path.Combine(MacDir, "libusrsctp.a"),
                Path.Combine(MacDir, "libjuice.a"),
                Path.Combine(MacDir, "libmbedtls.a"),
                Path.Combine(MacDir, "libmbedx509.a"),
                Path.Combine(MacDir, "libmbedcrypto.a"),
            };
        }
        else
        {
            RequiredLibs = new string[0];
        }

        foreach (var libPath in RequiredLibs)
        {
            if (!File.Exists(libPath))
            {
                throw new BuildException($"Open3DStream: Missing required library '{libPath}'. Ensure the 'Build Plugin Core' step populated ThirdParty libs prior to BuildPlugin.");
            }
        }

        // Expose O3DS headers to dependent modules (e.g., Open3DBroadcast)
        PublicIncludePaths.AddRange(new string[]
        {
            LibDir + "include"
        });

		PublicDependencyModuleNames.AddRange( new string[] { "Core" } );

    // O3DS static libraries
    PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "nng.lib"));
    PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "flatbuffers.lib"));
    PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "open3dstreamstatic.lib"));

        // libdatachannel static library for WebRTC support
        // Note: datachannel depends on usrsctp, juice, and mbedtls libraries
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "datachannel.lib"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "usrsctp.lib"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "juice.lib"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "mbedtls.lib"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "mbedx509.lib"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "mbedcrypto.lib"));
            // Required Windows system libraries for libdatachannel/usrsctp/mbedtls
            // Notes:
            //  - Winsock and helpers are needed by usrsctp/juice
            //  - secur32/crypt32 provide TLS and credential routines
            //  - winmm is used for time functions on Windows
            //  - bcrypt is used by mbedtls for entropy
            PublicSystemLibraries.Add("ws2_32.lib");
            PublicSystemLibraries.Add("iphlpapi.lib");
            PublicSystemLibraries.Add("secur32.lib");
            PublicSystemLibraries.Add("crypt32.lib");
            PublicSystemLibraries.Add("winmm.lib");
            PublicSystemLibraries.Add("bcrypt.lib");
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "libdatachannel.a"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "libusrsctp.a"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "libjuice.a"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "libmbedtls.a"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "libmbedx509.a"));
            PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "libmbedcrypto.a"));
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            string MacDir = Path.Combine(WebRTCDir, "macos");
            PublicAdditionalLibraries.Add(Path.Combine(MacDir, "libdatachannel.a"));
            PublicAdditionalLibraries.Add(Path.Combine(MacDir, "libusrsctp.a"));
            PublicAdditionalLibraries.Add(Path.Combine(MacDir, "libjuice.a"));
            PublicAdditionalLibraries.Add(Path.Combine(MacDir, "libmbedtls.a"));
            PublicAdditionalLibraries.Add(Path.Combine(MacDir, "libmbedx509.a"));
            PublicAdditionalLibraries.Add(Path.Combine(MacDir, "libmbedcrypto.a"));
        }

        PublicDefinitions.Add("NNG_STATIC_LIB");
        PublicDefinitions.Add("RTC_STATIC=1"); // Define RTC_STATIC for static libdatachannel

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "Slate",
                "SlateCore",
                "LiveLinkInterface",
                "Networking",
                "Sockets",
                "InputCore",
                "WebSockets",
                "Json",
                "JsonUtilities",
            }
            );
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}
