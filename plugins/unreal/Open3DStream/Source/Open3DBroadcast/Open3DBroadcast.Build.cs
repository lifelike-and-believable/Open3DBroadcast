// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Linq; // For IEnumerable<T>.Contains extension
using System.IO;

 [SupportedTargetTypes(TargetType.Game, TargetType.Editor)]
public class Open3DBroadcast : ModuleRules
{
	public Open3DBroadcast(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		// libdatachannel and parts of our code use C++ exceptions
		bEnableExceptions = true;
		
		// Allow conditional compilation via O3DS_WITH_BROADCAST flag
		bool bWithBroadcast = true;
		// Target.GlobalDefinitions can be null under some UAT/BuildPlugin contexts; guard access
		if (Target.GlobalDefinitions != null && Target.GlobalDefinitions.Contains("O3DS_WITH_BROADCAST=0"))
		{
			bWithBroadcast = false;
		}
		
		if (!bWithBroadcast)
		{
			// When broadcast module is disabled, make this a stub module
			PublicDependencyModuleNames.AddRange(new string[] { "Core" });
			PublicDefinitions.Add("O3DS_WITH_BROADCAST=0");
			return;
		}
		
		PublicDefinitions.Add("O3DS_WITH_BROADCAST=1");
		// Broadcast module is decoupled from receiver; default to no stream module available
		PublicDefinitions.Add("O3DS_HAVE_STREAM_MODULE=0");

		// ThirdParty roots no longer needed here; Shared owns WebRTC linkage
		
		PrivateIncludePaths.AddRange( new string[] {} );

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Open3DShared"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"LiveLinkInterface",
				"LiveLink",
				"AnimGraphRuntime",
				"Open3DShared"
			}
			);

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"Sockets",
			"Networking",
			"AudioCaptureCore",
			"AudioMixer",
			"WebSockets",
			"Json",
			"JsonUtilities",
		});

		// ThirdParty roots for core O3DS static libs (same layout as Open3DStream module)
		string ThirdPartyDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "ThirdParty"));
		// Expose O3DS headers to dependent modules
		PublicIncludePaths.AddRange(new string[]
		{
			Path.Combine(ThirdPartyDir, "include"),
		});

		// Link core O3DS libraries needed by broadcast transports and tests
		PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "nng.lib"));
		PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "flatbuffers.lib"));
		PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "open3dstreamstatic.lib"));
		PublicDefinitions.Add("NNG_STATIC_LIB");

		// Ensure required Windows system libraries are available when linking against static libs
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
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

		// WebRTC libraries are linked by Open3DShared
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}