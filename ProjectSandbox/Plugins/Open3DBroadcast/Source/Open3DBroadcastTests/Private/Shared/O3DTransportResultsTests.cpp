// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 3 (ADR 0007 items 3 and 4; SHR-14): results, connection state and capabilities.
// - FO3DTransportResult and the LexToString helpers.
// - FO3DConnectionStateTracker: Begin, Set and End, the callback thread, ordering, and nothing
//   reported after End.
// - The registry's capability query, and the values every built-in transport reports (ADR 0005
//   (iii) delivery guarantee, audio, control, payload limit), NNG per mode.
// - Result codes per failure case on the built-in transports: Start before Initialize is
//   NotRunning, an unusable config is InvalidConfig, a TCP sender without a receiver is
//   NotConnected.
// The conformance suite (Open3DBroadcast.Conformance.*) checks the rest on every transport:
// NoConsumer, Invalid, DroppedBackpressure, capability consistency and Start/Stop transitions.
// Built-in transports are reached through the registry and skipped when their module is not
// loaded. No network beyond 127.0.0.1.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeLock.h"
#include "O3DTestFakes.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DTransportRegistry.h"

#include <atomic>

namespace O3DTransportResultsTests
{
	/** Thread-safe record of tracker callbacks. */
	struct FRecorder
	{
		struct FEntry
		{
			EO3DConnectionState State = EO3DConnectionState::Idle;
			EO3DTransportError Code = EO3DTransportError::None;
			bool bOnGameThread = false;
		};

		FO3DConnectionStateCallback MakeCallback()
		{
			return [this](EO3DConnectionState State, const FO3DTransportResult& Reason)
			{
				FScopeLock Lock(&Mutex);
				FEntry Entry;
				Entry.State = State;
				Entry.Code = Reason.Code;
				Entry.bOnGameThread = IsInGameThread();
				Entries.Add(Entry);
			};
		}

		TArray<FEntry> Get() const
		{
			FScopeLock Lock(&Mutex);
			return Entries;
		}

		mutable FCriticalSection Mutex;
		TArray<FEntry> Entries;
	};

	FO3DTransportCapabilities MakeCaps(EO3DDeliveryGuarantee Delivery, bool bBidirectional, int32 MaxPayloadBytes)
	{
		FO3DTransportCapabilities Caps;
		Caps.bSend = true;
		Caps.bReceive = true;
		Caps.bAudioSend = true;
		Caps.bAudioReceive = true;
		Caps.bControl = true;
		Caps.bBidirectional = bBidirectional;
		Caps.Delivery = Delivery;
		Caps.MaxPayloadBytes = MaxPayloadBytes;
		return Caps;
	}

	FString Describe(const FO3DTransportCapabilities& Caps)
	{
		return FString::Printf(TEXT("[audio=%d/%d control=%d bidir=%d peerJoin=%d delivery=%s max=%d]"),
			Caps.bAudioSend ? 1 : 0, Caps.bAudioReceive ? 1 : 0, Caps.bControl ? 1 : 0, Caps.bBidirectional ? 1 : 0,
			Caps.bPeerJoinSignal ? 1 : 0, LexToString(Caps.Delivery), Caps.MaxPayloadBytes);
	}

	/** Checks the registered descriptor's answer for Config, if Name is registered. */
	void ExpectRegisteredCaps(FAutomationTestBase& Test, const TCHAR* Name, const FO3DTransportConfig& Config, const FO3DTransportCapabilities& Expected, const TCHAR* What)
	{
		FO3DTransportCapabilities Actual;
		if (!FO3DTransportRegistry::Get().GetCapabilities(FName(Name), Config, Actual))
		{
			Test.AddInfo(FString::Printf(TEXT("%s is not registered in this build; skipped."), Name));
			return;
		}
		Test.TestTrue(*FString::Printf(TEXT("%s %s: %s (expected %s)"), Name, What, *Describe(Actual), *Describe(Expected)), Actual == Expected);
	}

