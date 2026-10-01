// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

// WP-F11 (ADR 0002): the add-on packaging of the WebRTC transport. The module checks the
// transport-interface version before registering, finds livekit_ffi in its own plugin rather
// than in Open3DBroadcast, and registers under "WebRTC" when both hold.

#if WITH_DEV_AUTOMATION_TESTS

#include "../Shared/WebRTCAddOn.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#include "Transport/O3DTransportApiVersion.h"
#include "Transport/O3DTransportRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCAddOnApiVersionTest, "Open3DBroadcast.Transport.WebRTC.AddOn.ApiVersionCheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCAddOnApiVersionTest::RunTest(const FString& Parameters)
{
	const int32 HostVersion = O3DTransport::GetHostApiVersion();
	FString Error;

	TestTrue(TEXT("This add-on build matches the loaded Open3DBroadcast"), O3DWebRTCAddOn::CheckHostApiVersion(HostVersion, Error));
	TestTrue(TEXT("No message on a match"), Error.IsEmpty());

	TestFalse(TEXT("A newer host is refused"), O3DWebRTCAddOn::CheckHostApiVersion(HostVersion + 1, Error));
	TestTrue(TEXT("The message names the add-on"), Error.Contains(O3DWebRTCAddOn::PluginName));
	TestFalse(TEXT("An older host is refused"), O3DWebRTCAddOn::CheckHostApiVersion(HostVersion - 1, Error));
	TestFalse(TEXT("The refusal has a message"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCAddOnLibraryLocationTest, "Open3DBroadcast.Transport.WebRTC.AddOn.LiveKitLibraryIsInAddOnPlugin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCAddOnLibraryLocationTest::RunTest(const FString& Parameters)
{
	const FO3DFfiLibraryDesc Desc = O3DWebRTCAddOn::MakeLiveKitLibraryDesc();
	TestEqual(TEXT("livekit_ffi is looked up in the add-on plugin"), Desc.OwningPluginName, FString(O3DWebRTCAddOn::PluginName));
	// WP-A1 PR 2: Unload() asks the transport registry about live "WebRTC" instances.
	TestTrue(TEXT("livekit_ffi stays loaded while a WebRTC instance is live"), Desc.TransportNames.Contains(FName(TEXT("WebRTC"))));

	const TSharedPtr<IPlugin> AddOnPlugin = IPluginManager::Get().FindPlugin(O3DWebRTCAddOn::PluginName);
	if (!TestTrue(TEXT("The add-on plugin is known to the plugin manager"), AddOnPlugin.IsValid()))
	{
		return false;
	}
	const FString DllPath = FPaths::Combine(FPaths::ConvertRelativePathToFull(AddOnPlugin->GetBaseDir()), Desc.RelativePath);
	TestTrue(*FString::Printf(TEXT("livekit_ffi.dll exists at %s"), *DllPath), FPaths::FileExists(DllPath));

	// The main plugin no longer carries the DLL (ADR 0002 Decision 1).
	const TSharedPtr<IPlugin> HostPlugin = IPluginManager::Get().FindPlugin(TEXT("Open3DBroadcast"));
	if (TestTrue(TEXT("Open3DBroadcast is loaded"), HostPlugin.IsValid()))
	{
		const FString HostPath = FPaths::Combine(FPaths::ConvertRelativePathToFull(HostPlugin->GetBaseDir()), Desc.RelativePath);
		TestFalse(TEXT("Open3DBroadcast has no livekit_ffi.dll"), FPaths::FileExists(HostPath));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCAddOnRegisteredTest, "Open3DBroadcast.Transport.WebRTC.AddOn.RegistersTransport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCAddOnRegisteredTest::RunTest(const FString& Parameters)
{
	// With a matching host and livekit_ffi present, StartupModule registered one descriptor with
	// both factories, so the sender and receiver pickers list WebRTC (WP-F11 acceptance).
	const FName TransportName(TEXT("WebRTC"));
	TestTrue(TEXT("WebRTC sender is registered"), FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Sender).Contains(TransportName));
	TestTrue(TEXT("WebRTC receiver is registered"), FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Receiver).Contains(TransportName));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_WEBRTC
