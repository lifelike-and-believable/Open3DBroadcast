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

        // WebRTC Support - Enabled by default
        // WebRTC options will appear in LiveLink Source dialog if:
        // 1. The Open3DStream library was built with -DO3DS_ENABLE_WEBRTC=ON
        // 2. WebRTC library files exist in the lib directory
        // 
        // To disable WebRTC support, comment out the line below:
        PublicDefinitions.Add("O3DS_ENABLE_WEBRTC");
        
        // Conditionally link WebRTC libraries if they exist
        string DataChannelLib = LibDir + "datachannel.lib";
        if (File.Exists(DataChannelLib))
        {
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
            System.Console.WriteLine("Open3DStream: WebRTC libraries not found - WebRTC options will be available if library was built with WebRTC support");
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
