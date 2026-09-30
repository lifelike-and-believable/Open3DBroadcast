// Copyright Lifelike & Believable. All Rights Reserved.

// MoQ session wrapper and dispatcher unit tests, through the FMoQTestSession façade in
// Testing/MoQTesting.h (WP-T2). The wrapper itself stays private to the MoQ module.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#if O3D_WITH_TRANSPORT_MOQ

#include "Testing/MoQTesting.h"
#include "Transport/MoQ/MoQFakeFfi.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSessionInvalidUrlTest, "Open3DBroadcast.Transport.MoQ.Session.InvalidUrl", O3DB_TEST_FLAGS)
bool FMoQSessionInvalidUrlTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	FMoQTestSession Session(Fake->MakeApi());
	const FMoQTestResult Result = Session.Initialize(TEXT("   \t"));
	TestEqual(TEXT("Empty URL should be rejected"), Result.Code, FString(TEXT("InvalidArgument")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSessionCreatePublisherWithoutConnectionTest, "Open3DBroadcast.Transport.MoQ.Session.CreatePublisherWithoutConnection", O3DB_TEST_FLAGS)
bool FMoQSessionCreatePublisherWithoutConnectionTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	FMoQTestSession Session(Fake->MakeApi());
	TestTrue(TEXT("Initialize should succeed"), Session.Initialize(TEXT("https://localhost:4443")).IsOk());

	bool bCreated = true;
	const FMoQTestResult Result = Session.CreatePublisher(TEXT("mocap/test"), TEXT("character"), MOQ_DELIVERY_STREAM, bCreated);
	TestEqual(TEXT("Expected not-connected error"), Result.Code, FString(TEXT("NotConnected")));
	TestFalse(TEXT("Publisher should not be created"), bCreated);
	TestEqual(TEXT("moq-ffi was not asked for a publisher"), Fake->GetPublishersCreated(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSubscriptionRequiresCallbackTest, "Open3DBroadcast.Transport.MoQ.Session.SubscribeRequiresCallback", O3DB_TEST_FLAGS)
bool FMoQSubscriptionRequiresCallbackTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	FMoQTestSession Session(Fake->MakeApi());
	TestTrue(TEXT("Initialize should succeed"), Session.Initialize(TEXT("https://localhost:4443")).IsOk());

	bool bCreated = true;
	const FMoQTestResult Result = Session.Subscribe(TEXT("mocap/test"), TEXT("character"), nullptr, bCreated);
	TestEqual(TEXT("Missing callback should return invalid argument"), Result.Code, FString(TEXT("InvalidArgument")));
	TestFalse(TEXT("Subscriber handle should be invalid"), bCreated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQDispatcherRunsOnGameThreadTest, "Open3DBroadcast.Transport.MoQ.Dispatcher.GameThread", O3DB_TEST_FLAGS)
bool FMoQDispatcherRunsOnGameThreadTest::RunTest(const FString& Parameters)
{
	std::atomic<bool> bCompleted{false};
	std::atomic<bool> bOnGameThread{false};

	MoQTesting::InitializeDispatcher();
	MoQTesting::EnqueueOnDispatcher([&bCompleted, &bOnGameThread]()
	{
		bOnGameThread = IsInGameThread();
		bCompleted = true;
	});

	MoQTesting::PumpDispatcher();

	TestTrue(TEXT("Dispatcher should complete queued work"), bCompleted.load());
	TestTrue(TEXT("Work should execute on game thread"), bOnGameThread.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSessionConnectionDispatchTest, "Open3DBroadcast.Transport.MoQ.Session.ConnectionDispatch", O3DB_TEST_FLAGS)
bool FMoQSessionConnectionDispatchTest::RunTest(const FString& Parameters)
{
	MoQTesting::InitializeDispatcher();

	// Declared before the session so they outlive its handler.
	std::atomic<bool> bDelegateCalled{false};
	std::atomic<bool> bDelegateOnGameThread{false};

	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	FMoQTestSession Session(Fake->MakeApi());
	TestTrue(TEXT("Initialize should succeed"), Session.Initialize(TEXT("https://localhost:4443")).IsOk());
	Session.AddConnectionStateHandler([&bDelegateCalled, &bDelegateOnGameThread](MoqConnectionState State)
	{
		if (State == MOQ_STATE_CONNECTED)
		{
			bDelegateCalled = true;
			bDelegateOnGameThread = IsInGameThread();
		}
	});

	Session.InvokeConnectionState(MOQ_STATE_CONNECTED);
	MoQTesting::PumpDispatcher();

	TestTrue(TEXT("Connection delegate should be invoked"), bDelegateCalled.load());
	TestTrue(TEXT("Connection delegate should execute on the game thread"), bDelegateOnGameThread.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSessionSubscriberDispatchTest, "Open3DBroadcast.Transport.MoQ.Session.SubscriberDispatch", O3DB_TEST_FLAGS)
bool FMoQSessionSubscriberDispatchTest::RunTest(const FString& Parameters)
{
	MoQTesting::InitializeDispatcher();

	std::atomic<bool> bPayloadReceived{false};
	std::atomic<bool> bOnGameThread{false};
	std::atomic<int64> PayloadSize{0};

	TFunction<void(const TArray64<uint8>&)> Handler = [this, &bPayloadReceived, &bOnGameThread, &PayloadSize](const TArray64<uint8>& Payload)
	{
		bOnGameThread = IsInGameThread();
		PayloadSize = Payload.Num();
		bPayloadReceived = !Payload.IsEmpty();
		if (Payload.IsEmpty())
		{
			AddError(TEXT("Payload should not be empty in positive-path dispatcher test"));
		}
	};

	TArray64<uint8> Payload;
	Payload.Add(0x10);
	Payload.Add(0x20);
	Payload.Add(0x30);

	MoQTesting::InvokeSubscriberCallback(Handler, Payload);
	MoQTesting::PumpDispatcher();

	TestTrue(TEXT("Subscriber callback should be invoked"), bPayloadReceived.load());
	TestTrue(TEXT("Subscriber callback should execute on the game thread"), bOnGameThread.load());
	TestEqual(TEXT("Payload size should round-trip"), PayloadSize.load(), static_cast<int64>(Payload.Num()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQResultFromCodeFallbackTest, "Open3DBroadcast.Transport.MoQ.Result.Fallback", O3DB_TEST_FLAGS)
bool FMoQResultFromCodeFallbackTest::RunTest(const FString& Parameters)
{
	const FMoQTestResult Result = MoQTesting::MakeResultFromRawCode(MOQ_ERROR_TIMEOUT, FString());
	TestTrue(TEXT("Fallback message should not be empty"), !Result.Message.IsEmpty());
	TestEqual(TEXT("Timeout should map correctly"), Result.Code, FString(TEXT("Timeout")));
	TestFalse(TEXT("A timeout is not OK"), Result.IsOk());
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
