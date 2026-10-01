// Copyright Lifelike & Believable. All Rights Reserved.
//
// ADR 0011 (CTL-2): FO3DControlValue, its Blueprint library, and the conversions to and from the
// core types (O3DControlConvert.h). Every value type survives engine -> core -> wire -> core ->
// engine bit-exactly, including non-ASCII text, and case is preserved.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DControlConvert.h"
#include "O3DControlTypes.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/control.h"
THIRD_PARTY_INCLUDES_END

namespace O3DControlTypesTests
{
	TArray<FO3DControlValue> EveryType()
	{
		TArray<uint8> Bytes = { 0, 1, 2, 254, 255 };
		return {
			FO3DControlValue(),
			FO3DControlValue::MakeBool(true),
			FO3DControlValue::MakeInt(MIN_int64),
			FO3DControlValue::MakeFloat(0.1 + 0.2),
			FO3DControlValue::MakeString(TEXT("Fog — thick, café")),
			FO3DControlValue::MakeName(TEXT("Emotion.Joy")),
			FO3DControlValue::MakeVector(FVector(-1.0e-300, 3.0, 1.0e300)),
			FO3DControlValue::MakeQuat(FQuat(0.5, 0.5, 0.5, 0.5)),
			FO3DControlValue::MakeTransform(FTransform(FQuat(0.0, 0.70710678118654752, 0.0, 0.70710678118654752), FVector(1.5, -2.25, 1.0e9), FVector(1.0, 2.0, 0.5))),
			FO3DControlValue::MakeColor(FLinearColor(0.25f, 0.5f, 1.0f, 0.75f)),
			FO3DControlValue::MakeBytes(Bytes),
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlConvertRoundTripTest, "Open3DBroadcast.Shared.Control.Types.RoundTripThroughTheWire", O3DB_TEST_FLAGS)
bool FO3DControlConvertRoundTripTest::RunTest(const FString& Parameters)
{
	for (const FO3DControlValue& Original : O3DControlTypesTests::EveryType())
	{
		O3DS::Control::Value Core;
		O3DControl::ToCore(Original, Core);
		TestTrue(*FString::Printf(TEXT("Engine -> core -> engine: %s"), *Original.ToString()), O3DControl::FromCore(Core) == Original);

		// Through a real message as well, so UTF-8 and doubles survive encoding.
		O3DS::Control::Message Message;
		Message.source_id = "0a1b2c3d4e5f60718293a4b5c6d7e8f9";
		Message.epoch = 1;
		Message.seq = 1;
		O3DS::Control::Entry Entry;
		Entry.key = "k";
		Entry.value = Core;
		Entry.version = 1;
		Message.set.push_back(Entry);
		std::vector<uint8_t> Bytes;
		TestTrue(*FString::Printf(TEXT("Encodes: %s"), *Original.ToString()), O3DS::Control::SerializeMessage(Message, Bytes));
		O3DS::Control::Message Parsed;
		TestTrue(TEXT("Parses"), O3DS::Control::ParseMessage(Bytes.data(), Bytes.size(), Parsed) == O3DS::Control::ParseError::None);
		if (Parsed.set.size() == 1)
		{
			TestTrue(*FString::Printf(TEXT("Wire round trip: %s"), *Original.ToString()), O3DControl::FromCore(Parsed.set[0].value) == Original);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlUtf8Test, "Open3DBroadcast.Shared.Control.Types.Utf8AndCase", O3DB_TEST_FLAGS)
bool FO3DControlUtf8Test::RunTest(const FString& Parameters)
{
	const FString Text = TEXT("Light.Intensity é中\U0001F3AC");
	const std::string Utf8 = O3DControl::ToUtf8(Text);
	TestTrue(TEXT("Encodes to valid UTF-8"), O3DS::Control::IsValidUtf8(Utf8));
	TestEqual(TEXT("Round trip is exact"), O3DControl::FromUtf8(Utf8), Text);
	TestEqual(TEXT("Empty"), O3DControl::FromUtf8(O3DControl::ToUtf8(FString())), FString());

	// Keys are case-sensitive strings: two casings are two keys, and neither is folded.
	TestTrue(TEXT("Case preserved"), O3DControl::FromUtf8(O3DControl::ToUtf8(TEXT("light.intensity"))).Equals(TEXT("light.intensity"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("Name values compare case-sensitively"), FO3DControlValue::MakeName(TEXT("Joy")) == FO3DControlValue::MakeName(TEXT("joy")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlChangeConvertTest, "Open3DBroadcast.Shared.Control.Types.ChangeToBusChange", O3DB_TEST_FLAGS)
bool FO3DControlChangeConvertTest::RunTest(const FString& Parameters)
{
	O3DS::Control::Change Core;
	Core.kind = O3DS::Control::Change::Kind::Event;
	Core.source_id = "src";
	Core.source_name = "BP_Stage";
	Core.name = "vfx.muzzle_flash";
	Core.target = "Hero";
	Core.value = O3DS::Control::Value::MakeName("left_hand");
	Core.epoch = 1001;
	Core.seq = 77;
	Core.sender_time_us = 2500000;
	Core.event_id = 9;

	const FO3DControlChange Change = O3DControl::FromCore(Core, TEXT("stream-a"));
	TestTrue(TEXT("Kind"), Change.Kind == FO3DControlChange::EKind::Event);
	TestEqual(TEXT("Name"), Change.Name, FString(TEXT("vfx.muzzle_flash")));
	TestTrue(TEXT("Value"), Change.Value == FO3DControlValue::MakeName(TEXT("left_hand")));
	TestEqual(TEXT("SourceId"), Change.Meta.SourceId, FString(TEXT("src")));
	TestEqual(TEXT("SourceName"), Change.Meta.SourceName, FString(TEXT("BP_Stage")));
	TestEqual(TEXT("StreamId"), Change.Meta.StreamId, FString(TEXT("stream-a")));
	TestEqual(TEXT("TargetSubject"), Change.Meta.TargetSubject, FString(TEXT("Hero")));
	TestEqual(TEXT("SenderTimeSec"), Change.Meta.SenderTimeSec, 2.5);
	TestEqual(TEXT("Epoch"), Change.Meta.Epoch, static_cast<int64>(1001));
	TestEqual(TEXT("EventId"), Change.Meta.EventId, static_cast<int64>(9));
	TestEqual(TEXT("Events carry no version"), Change.Meta.Version, static_cast<int64>(0));

	Core.kind = O3DS::Control::Change::Kind::ValueCleared;
	Core.version = 40;
	const FO3DControlChange Cleared = O3DControl::FromCore(Core, TEXT("stream-a"));
	TestTrue(TEXT("Cleared kind"), Cleared.Kind == FO3DControlChange::EKind::ValueCleared);
	TestTrue(TEXT("A clear carries no value"), Cleared.Value.Type == EO3DControlValueType::None);
	TestEqual(TEXT("Clear version"), Cleared.Meta.Version, static_cast<int64>(40));
	TestEqual(TEXT("Values carry no event id"), Cleared.Meta.EventId, static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlValueLibraryTest, "Open3DBroadcast.Shared.Control.Types.BlueprintLibrary", O3DB_TEST_FLAGS)
bool FO3DControlValueLibraryTest::RunTest(const FString& Parameters)
{
	bool bOk = false;
	TestEqual(TEXT("Float from Float"), UO3DControlValueLibrary::ControlAsFloat(FO3DControlValue::MakeFloat(0.25), bOk), 0.25);
	TestTrue(TEXT("ok"), bOk);
	TestEqual(TEXT("Float from Int"), UO3DControlValueLibrary::ControlAsFloat(FO3DControlValue::MakeInt(3), bOk), 3.0);
	TestTrue(TEXT("ok"), bOk);
	UO3DControlValueLibrary::ControlAsFloat(FO3DControlValue::MakeString(TEXT("3")), bOk);
	TestFalse(TEXT("No parsing of strings"), bOk);

	TestEqual(TEXT("Int from Float truncates"), UO3DControlValueLibrary::ControlAsInt(FO3DControlValue::MakeFloat(-2.7), bOk), static_cast<int64>(-2));
	UO3DControlValueLibrary::ControlAsInt(FO3DControlValue::MakeFloat(1.0e300), bOk);
	TestFalse(TEXT("Out-of-range Float is not an Int"), bOk);

	TestTrue(TEXT("Bool from Int"), UO3DControlValueLibrary::ControlAsBool(FO3DControlValue::MakeInt(5), bOk));
	TestEqual(TEXT("String from Name"), UO3DControlValueLibrary::ControlAsString(FO3DControlValue::MakeName(TEXT("Joy")), bOk), FString(TEXT("Joy")));
	TestTrue(TEXT("ok"), bOk);

	const FRotator Rotator(10.0, 20.0, 30.0);
	const FO3DControlValue Quat = UO3DControlValueLibrary::MakeControlRotator(Rotator);
	TestTrue(TEXT("Rotators are stored as quaternions"), Quat.Type == EO3DControlValueType::Quat);
	TestTrue(TEXT("Rotator round trip within tolerance"), UO3DControlValueLibrary::ControlAsRotator(Quat, bOk).Equals(Rotator, 1.0e-6));

	UO3DControlValueLibrary::ControlAsVector(FO3DControlValue::MakeColor(FLinearColor::Red), bOk);
	TestFalse(TEXT("Wrong type reports failure"), bOk);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
