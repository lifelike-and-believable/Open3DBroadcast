// Copyright (c) Open3DStream Contributors
//
// WP-S5 tests for MoQ (TRF-1, TRF-10, TRF-12).
//
// InitStopWithAudio cycles Initialize/CreateAudioSink/Stop without Start(). WP-S8 adds
// Open3DBroadcast.Transport.MoQ.Lifetime.StartStopWithAudio (MoQFunctionalTests.cpp), which runs
// Start() through the fake FFI table so the publish worker is covered too.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "Sender/MoQSender.h"
#include "Shared/MoQAsyncDispatcher.h"
#include "Shared/MoQSessionWrapper.h"
#include "Testing/O3DLifetimeTestUtils.h"
#include "Tests/MoQFakeFfi.h"

#include <atomic>

#if O3D_WITH_TRANSPORT_MOQ

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQLifetimeStressTest, "Open3DBroadcast.Transport.MoQ.Lifetime.InitStopWithAudio", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQLifetimeStressTest::RunTest(const FString& Parameters)
{
	const O3DLifetimeTest::FStressResult Result = O3DLifetimeTest::RunSenderStress<FO3DMoQSender>([](int32)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.Uri = TEXT("https://127.0.0.1:4443"); // never contacted: Start() is not called
		Config.StreamId = TEXT("wp_s5/lifetime");
		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 1;
		return Config;
	}, /*bStart=*/false);

	TestEqual(TEXT("All cycles ran"), Result.CyclesRun, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("Every cycle initialized"), Result.StartFailures, 0);
	TestEqual(TEXT("Every cycle produced a sink"), Result.SinksCreated, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("No sink accepts PCM after its sender stopped"), Result.StaleSinkAccepted, 0);
	AddInfo(FString::Printf(TEXT("Fake audio thread submitted %lld buffers, %lld accepted"), Result.Submitted, Result.Accepted));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQStaleCallbackTokenTest, "Open3DBroadcast.Transport.MoQ.Lifetime.StaleFfiCallbackTokens", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQStaleCallbackTokenTest::RunTest(const FString& Parameters)
{
	FMoQAsyncDispatcher::Get().Initialize();

	// TRF-12: a subscriber callback whose binding is gone must do nothing. Unknown tokens
	// (including what used to be a raw binding address) resolve to no binding.
	TArray64<uint8> Payload;
	Payload.Add(0x42);
	FMoQSessionWrapperTestHelper::InvokeSubscriberThunkWithToken(reinterpret_cast<void*>(static_cast<UPTRINT>(0xDEADBEEF)), Payload);
	FMoQSessionWrapperTestHelper::InvokeSubscriberThunkWithToken(nullptr, Payload);

	// A connection callback that arrives after the wrapper is destroyed must do nothing.
	// WP-S8: tokens are per connect attempt, so the session connects through the fake FFI
	// (the connect is held, as if it never returned) to get one.
	TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	Fake->bHoldBlockingWork = true;
	std::atomic<int32> DelegateCalls{0};
	void* StaleToken = nullptr;
	{
		TSharedRef<FMoQSessionWrapper, ESPMode::ThreadSafe> Session = MakeShared<FMoQSessionWrapper, ESPMode::ThreadSafe>(Fake->MakeApi());
		TestTrue(TEXT("Initialize should succeed"), Session->Initialize(TEXT("https://127.0.0.1:4443")).IsOk());
		Session->OnConnectionStateChanged().AddLambda([&DelegateCalls](MoqConnectionState) { DelegateCalls.fetch_add(1); });
		TestTrue(TEXT("Connect should start"), Session->Connect().IsOk());
		StaleToken = FMoQSessionWrapperTestHelper::GetConnectionToken(*Session);
		TestNotNull(TEXT("Session registered a connection token"), StaleToken);
	}

	FMoQSessionWrapperTestHelper::InvokeConnectionThunkWithToken(StaleToken, MOQ_STATE_CONNECTED);
	// The held connect finally returns after its session is gone: it must not report anything.
	Fake->RunHeldWork();
	MoQFakeTest::Pump();
	TestEqual(TEXT("Late connection callback reached no delegate"), DelegateCalls.load(), 0);
	TestTrue(TEXT("The abandoned client was destroyed"), Fake->IsClientDestroyed(1));
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