	FO3DTransportConfig MakeNngConfig(const TCHAR* Mode)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("nng");
		Config.AdvancedParams.Add(TEXT("nng.mode"), Mode);
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), TEXT("1"));
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportResultBasicsTest, "Open3DBroadcast.Shared.TransportResult.OkErrorAndText", O3DB_TEST_FLAGS)
bool FO3DTransportResultBasicsTest::RunTest(const FString& Parameters)
{
	const FO3DTransportResult Ok = FO3DTransportResult::Ok();
	TestTrue(TEXT("Ok is Ok"), Ok.IsOk());
	TestTrue(TEXT("Ok converts to true"), static_cast<bool>(Ok));
	TestTrue(TEXT("Ok has code None"), Ok.Code == EO3DTransportError::None);
	TestEqual(TEXT("Ok prints as Ok"), LexToString(Ok), FString(TEXT("Ok")));

	const FO3DTransportResult Error = FO3DTransportResult::Error(EO3DTransportError::NoConsumer, TEXT("no consumer"));
	TestFalse(TEXT("An error is not Ok"), Error.IsOk());
	TestFalse(TEXT("An error converts to false"), static_cast<bool>(Error));
	TestTrue(TEXT("`if (!Result)` works"), !Error);
	TestEqual(TEXT("An error prints its code and message"), LexToString(Error), FString(TEXT("NoConsumer: no consumer")));
	TestEqual(TEXT("Without a message only the code"), LexToString(FO3DTransportResult::Error(EO3DTransportError::Timeout)), FString(TEXT("Timeout")));

	TestEqual(TEXT("Send result text"), FString(LexToString(EO3DSendResult::DroppedBackpressure)), FString(TEXT("DroppedBackpressure")));
	TestEqual(TEXT("State text"), FString(LexToString(EO3DConnectionState::Reconnecting)), FString(TEXT("Reconnecting")));
	TestEqual(TEXT("Delivery text"), FString(LexToString(EO3DDeliveryGuarantee::ReliableOrdered)), FString(TEXT("ReliableOrdered")));
	TestTrue(TEXT("Only Queued is accepted"), O3DTransport::IsAccepted(EO3DSendResult::Queued) && !O3DTransport::IsAccepted(EO3DSendResult::NotConnected));

	const uint8 Bytes[3] = { 7, 8, 9 };
	const FO3DSendPayload Payload = FO3DSendPayload::MakeCopy(Bytes, 3, TEXT("Hero"), 1.5, true);
	TestEqual(TEXT("MakeCopy copies the bytes"), Payload.Bytes.Num(), 3);
	TestTrue(TEXT("MakeCopy keeps subject, time and full-sync flag"), Payload.Subject == TEXT("Hero") && Payload.CaptureTimeSec == 1.5 && Payload.bFullSync);
	TestEqual(TEXT("MakeCopy of null is empty"), FO3DSendPayload::MakeCopy(nullptr, 5).Bytes.Num(), 0);

	FO3DTransportStats Stats;
	Stats.State = EO3DConnectionState::Connected;
	Stats.PendingBytes = 10;
	Stats.Reset();
	TestTrue(TEXT("Stats.Reset clears the state and the new counters"), Stats.State == EO3DConnectionState::Idle && Stats.PendingBytes == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DConnectionStateTrackerTest, "Open3DBroadcast.Shared.ConnectionState.TrackerRules", O3DB_TEST_FLAGS)
bool FO3DConnectionStateTrackerTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportResultsTests;
	FRecorder Recorder;
	FO3DConnectionStateTracker Tracker;
	Tracker.SetCallback(Recorder.MakeCallback());

	TestTrue(TEXT("Idle at first"), Tracker.Get() == EO3DConnectionState::Idle);
	TestFalse(TEXT("Set outside a session is ignored"), Tracker.Set(EO3DConnectionState::Connected));
	TestTrue(TEXT("Still Idle"), Tracker.Get() == EO3DConnectionState::Idle);

	Tracker.Begin(EO3DConnectionState::Connecting);
	TestTrue(TEXT("Open after Begin"), Tracker.IsOpen());
	TestFalse(TEXT("The same state again is not a change"), Tracker.Set(EO3DConnectionState::Connecting));
	TestTrue(TEXT("A new state is a change"), Tracker.Set(EO3DConnectionState::Connected));
	Tracker.Set(EO3DConnectionState::Reconnecting, FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, TEXT("lost")));
	Tracker.End(EO3DConnectionState::Idle);
	TestFalse(TEXT("Closed after End"), Tracker.IsOpen());
	TestFalse(TEXT("A late Set after End is ignored"), Tracker.Set(EO3DConnectionState::Connected));
	Tracker.End(EO3DConnectionState::Idle);

	const TArray<FRecorder::FEntry> Entries = Recorder.Get();
	if (TestEqual(TEXT("Four changes reported"), Entries.Num(), 4))
	{
		TestTrue(TEXT("In order"), Entries[0].State == EO3DConnectionState::Connecting && Entries[1].State == EO3DConnectionState::Connected
			&& Entries[2].State == EO3DConnectionState::Reconnecting && Entries[3].State == EO3DConnectionState::Idle);
		TestTrue(TEXT("The reason travels with the change"), Entries[2].Code == EO3DTransportError::ConnectFailed && Entries[1].Code == EO3DTransportError::None);
		TestTrue(TEXT("Changes made on the game thread are reported there"), Entries[0].bOnGameThread && Entries[3].bOnGameThread);
	}

	// A failed start ends the session in Failed and reports it.
	Tracker.End(EO3DConnectionState::Failed, FO3DTransportResult::Error(EO3DTransportError::AddressInUse));
	TestTrue(TEXT("Failed"), Tracker.Get() == EO3DConnectionState::Failed);
	TestTrue(TEXT("Failed was reported with its reason"), Recorder.Get().Last().Code == EO3DTransportError::AddressInUse);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DConnectionStateThreadTest, "Open3DBroadcast.Shared.ConnectionState.CallbackRunsOnTheChangingThread", O3DB_TEST_FLAGS)
