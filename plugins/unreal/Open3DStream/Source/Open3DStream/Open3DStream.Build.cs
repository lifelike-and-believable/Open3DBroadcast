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
		
		string LibDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/"));
		string O3DSDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/o3ds/"));
		string WebRTCDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/webrtc/"));
	
		PrivateIncludePaths.AddRange( new string[] 
        { 
            O3DSDir + "include",
            WebRTCDir + "include"
        } );

		PublicDependencyModuleNames.AddRange( new string[] { "Core" } );

        // O3DS core static library (prebuilt)
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicAdditionalLibraries.Add(O3DSDir + "lib/Win64/Release/o3ds.lib");
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            PublicAdditionalLibraries.Add(O3DSDir + "lib/Linux/Release/libo3ds.a");
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            PublicAdditionalLibraries.Add(O3DSDir + "lib/Mac/Release/libo3ds.a");
        }

        // Define NNG_STATIC_LIB for O3DS core (NNG is statically linked into o3ds.lib)
        PublicDefinitions.Add("NNG_STATIC_LIB");

        // libdatachannel static library for WebRTC support
        // Note: datachannel depends on usrsctp, juice, and mbedtls libraries
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicAdditionalLibraries.Add(WebRTCDir + "datachannel.lib");
            PublicAdditionalLibraries.Add(WebRTCDir + "usrsctp.lib");
            PublicAdditionalLibraries.Add(WebRTCDir + "juice.lib");
            PublicAdditionalLibraries.Add(WebRTCDir + "mbedtls.lib");
            PublicAdditionalLibraries.Add(WebRTCDir + "mbedx509.lib");
            PublicAdditionalLibraries.Add(WebRTCDir + "mbedcrypto.lib");
            // Required Windows system libraries
            PublicSystemLibraries.Add("bcrypt.lib");  // For BCryptGenRandom (mbedtls entropy)
            PublicSystemLibraries.Add("synchronization.lib");  // For C11 threading (_Thrd_*, _Cnd_*)
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            PublicAdditionalLibraries.Add(WebRTCDir + "libdatachannel.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "libusrsctp.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "libjuice.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "libmbedtls.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "libmbedx509.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "libmbedcrypto.a");
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            PublicAdditionalLibraries.Add(WebRTCDir + "macos/libdatachannel.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "macos/libusrsctp.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "macos/libjuice.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "macos/libmbedtls.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "macos/libmbedx509.a");
            PublicAdditionalLibraries.Add(WebRTCDir + "macos/libmbedcrypto.a");
        }

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
