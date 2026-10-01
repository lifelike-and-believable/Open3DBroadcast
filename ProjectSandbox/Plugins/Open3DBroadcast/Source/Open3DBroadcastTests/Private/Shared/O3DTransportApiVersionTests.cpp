// Copyright Lifelike & Believable. All Rights Reserved.

// WP-F11 (ADR 0002, ADR 0007 item 2; minimal SHR-14): the transport-interface version an add-on
// transport such as Open3DBroadcastWebRTC checks in StartupModule before it registers anything.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Transport/O3DTransportApiVersion.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportApiVersionHostTest, "Open3DBroadcast.Shared.TransportApiVersion.HostReportsCompiledVersion", O3DB_TEST_FLAGS)
bool FO3DTransportApiVersionHostTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("GetHostApiVersion returns O3D_TRANSPORT_API_VERSION"), O3DTransport::GetHostApiVersion(), static_cast<int32>(O3D_TRANSPORT_API_VERSION));
	TestTrue(TEXT("The version is positive"), O3DTransport::GetHostApiVersion() > 0);
	// ADR 0011 (CTL-2) appended the control-channel virtuals to IOpen3DSender and IOpen3DReceiver,
	// a vtable change that needs version 2 or later; an add-on built against version 1 must refuse.
	TestTrue(TEXT("The control channel interface is version 2 or later"), O3DTransport::GetHostApiVersion() >= 2);
	FString Error;
	TestFalse(TEXT("An add-on built before the control channel is refused"), O3DTransport::CheckApiVersion(TEXT("OldAddOn"), 1, O3DTransport::GetHostApiVersion(), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportApiVersionCheckTest, "Open3DBroadcast.Shared.TransportApiVersion.CheckAcceptsOnlyEqualVersions", O3DB_TEST_FLAGS)
bool FO3DTransportApiVersionCheckTest::RunTest(const FString& Parameters)
{
	const FString AddOn(TEXT("TestAddOn"));
	FString Error(TEXT("stale text"));

	TestTrue(TEXT("Equal versions pass"), O3DTransport::CheckApiVersion(AddOn, 3, 3, Error));
	TestTrue(TEXT("A pass clears the message"), Error.IsEmpty());

	TestFalse(TEXT("An add-on built for an older host fails"), O3DTransport::CheckApiVersion(AddOn, 2, 3, Error));
	TestTrue(TEXT("The message names the add-on"), Error.Contains(AddOn));
	TestTrue(TEXT("The message names the add-on's version"), Error.Contains(TEXT("version 2")));
	TestTrue(TEXT("The message names the host's version"), Error.Contains(TEXT("version 3")));

	TestFalse(TEXT("An add-on built for a newer host fails"), O3DTransport::CheckApiVersion(AddOn, 4, 3, Error));
	TestFalse(TEXT("A failure leaves a message"), Error.IsEmpty());

	TestTrue(TEXT("The loaded host passes its own compiled version"),
		O3DTransport::CheckApiVersion(AddOn, O3D_TRANSPORT_API_VERSION, O3DTransport::GetHostApiVersion(), Error));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