bool FO3DConnectionStateThreadTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportResultsTests;
	// The fake sender's state, moved the way a transport's worker or FFI thread would.
	const TSharedRef<FO3DFakeSender> Sender = MakeShared<FO3DFakeSender>();
	FRecorder Recorder;
	Sender->SetStateChangedCallback(Recorder.MakeCallback());
	TestTrue(TEXT("Initialize"), Sender->Initialize(FO3DTransportConfig()).IsOk());
	TestTrue(TEXT("Start"), Sender->Start().IsOk());

	const TWeakPtr<FO3DFakeSender> Weak = Sender;
	FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady([Weak]()
	{
		if (const TSharedPtr<FO3DFakeSender> Pinned = Weak.Pin())
		{
			Pinned->SimulateConnectionState(EO3DConnectionState::Reconnecting, FO3DTransportResult::Error(EO3DTransportError::Timeout));
		}
	}, TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask);
	FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task, ENamedThreads::GameThread);
	TestTrue(TEXT("The background change is visible at once"), Sender->GetConnectionState() == EO3DConnectionState::Reconnecting);
	TestTrue(TEXT("Stats.State follows"), Sender->GetStats().State == EO3DConnectionState::Reconnecting);

	Sender->Stop();
	const TArray<FRecorder::FEntry> Entries = Recorder.Get();
	if (TestEqual(TEXT("Start, the background change and Stop were reported"), Entries.Num(), 3))
	{
		TestTrue(TEXT("Start reported Connected on the game thread"), Entries[0].State == EO3DConnectionState::Connected && Entries[0].bOnGameThread);
		TestTrue(TEXT("The background change ran on the background thread"), Entries[1].State == EO3DConnectionState::Reconnecting && !Entries[1].bOnGameThread);
		TestTrue(TEXT("Stop reported Idle on the game thread"), Entries[2].State == EO3DConnectionState::Idle && Entries[2].bOnGameThread);
	}

	// After Stop the transport's threads cannot move the state any more.
	Sender->SimulateConnectionState(EO3DConnectionState::Connected);
	TestTrue(TEXT("A change after Stop is ignored"), Sender->GetConnectionState() == EO3DConnectionState::Idle);
	TestEqual(TEXT("And not reported"), Recorder.Get().Num(), 3);
	Sender->SetStateChangedCallback(nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportCapabilitiesRegistryTest, "Open3DBroadcast.Shared.TransportCapabilities.RegistryQuery", O3DB_TEST_FLAGS)
bool FO3DTransportCapabilitiesRegistryTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FO3DTransportRegistry, ESPMode::ThreadSafe> Registry = MakeShared<FO3DTransportRegistry, ESPMode::ThreadSafe>();
	FO3DTransportCapabilities Caps;
	Caps.bControl = true;
	TestFalse(TEXT("An unknown name has no capabilities"), Registry->GetCapabilities(TEXT("O3DCapsMissing"), FO3DTransportConfig(), Caps));
	TestTrue(TEXT("and the output is reset"), Caps == FO3DTransportCapabilities());

	// The query sees the config, and bSend/bReceive always follow the factories.
	FO3DTransportDescriptor Descriptor;
	Descriptor.Name = TEXT("O3DCapsSenderOnly");
	Descriptor.CreateSender = []() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeSender>(); };
	Descriptor.GetCapabilities = [](const FO3DTransportConfig& Config)
	{
		FO3DTransportCapabilities Result;
		Result.bReceive = true; // wrong on purpose: there is no receiver factory
		Result.Delivery = Config.AdvancedParams.Contains(TEXT("lossy")) ? EO3DDeliveryGuarantee::Unreliable : EO3DDeliveryGuarantee::ReliableOrdered;
		return Result;
	};
	FO3DTransportRegistration Registration = Registry->Register(MoveTemp(Descriptor));

	FO3DTransportConfig Lossy;
	Lossy.AdvancedParams.Add(TEXT("lossy"), TEXT("1"));
	TestTrue(TEXT("Found"), Registry->GetCapabilities(TEXT("O3DCapsSenderOnly"), Lossy, Caps));
	TestTrue(TEXT("The config reaches the query"), Caps.Delivery == EO3DDeliveryGuarantee::Unreliable);
	TestTrue(TEXT("bSend follows the sender factory"), Caps.bSend);
	TestFalse(TEXT("bReceive follows the (missing) receiver factory"), Caps.bReceive);
	Registry->GetCapabilities(TEXT("O3DCapsSenderOnly"), FO3DTransportConfig(), Caps);
	TestTrue(TEXT("Another config, another answer"), Caps.Delivery == EO3DDeliveryGuarantee::ReliableOrdered);

	// A descriptor without the function (a transport registered through the deprecated shims).
	FO3DTransportDescriptor Plain;
	Plain.Name = TEXT("O3DCapsPlain");
	Plain.CreateReceiver = []() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeReceiver>(); };
	FO3DTransportRegistration PlainRegistration = Registry->Register(MoveTemp(Plain));
	TestTrue(TEXT("Found"), Registry->GetCapabilities(TEXT("O3DCapsPlain"), FO3DTransportConfig(), Caps));
	TestTrue(TEXT("Delivery Unknown without a query"), Caps.Delivery == EO3DDeliveryGuarantee::Unknown);
	TestTrue(TEXT("Only the roles are known"), Caps.bReceive && !Caps.bSend && !Caps.bControl);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportCapabilitiesBuiltInTest, "Open3DBroadcast.Shared.TransportCapabilities.BuiltInValues", O3DB_TEST_FLAGS)
