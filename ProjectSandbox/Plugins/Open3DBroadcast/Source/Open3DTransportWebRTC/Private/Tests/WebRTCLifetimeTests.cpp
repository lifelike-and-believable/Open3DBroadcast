// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DBuildFlags).

//
// WP-S5 tests for the WebRTC sender (TRF-1, TRF-40).
//
// This stress test uses the real LiveKit library: it cycles Initialize (which creates a real
// LiveKit client handle), CreateAudioSink and Stop without connecting. Sinks reject PCM while
// disconnected, but every submit still enters the gate and races Stop(), which destroys the
// client. WP-S7 added the FLkFfiApi seam (ADR 0006 F2, see WebRTCFunctionalTests.cpp); a
// variant that publishes into live fake tracks during Stop() is left to WP-T2.

#if WITH_DEV_AUTOMATION_TESTS

#include "../Sender/WebRTCSender.h"
#include "Testing/O3DLifetimeTestUtils.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCLifetimeStressTest, "Open3DBroadcast.Transport.WebRTC.Lifetime.InitStopWithAudio", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCLifetimeStressTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	const O3DLifetimeTest::FStressResult Result = O3DLifetimeTest::RunSenderStress<FO3DWebRTCSender>([](int32)
	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("ws://127.0.0.1:7880"); // never contacted: Start() is not called
		Config.Token = TEXT("wp-s5-test-token");
		Config.StreamId = TEXT("LifetimeStream");
		Config.Audio.bEnableAudio = true;
		Config.Audio.BitrateKbps = 24;
		Config.Audio.NumChannels = 1;
		Config.Audio.SampleRate = 48000;
		return Config;
	}, /*bStart=*/false);

	TestEqual(TEXT("All cycles ran"), Result.CyclesRun, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("Every cycle initialized"), Result.StartFailures, 0);
	TestEqual(TEXT("Every cycle produced a sink"), Result.SinksCreated, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("No sink accepts PCM after its sender stopped"), Result.StaleSinkAccepted, 0);
	AddInfo(FString::Printf(TEXT("Fake audio thread submitted %lld buffers, %lld accepted"), Result.Submitted, Result.Accepted));
#else
	AddInfo(TEXT("WebRTC transport is Win64-only; lifetime stress skipped."));
#endif
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_WEBRTC
