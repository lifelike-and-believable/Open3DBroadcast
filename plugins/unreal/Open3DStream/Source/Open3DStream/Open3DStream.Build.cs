// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class Open3DStream : ModuleRules
{
	public Open3DStream(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange( new string[] {} );
		
		string LibDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../lib/"));
		string WebRTCDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../lib/webrtc/"));
	
		PrivateIncludePaths.AddRange( new string[] 
        { 
            LibDir + "include",
            WebRTCDir + "include"
        } );

		PublicDependencyModuleNames.AddRange( new string[] { "Core" } );

        // O3DS static libraries
        PublicAdditionalLibraries.Add(LibDir + "nng.lib");
        PublicAdditionalLibraries.Add(LibDir + "flatbuffers.lib");
        PublicAdditionalLibraries.Add(LibDir + "open3dstreamstatic.lib");

        // libdatachannel static library for WebRTC support
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicAdditionalLibraries.Add(WebRTCDir + "datachannel.lib");
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            PublicAdditionalLibraries.Add(WebRTCDir + "libdatachannel.a");
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            PublicAdditionalLibraries.Add(WebRTCDir + "libdatachannel.a");
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
                // Note: Removed PixelStreaming and WebRTC - using libdatachannel instead
            }
            );
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}