bool FO3DTransportCapabilitiesBuiltInTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportResultsTests;
	const FO3DTransportConfig Empty;
	// ADR 0005 (iii): Loopback and TCP ReliableOrdered, UDP Unreliable, MoQ Unreliable in both
	// delivery modes (Q5), NNG by mode. ADR 0011: every built-in transport carries control.
	ExpectRegisteredCaps(*this, TEXT("Loopback"), Empty, MakeCaps(EO3DDeliveryGuarantee::ReliableOrdered, false, 0), TEXT("default"));
	ExpectRegisteredCaps(*this, TEXT("TCP"), Empty, MakeCaps(EO3DDeliveryGuarantee::ReliableOrdered, true, 50 * 1024 * 1024), TEXT("default"));
	ExpectRegisteredCaps(*this, TEXT("UDP"), Empty, MakeCaps(EO3DDeliveryGuarantee::Unreliable, false, 0), TEXT("default"));

	FO3DTransportConfig Datagram;
	Datagram.AdvancedParams.Add(TEXT("delivery_mode"), TEXT("datagram"));
	ExpectRegisteredCaps(*this, TEXT("MoQ"), Empty, MakeCaps(EO3DDeliveryGuarantee::Unreliable, false, 0), TEXT("stream mode"));
	ExpectRegisteredCaps(*this, TEXT("MoQ"), Datagram, MakeCaps(EO3DDeliveryGuarantee::Unreliable, false, 0), TEXT("datagram mode"));

	ExpectRegisteredCaps(*this, TEXT("NNG"), MakeNngConfig(TEXT("pub")), MakeCaps(EO3DDeliveryGuarantee::Unreliable, false, 0), TEXT("pub"));
	ExpectRegisteredCaps(*this, TEXT("NNG"), MakeNngConfig(TEXT("sub")), MakeCaps(EO3DDeliveryGuarantee::Unreliable, false, 0), TEXT("sub"));
	ExpectRegisteredCaps(*this, TEXT("NNG"), MakeNngConfig(TEXT("pair")), MakeCaps(EO3DDeliveryGuarantee::ReliableOrdered, true, 0), TEXT("pair"));
	ExpectRegisteredCaps(*this, TEXT("NNG"), MakeNngConfig(TEXT("push")), MakeCaps(EO3DDeliveryGuarantee::ReliableOrdered, false, 0), TEXT("push"));
	ExpectRegisteredCaps(*this, TEXT("NNG"), MakeNngConfig(TEXT("pull")), MakeCaps(EO3DDeliveryGuarantee::ReliableOrdered, false, 0), TEXT("pull"));

	// An NNG instance reports the mode it was initialized with.
	if (const TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender = FO3DTransportRegistry::Get().CreateSender(TEXT("NNG")))
	{
		FO3DTransportConfig Pair = MakeNngConfig(TEXT("pair"));
		Pair.AdvancedParams.Add(TEXT("port"), FString::FromInt(O3DTests::FindFreeLoopbackPort(/*bTcp=*/true)));
		TestTrue(TEXT("NNG pair sender initializes"), Sender->Initialize(Pair).IsOk());
		TestTrue(TEXT("The NNG pair sender is ReliableOrdered"), Sender->GetCapabilities().Delivery == EO3DDeliveryGuarantee::ReliableOrdered);
		TestTrue(TEXT("The NNG pair sender is bidirectional"), Sender->GetCapabilities().bBidirectional);
		Sender->Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportResultCodesTest, "Open3DBroadcast.Transport.Results.FailureCasesHaveCodes", O3DB_TEST_FLAGS)
