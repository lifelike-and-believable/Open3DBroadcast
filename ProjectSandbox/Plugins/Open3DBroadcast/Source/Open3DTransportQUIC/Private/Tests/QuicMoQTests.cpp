#if WITH_DEV_AUTOMATION_TESTS

#include "MoQ/MoQProtocol.h"
#include "MoQ/MoQTrackManager.h"

#include "Misc/AutomationTest.h"

using namespace O3DMoQ;

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

#endif // WITH_DEV_AUTOMATION_TESTS
