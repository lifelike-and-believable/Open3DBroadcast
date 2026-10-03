// Copyright Lifelike & Believable. All Rights Reserved.

// FO3DReceiverControlRouter (WP-A3 step 4, RCV-29): the receiver side of the control channel
// (docs/adr/0011-control-channel.md item 8), reached through FO3DReceiverControlRouterProbe. Payloads
// come from the core's own ControlPublisher; what the router publishes is read from FO3DControlBus.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "O3DControlBus.h"
#include "O3DControlSettings.h"
#include "Testing/O3DReceiverTesting.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/control.h"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

namespace O3DReceiverControlRouterTests
{
	/** Sets bAlignControlToMocap for the scope. */
	struct FScopedAlignment
	{
		bool bPrevious;
		explicit FScopedAlignment(bool bAlign)
			: bPrevious(GetDefault<UO3DControlSettings>()->bAlignControlToMocap)
		{
			GetMutableDefault<UO3DControlSettings>()->bAlignControlToMocap = bAlign;
		}
		~FScopedAlignment()
		{
			GetMutableDefault<UO3DControlSettings>()->bAlignControlToMocap = bPrevious;
		}
	};

	/** Every value change the bus publishes for one source. */
	struct FBusRecorder
	{
		FString SourceId;
		TArray<FO3DControlChange> Changes;
		FDelegateHandle Handle;

		explicit FBusRecorder(const FString& InSourceId)
			: SourceId(InSourceId)
		{
			Handle = FO3DControlBus::OnChange().AddLambda([this](const FO3DControlChange& Change)
			{
				if (Change.Meta.SourceId == SourceId)
				{
					Changes.Add(Change);
				}
			});
		}

		~FBusRecorder()
		{
			FO3DControlBus::OnChange().Remove(Handle);
			FO3DControlBus::ForgetSource(SourceId);
		}

		double LastValue() const
		{
			return Changes.Num() > 0 ? Changes.Last().Value.FloatValue : -1.0;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverControlRouterTest, "Open3DBroadcast.Receiver.ControlRouter.RoutesAlignsAndDiscards", O3DB_TEST_FLAGS)
bool FO3DReceiverControlRouterTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverControlRouterTests;

	const FString SourceId = FString::Printf(TEXT("router-test-%s"), *FGuid::NewGuid().ToString());
	const std::string SourceIdUtf8(TCHAR_TO_UTF8(*SourceId));
	const FString StreamId(TEXT("stream-a"));

	O3DS::Control::ControlPublisher Publisher(SourceIdUtf8, "RouterTest");
	Publisher.SetMocapSubjects({ "Hero" });
	Publisher.Start(5);

	// One publisher tick at the given sender time, every message it produced.
	auto Emit = [&Publisher](double NowS, uint64 SenderTimeUs)
	{
		std::vector<O3DS::Control::OutgoingMessage> Out;
		Publisher.Tick(NowS, SenderTimeUs, 0, Out);
		return Out;
	};

	FO3DReceiverControlRouterProbe Router;
	Router.ApplyConfig();
	FBusRecorder Bus(SourceId);

	// Disabled: dropped and counted, nothing published.
	Publisher.SetValue("light.level", "", O3DS::Control::Value::MakeDouble(1.0));
	const std::vector<O3DS::Control::OutgoingMessage> First = Emit(1.0, 1000000);
	for (const O3DS::Control::OutgoingMessage& Message : First)
	{
		Router.HandlePayload(false, TConstArrayView<uint8>(Message.bytes.data(), static_cast<int32>(Message.bytes.size())), 1.0, StreamId);
	}
	TestEqual(TEXT("Disabled payloads are counted"), Router.GetPayloadsDroppedDisabled(), static_cast<uint64>(First.size()));
	TestEqual(TEXT("And not published"), Bus.Changes.Num(), 0);

	// Enabled without alignment: published at once, with the stream id.
	{
		FScopedAlignment NoAlignment(false);
		Publisher.SetValue("light.level", "", O3DS::Control::Value::MakeDouble(2.0));
		for (const O3DS::Control::OutgoingMessage& Message : Emit(2.0, 2000000))
		{
			Router.HandlePayload(true, TConstArrayView<uint8>(Message.bytes.data(), static_cast<int32>(Message.bytes.size())), 2.0, StreamId);
		}
		TestTrue(TEXT("Published at once without alignment"), Bus.LastValue() == 2.0);
		TestTrue(TEXT("With the receiver's stream id"), Bus.Changes.Num() > 0 && Bus.Changes.Last().Meta.StreamId == StreamId);
		TestEqual(TEXT("Nothing held"), Router.GetNumHeld(), 0);
	}

	// With alignment: held until LiveLink presents the change's sender time.
	{
		FScopedAlignment Alignment(true);
		const int32 Before = Bus.Changes.Num();
		Publisher.SetValue("light.level", "", O3DS::Control::Value::MakeDouble(3.0));
		for (const O3DS::Control::OutgoingMessage& Message : Emit(3.0, 3000000))
		{
			Router.HandlePayload(true, TConstArrayView<uint8>(Message.bytes.data(), static_cast<int32>(Message.bytes.size())), 3.0, StreamId);
		}
		TestTrue(TEXT("Held while the pose is older"), Router.GetNumHeld() > 0);
		Router.Tick(true, 3.01, StreamId, 2500000);
		TestEqual(TEXT("Still held at an older presented time"), Bus.Changes.Num(), Before);
		TestTrue(TEXT("The mocap subjects of the source were asked about"), Router.LastAskedSubjects == TArray<FString>({ TEXT("Hero") }));
		Router.Tick(true, 3.02, StreamId, 3000000);
		TestTrue(TEXT("Released once the pose reaches it"), Bus.LastValue() == 3.0);
		TestEqual(TEXT("Nothing held after"), Router.GetNumHeld(), 0);

		// Held changes are published when the transport stops.
		Publisher.SetValue("light.level", "", O3DS::Control::Value::MakeDouble(4.0));
		for (const O3DS::Control::OutgoingMessage& Message : Emit(4.0, 4000000))
		{
			Router.HandlePayload(true, TConstArrayView<uint8>(Message.bytes.data(), static_cast<int32>(Message.bytes.size())), 4.0, StreamId);
		}
		TestTrue(TEXT("Held again"), Router.GetNumHeld() > 0);
		Router.FlushHeld(StreamId);
		TestTrue(TEXT("FlushHeld publishes them"), Bus.LastValue() == 4.0);
	}

	// Turning control off forgets the source on the bus, silently.
	TestTrue(TEXT("The bus knows the source"), FO3DControlBus::GetSources().Contains(SourceId));
	const int32 BeforeDisable = Bus.Changes.Num();
	Router.Tick(false, 5.0, StreamId, -1);
	TestFalse(TEXT("Disabling forgets the source"), FO3DControlBus::GetSources().Contains(SourceId));
	TestEqual(TEXT("Without publishing anything"), Bus.Changes.Num(), BeforeDisable);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
