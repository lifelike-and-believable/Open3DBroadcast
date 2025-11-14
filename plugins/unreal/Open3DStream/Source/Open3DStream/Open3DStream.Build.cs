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

		// Ensure automation tests in this module are compiled for Editor Debug/Development builds.
		// Some installed engine configurations disable these by default; explicitly enable them
		// so our tests in Private/Tests are discoverable when running the Editor headlessly.
		if ((Target.Configuration == UnrealTargetConfiguration.Debug || Target.Configuration == UnrealTargetConfiguration.Development)
			&& (Target.Type == TargetType.Editor || Target.bBuildEditor))
		{
			PublicDefinitions.Add("WITH_DEV_AUTOMATION_TESTS=1");
		}
		
		// Support for conditional compilation of broadcast features
		// By default, broadcast features are enabled unless explicitly disabled
		// To disable broadcast module, add -D O3DS_WITH_BROADCAST=0 to UBT command line
		// or define O3DS_WITH_BROADCAST=0 in your project's Target.cs GlobalDefinitions


		PublicDefinitions.Add("O3DS_WITH_BROADCAST=1");
		PublicIncludePaths.AddRange( new string[] {} );
		
		// Resolve ThirdParty roots
		string ThirdPartyDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "ThirdParty"));

		PrivateIncludePaths.AddRange(new string[]
		{
			Path.Combine(ThirdPartyDir, "include"),
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
 Path.Combine(ThirdPartyDir, "opus.lib"),
 };
 }
 else if (Target.Platform == UnrealTargetPlatform.Linux)
 {
	 RequiredLibs = new string[] {};
 }
 else if (Target.Platform == UnrealTargetPlatform.Mac)
 {
	 RequiredLibs = new string[] {};
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

		// Expose O3DS headers to dependent modules
		PublicIncludePaths.AddRange(new string[]
		{
			Path.Combine(ThirdPartyDir, "include"),
		});

		PublicDependencyModuleNames.AddRange( new string[] { "Core" } );

 // O3DS static libraries
 PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "nng.lib"));
 PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "flatbuffers.lib"));
 PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "opus.lib"));
 PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyDir, "open3dstreamstatic.lib"));

	// WebRTC libraries are now linked via Open3DShared

 PublicDefinitions.Add("NNG_STATIC_LIB");
	// Enable Opus flag remains globally defined by Shared

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
 "AudioMixer",
 "AudioCaptureCore",
 "Open3DShared",
 }
 );
				
		DynamicallyLoadedModuleNames.AddRange( new string[] {} );
	}
}
