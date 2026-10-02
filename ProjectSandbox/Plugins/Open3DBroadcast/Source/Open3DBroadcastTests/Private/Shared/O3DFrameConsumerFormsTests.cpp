// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 5b (ADR 0007 items 3 and 5, step 5; SHR-16, TRF-38): the two forms of
// ISerializedFrameConsumer.
// - View form: the demux hands the consumer a view of the received bytes themselves (enveloped
//   or raw mocap), not a copy; the view is only for the call, so a consumer that keeps the
//   bytes copies them, and what it kept is unaffected when the caller reuses its buffer.
// - Owned form: DeliverMocapOwned moves the receiver's buffer into the consumer (same heap
//   allocation, the caller's array left empty), and Loopback carries a frame from
//   SendSerialized to the consumer without one copy.
// - A consumer that implements only the view form still gets frames delivered in the owned
//   form (the default bridge), with the same bytes and subject.
// No network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "O3DUnifiedMessage.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Transport/O3DTransportRegistry.h"
#include "Transport/O3DUnifiedReceiveDemux.h"

namespace O3DConsumerFormsTest
{
	/** Implements only the view form. Records where the bytes were and copies them. */
	class FViewOnlyConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& Subject, TConstArrayView<uint8> Bytes, double /*TimestampSeconds*/) override
		{
			Subjects.Add(Subject);
			DataPointers.Add(Bytes.GetData());
			Copies.Emplace(Bytes.GetData(), Bytes.Num());
		}

		TArray<FString> Subjects;
		TArray<const uint8*> DataPointers;
		TArray<TArray<uint8>> Copies;
	};

	/** Implements both forms; keeps what the owned form hands over. */
	class FOwningConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& /*Subject*/, TConstArrayView<uint8> Bytes, double /*TimestampSeconds*/) override
		{
			++ViewCalls;
			ViewCopies.Emplace(Bytes.GetData(), Bytes.Num());
		}

		virtual void SubmitFrameOwned(const FString& Subject, TArray<uint8>&& Bytes, double /*TimestampSeconds*/) override
		{
			Subjects.Add(Subject);
			Kept.Add(MoveTemp(Bytes));
		}

		int32 ViewCalls = 0;
		TArray<TArray<uint8>> ViewCopies;
		TArray<FString> Subjects;
		TArray<TArray<uint8>> Kept;
	};

	static TArray<uint8> MakeFrameBytes(int32 Size, uint8 Seed)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Bytes[Index] = static_cast<uint8>(Seed + Index * 3);
		}
		// Not the envelope magic, so the demux treats a bare buffer as raw mocap.
		Bytes[0] = 1;
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DConsumerViewFormTest, "Open3DBroadcast.Shared.FrameConsumer.ViewFormHasNoCopy", O3DB_TEST_FLAGS)
bool FO3DConsumerViewFormTest::RunTest(const FString& Parameters)
{
	using namespace O3DConsumerFormsTest;
	const TSharedRef<FViewOnlyConsumer> Consumer = MakeShared<FViewOnlyConsumer>();
	FO3DReceiveDemuxSettings Settings;
	Settings.StreamId = TEXT("forms-stream");
	FO3DUnifiedReceiveDemux Demux(Settings);
	Demux.SetConsumer(Consumer);

	// An enveloped mocap message: the consumer sees the payload where it lies in the message.
	const TArray<uint8> Payload = MakeFrameBytes(96, 7);
	TArray<uint8> Message;
	if (!TestTrue(TEXT("Envelope built"), O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Mocap, O3DS::EUnifiedCodec::O3DS, Payload.GetData(), Payload.Num(), 1.0, Message)))
	{
		return false;
	}
	TestTrue(TEXT("Enveloped mocap delivered"), Demux.ProcessMessage(Message.GetData(), Message.Num(), 2.0, TEXT("Hero")) == EO3DDemuxResult::Mocap);

	// A raw (legacy) frame: the consumer sees the caller's buffer itself.
	TArray<uint8> Raw = MakeFrameBytes(64, 40);
	TestTrue(TEXT("Raw mocap delivered"), Demux.ProcessMessage(Raw.GetData(), Raw.Num(), 3.0) == EO3DDemuxResult::Mocap);

	// Bytes already known to be a frame: delivered as they are.
	const TArray<uint8> Known = MakeFrameBytes(32, 90);
	TestTrue(TEXT("Known frame delivered"), Demux.DeliverMocap(TEXT("hero"), Known, 4.0) == EO3DDemuxResult::Mocap);

	if (!TestEqual(TEXT("Three frames"), Consumer->DataPointers.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("Envelope: a view of the payload inside the message, no copy"), Consumer->DataPointers[0] == Message.GetData() + O3DS::UnifiedWireHeaderSize);
	TestTrue(TEXT("Raw: a view of the caller's buffer, no copy"), Consumer->DataPointers[1] == Raw.GetData());
	TestTrue(TEXT("Known: a view of the caller's buffer, no copy"), Consumer->DataPointers[2] == Known.GetData());
	TestTrue(TEXT("Envelope payload bytes"), Consumer->Copies[0] == Payload);

	// Subjects stay case-sensitive strings; an empty one is the stream id.
	TestEqual(TEXT("Subject as sent"), Consumer->Subjects[0], FString(TEXT("Hero")));
	TestEqual(TEXT("No subject: the stream id"), Consumer->Subjects[1], FString(TEXT("forms-stream")));
	TestTrue(TEXT("Case kept"), Consumer->Subjects[2].Equals(TEXT("hero"), ESearchCase::CaseSensitive));

	// The view was only for the call: the caller reuses its buffer, and what the consumer kept
	// (its own copy) is unchanged. The demux keeps nothing of it either.
	const TArray<uint8> RawBefore = Raw;
	FMemory::Memset(Raw.GetData(), 0xAB, Raw.Num());
	Raw.Empty();
	TestTrue(TEXT("The consumer's copy survives the caller reusing its buffer"), Consumer->Copies[1] == RawBefore);

	TestEqual(TEXT("Demux counted three frames"), static_cast<int32>(Demux.GetStats().Mocap), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DConsumerOwnedFormTest, "Open3DBroadcast.Shared.FrameConsumer.OwnedFormMovesTheBuffer", O3DB_TEST_FLAGS)
bool FO3DConsumerOwnedFormTest::RunTest(const FString& Parameters)
{
	using namespace O3DConsumerFormsTest;
	const TSharedRef<FOwningConsumer> Consumer = MakeShared<FOwningConsumer>();
	FO3DUnifiedReceiveDemux Demux;
	Demux.SetConsumer(Consumer);

	TArray<uint8> Frame = MakeFrameBytes(128, 3);
	const TArray<uint8> Expected = Frame;
	const uint8* Allocation = Frame.GetData();
	TestTrue(TEXT("Owned frame delivered"), Demux.DeliverMocapOwned(TEXT("Hero"), MoveTemp(Frame), 1.0) == EO3DDemuxResult::Mocap);

	TestEqual(TEXT("The owned form was used, not the view form"), Consumer->ViewCalls, 0);
	if (TestEqual(TEXT("One frame kept"), Consumer->Kept.Num(), 1))
	{
		TestTrue(TEXT("Same allocation: moved, not copied"), Consumer->Kept[0].GetData() == Allocation);
		TestTrue(TEXT("Same bytes"), Consumer->Kept[0] == Expected);
		TestEqual(TEXT("Subject"), Consumer->Subjects[0], FString(TEXT("Hero")));
	}
	TestEqual(TEXT("The caller's array gave its buffer up"), Frame.Num(), 0);

	// A transient buffer still goes through the view form.
	const TArray<uint8> Transient = MakeFrameBytes(16, 60);
	Demux.DeliverMocap(TEXT("Hero"), Transient, 2.0);
	TestEqual(TEXT("View form for transient bytes"), Consumer->ViewCalls, 1);

	// Without a consumer an owned frame is counted and left with the caller.
	FO3DUnifiedReceiveDemux NoConsumer;
	TArray<uint8> Unclaimed = MakeFrameBytes(8, 1);
	TestTrue(TEXT("Counted as mocap"), NoConsumer.DeliverMocapOwned(TEXT("x"), MoveTemp(Unclaimed), 1.0) == EO3DDemuxResult::Mocap);
	TestEqual(TEXT("Counted without a consumer"), static_cast<int32>(NoConsumer.GetStats().MocapWithoutConsumer), 1);
	TestEqual(TEXT("Nobody took it"), Unclaimed.Num(), 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DConsumerDefaultBridgeTest, "Open3DBroadcast.Shared.FrameConsumer.ViewOnlyConsumerGetsOwnedFrames", O3DB_TEST_FLAGS)
bool FO3DConsumerDefaultBridgeTest::RunTest(const FString& Parameters)
{
	using namespace O3DConsumerFormsTest;
	// A consumer written against the view form only (the LiveLink source's helpers and most
	// test recorders) still gets every frame a receiver hands over in the owned form.
	const TSharedRef<FViewOnlyConsumer> Consumer = MakeShared<FViewOnlyConsumer>();
	FO3DUnifiedReceiveDemux Demux;
	Demux.SetConsumer(Consumer);

	const TArray<uint8> First = MakeFrameBytes(48, 11);
	const TArray<uint8> Second = MakeFrameBytes(24, 77);
	Demux.DeliverMocapOwned(TEXT("Owned"), TArray<uint8>(First), 1.0);
	Demux.DeliverMocap(TEXT("Viewed"), Second, 2.0);

	if (!TestEqual(TEXT("Both frames reached the view form"), Consumer->Copies.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("Owned frame's bytes"), Consumer->Copies[0] == First);
	TestTrue(TEXT("Viewed frame's bytes"), Consumer->Copies[1] == Second);
	TestEqual(TEXT("Owned frame's subject"), Consumer->Subjects[0], FString(TEXT("Owned")));
	TestEqual(TEXT("Viewed frame's subject"), Consumer->Subjects[1], FString(TEXT("Viewed")));

	// Called directly on the interface too.
	ISerializedFrameConsumer& Interface = *Consumer;
	Interface.SubmitFrameOwned(TEXT("Direct"), TArray<uint8>(First), 3.0);
	TestEqual(TEXT("Direct owned call bridged"), Consumer->Copies.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DConsumerLoopbackNoCopyTest, "Open3DBroadcast.Transport.Loopback.FrameReachesConsumerWithoutCopy", O3DB_TEST_FLAGS)
bool FO3DConsumerLoopbackNoCopyTest::RunTest(const FString& Parameters)
{
	using namespace O3DConsumerFormsTest;
	FO3DTransportRegistry& Registry = FO3DTransportRegistry::Get();
	const TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender = Registry.CreateSender(TEXT("Loopback"));
	const TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Receiver = Registry.CreateReceiver(TEXT("Loopback"));
	if (!TestTrue(TEXT("Loopback registered"), Sender.IsValid() && Receiver.IsValid()))
	{
		return false;
	}

	FO3DTransportConfig Config;
	Config.Transport = TEXT("Loopback");
	Config.StreamId = O3DTests::MakeUniqueName(TEXT("forms"));
	const TSharedRef<FOwningConsumer> Consumer = MakeShared<FOwningConsumer>();
	TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(Config).IsOk());
	Receiver->SetConsumer(Consumer);
	TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk());
	TestTrue(TEXT("Sender initializes"), Sender->Initialize(Config).IsOk());
	TestTrue(TEXT("Sender starts"), Sender->Start().IsOk());

	// The sender takes the payload's buffer, the channel moves it, the receiver hands it over in
	// the owned form: the consumer ends up with the allocation the caller filled.
	TArray<uint8> Bytes = MakeFrameBytes(256, 5);
	const TArray<uint8> Expected = Bytes;
	const uint8* Allocation = Bytes.GetData();
	TestTrue(TEXT("Queued"), Sender->SendSerialized(FO3DSendPayload(MoveTemp(Bytes), TEXT("Hero"), 0.0)) == EO3DSendResult::Queued);
	TestEqual(TEXT("Poll delivers one frame"), Receiver->Poll(), 1);

	if (TestEqual(TEXT("The consumer kept one frame"), Consumer->Kept.Num(), 1))
	{
		TestTrue(TEXT("Same bytes"), Consumer->Kept[0] == Expected);
		TestTrue(TEXT("Same allocation: no copy from SendSerialized to the consumer"), Consumer->Kept[0].GetData() == Allocation);
		TestEqual(TEXT("Subject as sent"), Consumer->Subjects[0], FString(TEXT("Hero")));
	}
	TestEqual(TEXT("Not through the view form"), Consumer->ViewCalls, 0);

	Sender->Stop();
	Receiver->Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
