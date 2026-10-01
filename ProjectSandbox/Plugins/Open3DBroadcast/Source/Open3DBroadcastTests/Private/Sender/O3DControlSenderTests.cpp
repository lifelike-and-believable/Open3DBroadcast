// Copyright Lifelike & Believable. All Rights Reserved.

// ADR 0011 (CTL-4): control on the sending side. FO3DControlPublisher against the fake transport
// (so every envelope it sends can be inspected), and the UO3DSenderComponent Blueprint API.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DControlPublisher.h"
#include "O3DSenderComponent.h"
#include "O3DTestFakes.h"
#include "O3DUnifiedMessage.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/control.h"
THIRD_PARTY_INCLUDES_END

namespace O3DControlSenderTests
{
	/** Decodes every control envelope the fake sender accepted. */
	TArray<O3DS::Control::Message> Decode(const FO3DFakeSender& Sender)
	{
		TArray<O3DS::Control::Message> Messages;
		for (const TArray<uint8>& Envelope : Sender.GetRecordedControl())
		{
			TConstArrayView<uint8> Payload;
			O3DS::Control::Message Message;
			if (O3DS::TryGetControlPayload(Envelope.GetData(), Envelope.Num(), Payload)
				&& O3DS::Control::ParseMessage(Payload.GetData(), static_cast<size_t>(Payload.Num()), Message) == O3DS::Control::ParseError::None)
			{
				Messages.Add(MoveTemp(Message));
			}
		}
		return Messages;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlPublisherSendsThroughTransportTest, "Open3DBroadcast.Sender.Control.PublisherSendsEnvelopes", O3DB_TEST_FLAGS)
bool FO3DControlPublisherSendsThroughTransportTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlSenderTests;
	FO3DFakeSender Sender;
	FO3DTransportConfig Config;
	Sender.Initialize(Config);
	Sender.Start();

	FO3DControlPublisher Publisher(TEXT("Stage"));
	TestTrue(TEXT("Values may be set before Start"), Publisher.SetValue(TEXT("env.fog_density"), FString(), FO3DControlValue::MakeFloat(0.3)));
	FString Error;
	TestFalse(TEXT("Events need a running session"), Publisher.FireEvent(TEXT("vfx.spark"), FString(), FO3DControlValue(), &Error));
	TestFalse(TEXT("The refusal says why"), Error.IsEmpty());
	TestEqual(TEXT("Nothing sent while stopped"), Publisher.Tick(Sender), 0);

	Publisher.SetMocapSubjects({ TEXT("Hero") });
	Publisher.Start();
	TestTrue(TEXT("Event accepted once running"), Publisher.FireEvent(TEXT("vfx.spark"), TEXT("Hero"), FO3DControlValue::MakeName(TEXT("left_hand"))));
	TestTrue(TEXT("Messages sent at the first tick"), Publisher.Tick(Sender) > 0);

	const TArray<O3DS::Control::Message> Messages = Decode(Sender);
	bool bSawValue = false;
	bool bSawEvent = false;
	bool bSawSnapshot = false;
	for (const O3DS::Control::Message& Message : Messages)
	{
		TestEqual(TEXT("Source id on the wire"), FString(UTF8_TO_TCHAR(Message.source_id.c_str())), Publisher.GetSourceId());
		TestTrue(TEXT("Mocap subjects on the wire"), Message.mocap_subjects.size() == 1 && Message.mocap_subjects[0] == "Hero");
		bSawSnapshot |= Message.IsSnapshot();
		for (const O3DS::Control::Entry& Entry : Message.set)
		{
			bSawValue |= Entry.key == "env.fog_density";
		}
		for (const O3DS::Control::Event& Event : Message.events)
		{
			bSawEvent |= Event.name == "vfx.spark" && Event.target == "Hero";
		}
	}
	TestTrue(TEXT("The value set before Start went out"), bSawValue);
	TestTrue(TEXT("A snapshot went out at Start"), bSawSnapshot);
	TestTrue(TEXT("The event went out"), bSawEvent);
	TestEqual(TEXT("Control is not counted as a frame"), Sender.GetStats().FramesSent, static_cast<int64>(0));

