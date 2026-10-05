// Copyright 2026 Lifelike & Believable. All Rights Reserved.

//
// ADR 0011 (CTL-2): the control envelope (O3DUnifiedMessage.h): WriteControlEnvelope and the
// one classifier every receive path uses, TryGetControlPayload. Also checks what a receiver
// built before the control kind does with one: TCP, UDP and NNG route only Mocap and Audio, so
// an envelope of an unknown kind parses but matches neither and is dropped.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DUnifiedMessage.h"

#include <limits>

namespace O3DControlEnvelopeTests
{
	TArray<uint8> SamplePayload(int32 Num)
	{
		TArray<uint8> Payload;
		Payload.SetNumUninitialized(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Payload[Index] = static_cast<uint8>(Index * 31 + 7);
		}
		return Payload;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlEnvelopeRoundTripTest, "Open3DBroadcast.Shared.Control.Envelope.RoundTrip", O3DB_TEST_FLAGS)
bool FO3DControlEnvelopeRoundTripTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Payload = O3DControlEnvelopeTests::SamplePayload(300);
	TArray<uint8> Envelope;
	TestTrue(TEXT("Writes"), O3DS::WriteControlEnvelope(Payload, 12.5, Envelope));
	TestEqual(TEXT("Header plus payload"), Envelope.Num(), O3DS::UnifiedWireHeaderSize + Payload.Num());

	O3DS::FUnifiedHeader Header;
	const uint8* PayloadPtr = nullptr;
	int32 PayloadSize = 0;
	TestTrue(TEXT("Parses as an envelope"), O3DS::ParseUnifiedMessage(Envelope.GetData(), Envelope.Num(), Header, PayloadPtr, PayloadSize));
	TestTrue(TEXT("Kind is Control"), Header.GetKind() == O3DS::EUnifiedKind::Control);
	TestTrue(TEXT("Codec is O3DControl"), Header.GetCodec() == O3DS::EUnifiedCodec::O3DControl);
	TestEqual(TEXT("Timestamp in microseconds"), Header.TimestampUs(), static_cast<uint64>(12500000));

	TConstArrayView<uint8> View;
	TestTrue(TEXT("Classifies as control"), O3DS::TryGetControlPayload(Envelope.GetData(), Envelope.Num(), View));
	TestEqual(TEXT("Payload size"), View.Num(), Payload.Num());
	TestTrue(TEXT("Payload bytes"), FMemory::Memcmp(View.GetData(), Payload.GetData(), Payload.Num()) == 0);
	TestTrue(TEXT("The view points into the envelope (no copy)"), View.GetData() == Envelope.GetData() + O3DS::UnifiedWireHeaderSize);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlEnvelopeLimitsTest, "Open3DBroadcast.Shared.Control.Envelope.Limits", O3DB_TEST_FLAGS)
bool FO3DControlEnvelopeLimitsTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Envelope;
	TestFalse(TEXT("Empty payload refused (TCP reads zero-payload envelopes as keepalives)"), O3DS::WriteControlEnvelope(TArray<uint8>(), 1.0, Envelope));
	TestEqual(TEXT("Refusal leaves the output empty"), Envelope.Num(), 0);

	const TArray<uint8> Max = O3DControlEnvelopeTests::SamplePayload(O3DS::UnifiedMaxControlPayloadSize);
	TestTrue(TEXT("Largest payload accepted"), O3DS::WriteControlEnvelope(Max, 1.0, Envelope));
	TestTrue(TEXT("Whole envelope within the 1100-byte budget"), Envelope.Num() <= 1100);

	const TArray<uint8> Over = O3DControlEnvelopeTests::SamplePayload(O3DS::UnifiedMaxControlPayloadSize + 1);
	TestFalse(TEXT("One byte over refused"), O3DS::WriteControlEnvelope(Over, 1.0, Envelope));

	// Negative and non-finite timestamps are written as 0, not cast (undefined behaviour).
	const TArray<uint8> Small = O3DControlEnvelopeTests::SamplePayload(8);
	for (double Bad : { -5.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
	{
		TestTrue(TEXT("Writes with a bad timestamp"), O3DS::WriteControlEnvelope(Small, Bad, Envelope));
		O3DS::FUnifiedHeader Header;
		const uint8* PayloadPtr = nullptr;
		int32 PayloadSize = 0;
		O3DS::ParseUnifiedMessage(Envelope.GetData(), Envelope.Num(), Header, PayloadPtr, PayloadSize);
		TestEqual(TEXT("Timestamp clamped to 0"), Header.TimestampUs(), static_cast<uint64>(0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlEnvelopeClassifierTest, "Open3DBroadcast.Shared.Control.Envelope.ClassifierRejectsEverythingElse", O3DB_TEST_FLAGS)
bool FO3DControlEnvelopeClassifierTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Payload = O3DControlEnvelopeTests::SamplePayload(64);
	TConstArrayView<uint8> View;

	TArray<uint8> Audio;
	O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Audio, O3DS::EUnifiedCodec::PCM16, Payload.GetData(), Payload.Num(), 1.0, Audio);
	TestFalse(TEXT("Audio envelope is not control"), O3DS::TryGetControlPayload(Audio.GetData(), Audio.Num(), View));

	TArray<uint8> WrongCodec;
	O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Control, O3DS::EUnifiedCodec::Opus, Payload.GetData(), Payload.Num(), 1.0, WrongCodec);
	TestFalse(TEXT("Control kind with another codec is malformed"), O3DS::TryGetControlPayload(WrongCodec.GetData(), WrongCodec.Num(), View));

	TArray<uint8> Oversize;
	const TArray<uint8> Big = O3DControlEnvelopeTests::SamplePayload(O3DS::UnifiedMaxControlPayloadSize + 10);
	O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Control, O3DS::EUnifiedCodec::O3DControl, Big.GetData(), Big.Num(), 1.0, Oversize);
	TestFalse(TEXT("Oversize control payload is malformed"), O3DS::TryGetControlPayload(Oversize.GetData(), Oversize.Num(), View));

	// A raw mocap frame starts with its frame word (1), never the envelope magic.
	TArray<uint8> RawFrame = { 1, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD, 1, 2, 3, 4 };
	TestFalse(TEXT("Raw mocap is not control"), O3DS::TryGetControlPayload(RawFrame.GetData(), RawFrame.Num(), View));

	TArray<uint8> Good;
	O3DS::WriteControlEnvelope(Payload, 1.0, Good);
	for (int32 Len = 0; Len < Good.Num(); ++Len)
	{
		TestFalse(TEXT("Every truncation rejected"), O3DS::TryGetControlPayload(Good.GetData(), Len, View));
	}
	TestFalse(TEXT("Null rejected"), O3DS::TryGetControlPayload(nullptr, 100, View));
	TestEqual(TEXT("A rejection leaves an empty view"), View.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlEnvelopeOldReceiverTest, "Open3DBroadcast.Shared.Control.Envelope.OldReceiversRouteItNowhere", O3DB_TEST_FLAGS)
bool FO3DControlEnvelopeOldReceiverTest::RunTest(const FString& Parameters)
{
	// What SocketsTcpReceiver, SocketsUdpReceiver and NngReceiver did before CTL-3: parse the
	// envelope, then route Audio or Mocap. A control envelope parses, is neither, and is dropped,
	// so it never reaches the mocap consumer or the audio decoder.
	TArray<uint8> Envelope;
	O3DS::WriteControlEnvelope(O3DControlEnvelopeTests::SamplePayload(40), 1.0, Envelope);
	O3DS::FUnifiedHeader Header;
	const uint8* PayloadPtr = nullptr;
	int32 PayloadSize = 0;
	TestTrue(TEXT("Parses as an envelope, so it does not fall back to the raw-mocap path"),
		O3DS::ParseUnifiedMessage(Envelope.GetData(), Envelope.Num(), Header, PayloadPtr, PayloadSize));
	TestTrue(TEXT("Not Audio"), Header.GetKind() != O3DS::EUnifiedKind::Audio);
	TestTrue(TEXT("Not Mocap"), Header.GetKind() != O3DS::EUnifiedKind::Mocap);
	TestTrue(TEXT("Non-empty, so the TCP keepalive check does not consume it"), PayloadSize > 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
