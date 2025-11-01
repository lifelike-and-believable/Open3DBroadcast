// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Linq; // For IEnumerable<T>.Contains extension
using System.IO;

public class Open3DBroadcast : ModuleRules
{
	public Open3DBroadcast(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		// libdatachannel and parts of our code use C++ exceptions
		bEnableExceptions = true;
		
		// Allow conditional compilation via O3DS_WITH_BROADCAST flag
		bool bWithBroadcast = true;
		if (Target.GlobalDefinitions.Contains("O3DS_WITH_BROADCAST=0"))
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

		// ThirdParty roots (shared layout with Open3DStream module)
		string ThirdPartyDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "ThirdParty"));
		string WebRTCDir = Path.GetFullPath(Path.Combine(ThirdPartyDir, "webrtc"));

		// Make libdatachannel headers visible when compiling this module
		PublicIncludePaths.AddRange(new string[]
		{
			Path.Combine(WebRTCDir, "include") // provides <rtc/rtc.hpp>
		});
		
		PrivateIncludePaths.AddRange( new string[] {} );

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Open3DStream"  // Depend on Open3DStream to access protocol headers and connectors
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"LiveLinkInterface",
				"LiveLink",
				"AnimGraphRuntime"
			}
			);

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"Sockets",
			"Networking",
			"AudioCaptureCore",
			"WebSockets",
			"Json",
			"JsonUtilities",
		});

		// Link against libdatachannel and its Windows dependencies because this module
		// directly references rtc::* symbols (headers) in its sources.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDefinitions.Add("RTC_STATIC=1");
			PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "datachannel.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "usrsctp.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "juice.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "srtp2.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "libcrypto.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(WebRTCDir, "libssl.lib"));

			// Required Windows system libs
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
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}