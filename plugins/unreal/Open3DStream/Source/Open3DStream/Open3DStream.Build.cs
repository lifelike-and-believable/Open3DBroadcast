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
	
		PrivateIncludePaths.AddRange( new string[] 
        { LibDir + "include" } );

		PublicDependencyModuleNames.AddRange( new string[] { "Core" } );

        PublicAdditionalLibraries.Add(LibDir + "nng.lib");
        PublicAdditionalLibraries.Add(LibDir + "flatbuffers.lib");
        PublicAdditionalLibraries.Add(LibDir + "open3dstreamstatic.lib");

        PublicDefinitions.Add("NNG_STATIC_LIB");

        // WebRTC Support - Enabled by default if datachannel.lib exists
        // WebRTC options will appear in LiveLink Source dialog if:
        // 1. The Open3DStream library was built with -DO3DS_ENABLE_WEBRTC=ON
        // 2. WebRTC library files exist in the lib directory
        string DataChannelLib = LibDir + "datachannel.lib";
        if (File.Exists(DataChannelLib))
        {
            // Define O3DS_ENABLE_WEBRTC to enable WebRTC code paths
            PublicDefinitions.Add("O3DS_ENABLE_WEBRTC");
            
            PublicAdditionalLibraries.Add(DataChannelLib);
            
            // Add Windows system libraries required by libdatachannel
            if (Target.Platform == UnrealTargetPlatform.Win64)
            {
                PublicSystemLibraries.Add("ws2_32.lib");
                PublicSystemLibraries.Add("bcrypt.lib");
                PublicSystemLibraries.Add("secur32.lib");
                PublicSystemLibraries.Add("iphlpapi.lib");
                PublicSystemLibraries.Add("crypt32.lib");
            }
            
            System.Console.WriteLine("Open3DStream: WebRTC support enabled (datachannel.lib found)");
        }
        else
        {
            System.Console.WriteLine("Open3DStream: WebRTC libraries not found - WebRTC options will be disabled");
        }

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
                "Networking",
                "Sockets",
				// ... add private dependencies that you statically link with here ...	
			}
			);
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}
