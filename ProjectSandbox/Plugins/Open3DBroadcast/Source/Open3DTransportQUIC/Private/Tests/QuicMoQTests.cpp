#if WITH_DEV_AUTOMATION_TESTS

#include "MoQ/MoQProtocol.h"
#include "MoQ/MoQTrackManager.h"
#include "Sender/QuicSender.h"
#include "Shared/QuicHelpers.h"

#include "O3DTransportTypes.h"

#include "Misc/AutomationTest.h"

using namespace O3DMoQ;

#if WITH_DEV_AUTOMATION_TESTS
class FMockQuicRelay : public O3DQuic::IQuicPublisherRelay
{
public:
	int32 FanoutReturn = 1;
	int32 LastPayloadBytes = 0;
	int32 AnnounceCount = 0;

	virtual bool Initialize(const O3DQuic::FQuicSenderOptions& Options, FString& OutError) override
	{
		(void)Options;
		OutError.Reset();
		return true;
	}

	virtual void Shutdown() override {}

	virtual void UpdateTrackMetadata(O3DMoQ::FMoQTrackId TrackId, const TArray<uint8>& AnnouncePayload, const TArray<uint8>& UnannouncePayload) override
	{
		(void)TrackId;
		(void)AnnouncePayload;
		(void)UnannouncePayload;
	}

	virtual bool SendAnnounce(const TArray<uint8>& Payload) override
	{
		AnnounceCount++;
		return Payload.Num() > 0;
	}

	virtual bool SendUnannounce(const TArray<uint8>& Payload) override
	{
		return Payload.Num() > 0;
	}

	virtual int32 FanoutObject(const TArray<uint8>& Payload, FString& OutError) override
	{
		LastPayloadBytes = Payload.Num();
		if (FanoutReturn <= 0)
		{
			OutError = TEXT("Relay rejected payload");
		}
		else
		{
			OutError.Reset();
		}
		return FanoutReturn;
	}