	FO3DControlValue Readback;
	TestTrue(TEXT("FindValue"), Publisher.FindValue(TEXT("env.fog_density"), FString(), Readback) && Readback == FO3DControlValue::MakeFloat(0.3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlPublisherRetriesRefusedTest, "Open3DBroadcast.Sender.Control.RefusedSendsAreRetried", O3DB_TEST_FLAGS)
bool FO3DControlPublisherRetriesRefusedTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlSenderTests;
	FO3DFakeSender Sender;
	FO3DTransportConfig Config;
	Sender.Initialize(Config);
	Sender.Start();
	Sender.SetMaxQueued(0); // the transport refuses everything

	FO3DControlPublisher Publisher(TEXT("Stage"));
	Publisher.SetConfig(/*Snapshot*/ 10.0, /*Redundancy*/ 1, /*Rate*/ 30.0);
	Publisher.Start();
	Publisher.FireEvent(TEXT("light.cue"), FString(), FO3DControlValue::MakeInt(3));
	TestEqual(TEXT("Refused"), Publisher.Tick(Sender), 0);

	Sender.SetMaxQueued(-1); // the transport recovers
	TestTrue(TEXT("Retried on the next tick"), Publisher.Tick(Sender) > 0);
	bool bSawEvent = false;
	for (const O3DS::Control::Message& Message : Decode(Sender))
	{
		for (const O3DS::Control::Event& Event : Message.events)
		{
			bSawEvent |= Event.name == "light.cue";
		}
	}
	TestTrue(TEXT("The refused event arrived on retry"), bSawEvent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlSourceIdsTest, "Open3DBroadcast.Sender.Control.SourceIdPerInstance", O3DB_TEST_FLAGS)
bool FO3DControlSourceIdsTest::RunTest(const FString& Parameters)
{
	const FO3DControlPublisher A(TEXT("Same"));
	const FO3DControlPublisher B(TEXT("Same"));
	TestNotEqual(TEXT("Two publishers never share an id"), A.GetSourceId(), B.GetSourceId());

	UO3DSenderComponent* Original = NewObject<UO3DSenderComponent>();
	const FString OriginalId = Original->GetControlSourceId();
	TestEqual(TEXT("Stable for one instance"), Original->GetControlSourceId(), OriginalId);
	UO3DSenderComponent* Copy = DuplicateObject<UO3DSenderComponent>(Original, GetTransientPackage());
	TestNotEqual(TEXT("A duplicated component gets its own id (never serialized)"), Copy->GetControlSourceId(), OriginalId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlSenderComponentApiTest, "Open3DBroadcast.Sender.Control.ComponentApiBeforeCapture", O3DB_TEST_FLAGS)
bool FO3DControlSenderComponentApiTest::RunTest(const FString& Parameters)
{
	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>();
	TestTrue(TEXT("SetControlValue before capture is kept"), Component->SetControlValue(TEXT("env.time_of_day"), FO3DControlValue::MakeFloat(18.5), FString()));
	FO3DControlValue Value;
	TestTrue(TEXT("GetControlValue"), Component->GetControlValue(TEXT("env.time_of_day"), FString(), Value) && Value == FO3DControlValue::MakeFloat(18.5));
	TestTrue(TEXT("Keys are case-sensitive"), !Component->GetControlValue(TEXT("Env.Time_Of_Day"), FString(), Value));

	AddExpectedMessage(TEXT("FireControlEvent\\('vfx.spark'\\)"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("FireControlEvent without a running transport is refused and logged"), Component->FireControlEvent(TEXT("vfx.spark"), FO3DControlValue(), FString()));

	Component->ClearControlValue(TEXT("env.time_of_day"), FString());
	TestFalse(TEXT("Cleared"), Component->GetControlValue(TEXT("env.time_of_day"), FString(), Value));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