bool FO3DTransportResultCodesTest::RunTest(const FString& Parameters)
{
	FO3DTransportRegistry& Registry = FO3DTransportRegistry::Get();
	auto IsBuiltIn = [&Registry](const TCHAR* Name) { return Registry.IsRegistered(FName(Name), EO3DTransportRole::Sender); };

	// The transports log these paths (warnings, MoQ's config error); expected once or more each.
	if (IsBuiltIn(TEXT("TCP")))
	{
		AddExpectedError(TEXT("cannot start: not initialized"), EAutomationExpectedMessageFlags::Contains, 0);
		AddExpectedError(TEXT("requires tcp://host:port"), EAutomationExpectedMessageFlags::Contains, 0);
	}
	if (IsBuiltIn(TEXT("UDP")))
	{
		AddExpectedError(TEXT("requires udp://host:port"), EAutomationExpectedMessageFlags::Contains, 0);
	}
	if (IsBuiltIn(TEXT("NNG")) || IsBuiltIn(TEXT("MoQ")))
	{
		AddExpectedError(TEXT("Start called before Initialize"), EAutomationExpectedMessageFlags::Contains, 0);
	}
	if (IsBuiltIn(TEXT("NNG")))
	{
		AddExpectedError(TEXT("Failed to parse NNG sender config"), EAutomationExpectedMessageFlags::Contains, 0);
	}
	if (IsBuiltIn(TEXT("MoQ")))
	{
		AddExpectedError(TEXT("configuration invalid"), EAutomationExpectedMessageFlags::Contains, 0);
	}

	// Start before Initialize: NotRunning, for every built-in sender and receiver.
	for (const TCHAR* Name : { TEXT("Loopback"), TEXT("TCP"), TEXT("UDP"), TEXT("NNG"), TEXT("MoQ") })
	{
		if (!Registry.IsRegistered(FName(Name), EO3DTransportRole::Sender))
		{
			AddInfo(FString::Printf(TEXT("%s is not registered in this build; skipped."), Name));
			continue;
		}
		const TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender = Registry.CreateSender(FName(Name));
		const TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Receiver = Registry.CreateReceiver(FName(Name));
		if (!Sender.IsValid() || !Receiver.IsValid())
		{
			AddInfo(FString::Printf(TEXT("%s did not create an instance in this build; skipped."), Name));
			continue;
		}
		const FO3DTransportResult SenderResult = Sender->Start();
		TestTrue(*FString::Printf(TEXT("%s sender Start before Initialize is NotRunning (got %s)"), Name, *LexToString(SenderResult)), SenderResult.Code == EO3DTransportError::NotRunning);
		TestTrue(*FString::Printf(TEXT("%s sender stays Idle"), Name), Sender->GetConnectionState() == EO3DConnectionState::Idle);
		Sender->Stop();

		Receiver->SetConsumer(MakeShared<FO3DRecordingFrameConsumer>());
		const FO3DTransportResult ReceiverResult = Receiver->Start();
		TestTrue(*FString::Printf(TEXT("%s receiver Start before Initialize is NotRunning (got %s)"), Name, *LexToString(ReceiverResult)), ReceiverResult.Code == EO3DTransportError::NotRunning);
		Receiver->Stop();
	}

	// A config the transport cannot use: InvalidConfig, with a message.
	auto ExpectInvalidConfig = [this, &Registry](const TCHAR* Name, const FO3DTransportConfig& Config, bool bSender)
	{
		if (!Registry.IsRegistered(FName(Name), bSender ? EO3DTransportRole::Sender : EO3DTransportRole::Receiver))
		{
			return;
		}
		FO3DTransportResult Result;
		if (bSender)
		{
			const TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender = Registry.CreateSender(FName(Name));
			if (!Sender.IsValid())
			{
				return;
			}
			Result = Sender->Initialize(Config);
		}
		else
		{
			const TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Receiver = Registry.CreateReceiver(FName(Name));
			if (!Receiver.IsValid())
			{
				return;
			}
			Result = Receiver->Initialize(Config);
		}
		TestTrue(*FString::Printf(TEXT("%s %s: InvalidConfig (got %s)"), Name, bSender ? TEXT("sender") : TEXT("receiver"), *LexToString(Result)),
			Result.Code == EO3DTransportError::InvalidConfig && !Result.Message.IsEmpty());
	};
	const FO3DTransportConfig NoEndpoint;
	ExpectInvalidConfig(TEXT("TCP"), NoEndpoint, true);
	ExpectInvalidConfig(TEXT("TCP"), NoEndpoint, false);
	ExpectInvalidConfig(TEXT("UDP"), NoEndpoint, true);
	ExpectInvalidConfig(TEXT("UDP"), NoEndpoint, false);
	ExpectInvalidConfig(TEXT("MoQ"), NoEndpoint, true);
	ExpectInvalidConfig(TEXT("MoQ"), NoEndpoint, false);
	FO3DTransportConfig NngSubSender;
	NngSubSender.AdvancedParams.Add(TEXT("nng.mode"), TEXT("sub"));
	ExpectInvalidConfig(TEXT("NNG"), NngSubSender, true); // a sender cannot subscribe

	// A TCP sender with no receiver connected: NotConnected for frames and control.
	if (Registry.IsRegistered(TEXT("TCP"), EO3DTransportRole::Sender))
	{
		const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
		FO3DTransportConfig Config;
		Config.Transport = TEXT("sockets.tcp");
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(TEXT("bind"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		const TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender = Registry.CreateSender(TEXT("TCP"));
		if (Sender.IsValid() && TestTrue(TEXT("TCP sender initializes"), Sender->Initialize(Config).IsOk()) && TestTrue(TEXT("TCP sender starts"), Sender->Start().IsOk()))
		{
			TestTrue(TEXT("TCP sender is Connecting (listening, no receiver)"), Sender->GetConnectionState() == EO3DConnectionState::Connecting);
			const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("ResultActor"), 1);
			TestTrue(TEXT("A frame without a receiver is NotConnected"),
				Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frames[0].GetData(), Frames[0].Num(), TEXT("ResultActor"), 0.0)) == EO3DSendResult::NotConnected);
			TArray<uint8> Envelope;
			const TArray<uint8> ControlPayload = { 1, 2, 3 };
			O3DS::WriteControlEnvelope(ControlPayload, 1.0, Envelope);
			TestTrue(TEXT("Control without a receiver is NotConnected"), Sender->SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::NotConnected);
			TestEqual(TEXT("NotConnected is not a dropped frame"), Sender->GetStats().DroppedFrames, static_cast<int64>(0));
		}
		if (Sender.IsValid())
		{
			Sender->Stop();
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