	virtual bool HasActiveSubscribers() const override
	{
		return true;
	}
};
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQAnnounceSerializationTest, "Open3DBroadcast.Open3DTransportQUIC.MoQ.Protocol.Announce", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQAnnounceSerializationTest::RunTest(const FString& Parameters)
{
	FMoQAnnounceMessage Message;
	Message.TrackId = 42;
	Message.TrackName = TEXT("mocap/session1/character1");
	Message.Properties.Priority = 192;
	Message.Properties.bDatagramsAllowed = true;
	Message.Properties.bIsAudio = false;
	Message.Properties.Reliability = EMoQReliabilityMode::ReliableOrdered;

	TArray<uint8> Buffer;
	FString Error;
	TestTrue(TEXT("Announce serialize"), Message.Serialize(Buffer, Error));
	TestTrue(TEXT("No error after serialize"), Error.IsEmpty());

	FMoQAnnounceMessage Parsed;
	Error.Reset();
	TestTrue(TEXT("Announce deserialize"), FMoQAnnounceMessage::Deserialize(Buffer, Parsed, Error));
	TestTrue(TEXT("No error after deserialize"), Error.IsEmpty());
	TestEqual(TEXT("TrackId round trip"), Parsed.TrackId, Message.TrackId);
	TestEqual(TEXT("Track name round trip"), Parsed.TrackName, Message.TrackName);
	TestEqual(TEXT("Priority round trip"), Parsed.Properties.Priority, Message.Properties.Priority);
	TestTrue(TEXT("Reliability round trip"), Parsed.Properties.Reliability == Message.Properties.Reliability);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQObjectSerializationTest, "Open3DBroadcast.Open3DTransportQUIC.MoQ.Protocol.Object", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQObjectSerializationTest::RunTest(const FString& Parameters)
{
	FMoQObjectMessage Message;
	Message.TrackId = 7;
	Message.Sequence = 1337;
	Message.Priority = 220;
	Message.TimestampMicros = 123456789ull;
	Message.Reliability = EMoQReliabilityMode::UnreliableSequenced;
	Message.Payload = {0xDE, 0xAD, 0xBE, 0xEF};

	TArray<uint8> Buffer;
	FString Error;
	TestTrue(TEXT("Object serialize"), Message.Serialize(Buffer, Error));
	TestTrue(TEXT("No error after serialize"), Error.IsEmpty());

	FMoQObjectMessage Parsed;
	Error.Reset();
	TestTrue(TEXT("Object deserialize"), FMoQObjectMessage::Deserialize(Buffer, Parsed, Error));
	TestTrue(TEXT("No error after deserialize"), Error.IsEmpty());
	TestEqual(TEXT("TrackId"), Parsed.TrackId, Message.TrackId);
	TestEqual(TEXT("Sequence"), Parsed.Sequence, Message.Sequence);
	TestEqual(TEXT("Priority"), Parsed.Priority, Message.Priority);
	TestEqual(TEXT("Timestamp"), Parsed.TimestampMicros, Message.TimestampMicros);
	TestTrue(TEXT("Payload bytes"), Parsed.Payload == Message.Payload);
	TestTrue(TEXT("Reliability"), Parsed.Reliability == Message.Reliability);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQTrackManagerStateMachineTest, "Open3DBroadcast.Open3DTransportQUIC.MoQ.TrackManager.StateMachine", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQTrackManagerStateMachineTest::RunTest(const FString& Parameters)
{
	FMoQTrackManager Manager;
	FMoQTrackProperties TrackProps;
	TrackProps.Priority = 180;
	TrackProps.Reliability = EMoQReliabilityMode::ReliableOrdered;
	TrackProps.bDatagramsAllowed = true;

	FMoQAnnounceMessage Announce;
	FString Error;
	const FMoQTrackId TrackId = Manager.PublishTrack(TEXT("mocap/sessionA/characterRoot"), TrackProps, Announce, Error);
	TestTrue(TEXT("Track published"), TrackId != 0);
	TestTrue(TEXT("Announce error cleared"), Error.IsEmpty());
	TestEqual(TEXT("Announce track"), Announce.TrackId, TrackId);

	FMoQSubscribeMessage SubscribeMessage;
	const FMoQSubscriptionId SubscriptionId = Manager.SubscribeTrack(TEXT("mocap/sessionA/*"), TrackProps, SubscribeMessage, Error);
	TestTrue(TEXT("Subscription created"), SubscriptionId != 0);
	TestTrue(TEXT("Subscribe error cleared"), Error.IsEmpty());
	TestEqual(TEXT("Initial state pending"), Manager.GetSubscriptionState(SubscriptionId), EMoQSubscriptionState::Pending);

	FMoQSubscribeOkMessage OkMessage;
	OkMessage.SubscriptionId = SubscriptionId;
	OkMessage.TrackId = TrackId;
	OkMessage.TrackName = Announce.TrackName;
	OkMessage.ResolvedProperties = Announce.Properties;
	TestTrue(TEXT("Apply subscribe ok"), Manager.ApplySubscribeOk(OkMessage, Error));
	TestTrue(TEXT("Subscribe ok no error"), Error.IsEmpty());
	TestEqual(TEXT("State becomes active"), Manager.GetSubscriptionState(SubscriptionId), EMoQSubscriptionState::Active);

	FMoQSubscriptionStateInfo Snapshot;
	TestTrue(TEXT("Snapshot available"), Manager.TryGetSubscription(SubscriptionId, Snapshot));
	TestEqual(TEXT("Snapshot bound track"), Snapshot.BoundTrack.TrackId, TrackId);

	FMoQSubscribeErrorMessage ErrorMessage;
	ErrorMessage.SubscriptionId = SubscriptionId;
	ErrorMessage.ErrorCode = 11;
	ErrorMessage.ErrorReason = TEXT("Relay rejected subscription");
	TestTrue(TEXT("Apply subscribe error"), Manager.ApplySubscribeError(ErrorMessage));
	TestEqual(TEXT("State becomes error"), Manager.GetSubscriptionState(SubscriptionId), EMoQSubscriptionState::Error);

	TestTrue(TEXT("Close subscription"), Manager.CloseSubscription(SubscriptionId, TEXT("Session shutdown")));
	TestEqual(TEXT("State becomes closed"), Manager.GetSubscriptionState(SubscriptionId), EMoQSubscriptionState::Closed);

	FMoQUnsubscribeMessage UnsubscribeMessage;
	TestTrue(TEXT("Unsubscribe message produced"), Manager.UnsubscribeTrack(SubscriptionId, UnsubscribeMessage, Error));
	TestTrue(TEXT("Unsubscribe no error"), Error.IsEmpty());
	TestEqual(TEXT("Unsubscribe id"), UnsubscribeMessage.SubscriptionId, SubscriptionId);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQPatternMatchingTest, "Open3DBroadcast.Open3DTransportQUIC.MoQ.TrackManager.PatternMatching", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQPatternMatchingTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Exact match"), FMoQTrackManager::DoesPatternMatch(TEXT("mocap/session1/actor"), TEXT("mocap/session1/actor")));
	TestTrue(TEXT("Trailing wildcard"), FMoQTrackManager::DoesPatternMatch(TEXT("mocap/session1/*"), TEXT("mocap/session1/actorA")));
	TestTrue(TEXT("Leading wildcard"), FMoQTrackManager::DoesPatternMatch(TEXT("*/character"), TEXT("mocap/session2/character")));
	TestTrue(TEXT("Middle wildcard"), FMoQTrackManager::DoesPatternMatch(TEXT("mocap/*/character"), TEXT("mocap/session3/character")));
	TestFalse(TEXT("Mismatch"), FMoQTrackManager::DoesPatternMatch(TEXT("mocap/session1/*"), TEXT("audio/session1/track")));
	TestFalse(TEXT("Empty pattern"), FMoQTrackManager::DoesPatternMatch(TEXT(""), TEXT("anything")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQuicSenderOptionsParseTest, "Open3DBroadcast.Open3DTransportQUIC.Helpers.SenderOptions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FQuicSenderOptionsParseTest::RunTest(const FString& Parameters)
{
	FO3DTransportConfig Config;
	Config.Uri = TEXT("quic://127.0.0.1:9100");
	Config.StreamId = TEXT("mocap/session1/actorA");
	Config.AdvancedParams.Add(TEXT("quic.track_name"), TEXT("mocap/session1/actorB"));
	Config.AdvancedParams.Add(TEXT("quic.priority"), TEXT("180"));
	Config.AdvancedParams.Add(TEXT("quic.reliability"), TEXT("unreliable"));
	Config.AdvancedParams.Add(TEXT("quic.datagrams"), TEXT("0"));

	O3DQuic::FQuicSenderOptions Options;
	FString Error;
	TestTrue(TEXT("ParseSenderOptions succeeds"), O3DQuic::ParseSenderOptions(Config, Options, Error));
	TestTrue(TEXT("No error on parse"), Error.IsEmpty());
	TestEqual(TEXT("Endpoint host"), Options.Endpoint.Host, FString(TEXT("127.0.0.1")));
	TestEqual(TEXT("Endpoint port"), Options.Endpoint.Port, static_cast<uint16>(9100));
	TestEqual(TEXT("Track name"), Options.TrackName, FString(TEXT("mocap/session1/actorB")));
	TestEqual(TEXT("Priority"), Options.TrackProperties.Priority, static_cast<uint8>(180));
	TestTrue(TEXT("Reliability mode"), Options.TrackProperties.Reliability == EMoQReliabilityMode::UnreliableSequenced);
	TestFalse(TEXT("Datagrams disabled"), Options.bEnableDatagrams);
	TestEqual(TEXT("Stream id"), Options.StreamId, FString(TEXT("127.0.0.1:9100")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQuicSenderOptionsInvalidTrackTest, "Open3DBroadcast.Open3DTransportQUIC.Helpers.SenderOptionsInvalid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FQuicSenderOptionsInvalidTrackTest::RunTest(const FString& Parameters)
{
	FO3DTransportConfig Config;
	Config.AdvancedParams.Add(TEXT("quic.track_name"), TEXT("invalid track"));

	O3DQuic::FQuicSenderOptions Options;
	FString Error;
	TestFalse(TEXT("Invalid track fails"), O3DQuic::ParseSenderOptions(Config, Options, Error));
	TestTrue(TEXT("Error populated"), !Error.IsEmpty());

	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQuicSenderStatsFanoutTest, "Open3DBroadcast.Open3DTransportQUIC.Sender.StatsFanout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FQuicSenderStatsFanoutTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FO3DQuicSender Sender;
	FO3DTransportConfig Config;
	Config.Uri = TEXT("quic://127.0.0.1:4700");
	Config.Transport = TEXT("QUIC");
	TestTrue(TEXT("Sender initialized"), Sender.Initialize(Config));

	TSharedPtr<FMockQuicRelay, ESPMode::ThreadSafe> Relay = MakeShared<FMockQuicRelay, ESPMode::ThreadSafe>();
	Relay->FanoutReturn = 2;
	Sender.SetRelayForTesting(Relay);

	TArray<uint8> Payload;
	Payload.Init(0x5A, 32);
	TestTrue(TEXT("Frame enqueued"), Sender.EnqueueFrameForTesting(Payload, 1000));
	TestTrue(TEXT("Frame processed"), Sender.ProcessSingleFrameForTesting());

	FO3DTransportStats Stats = Sender.GetStats();
	TestEqual(TEXT("FramesSent counted once"), Stats.FramesSent, static_cast<int64>(1));
	TestEqual(TEXT("BytesSent matches relay payload"), Stats.BytesSent, static_cast<int64>(Relay->LastPayloadBytes));
	TestEqual(TEXT("DroppedFrames stays zero"), Stats.DroppedFrames, static_cast<int64>(0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQuicSenderStatsDropTest, "Open3DBroadcast.Open3DTransportQUIC.Sender.StatsDrop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FQuicSenderStatsDropTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FO3DQuicSender Sender;
	FO3DTransportConfig Config;
	Config.Uri = TEXT("quic://127.0.0.1:4700");
	Config.Transport = TEXT("QUIC");
	TestTrue(TEXT("Sender initialized"), Sender.Initialize(Config));

	TSharedPtr<FMockQuicRelay, ESPMode::ThreadSafe> Relay = MakeShared<FMockQuicRelay, ESPMode::ThreadSafe>();
	Relay->FanoutReturn = 0;
	Sender.SetRelayForTesting(Relay);

	TArray<uint8> Payload;
	Payload.Init(0x11, 24);
	TestTrue(TEXT("Frame enqueued"), Sender.EnqueueFrameForTesting(Payload, 2000));
	TestTrue(TEXT("Frame processed"), Sender.ProcessSingleFrameForTesting());

	FO3DTransportStats Stats = Sender.GetStats();
	TestEqual(TEXT("FramesSent remains zero"), Stats.FramesSent, static_cast<int64>(0));
	TestEqual(TEXT("DroppedFrames increments"), Stats.DroppedFrames, static_cast<int64>(1));

	return true;
}
#endif
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQuicSenderOptionsDefaultsTest, "Open3DBroadcast.Open3DTransportQUIC.Helpers.SenderOptionsDefaults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FQuicSenderOptionsDefaultsTest::RunTest(const FString& Parameters)
{
	FO3DTransportConfig Config;
	O3DQuic::FQuicSenderOptions Options;
	FString Error;
	TestTrue(TEXT("ParseSenderOptions applies defaults"), O3DQuic::ParseSenderOptions(Config, Options, Error));
	TestTrue(TEXT("No error for defaults"), Error.IsEmpty());
	TestEqual(TEXT("Default host"), Options.Endpoint.Host, FString(TEXT("0.0.0.0")));
	TestEqual(TEXT("Default port"), Options.Endpoint.Port, O3DQuic::DefaultQuicPort);
	TestEqual(TEXT("Default track name"), Options.TrackName, FString(O3DQuic::DefaultTrackName));
	TestEqual(TEXT("Default stream id"), Options.StreamId, FString::Printf(TEXT("%s:%u"), TEXT("0.0.0.0"), O3DQuic::DefaultQuicPort));
	const FString Description = Options.Describe();
	TestTrue(TEXT("Describe contains track"), Description.Contains(Options.TrackName));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
