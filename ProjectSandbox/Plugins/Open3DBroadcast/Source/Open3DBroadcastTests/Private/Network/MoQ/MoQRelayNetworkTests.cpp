// Copyright (c) Open3DStream Contributors
//
// Open3DBroadcast.Network.MoQ.*: the MoQ transport against a real relay (ADR 0006 §6, UX-4,
// TRF-34). Formerly MoQCloudflareRelayTests.cpp, whose Cloudflare.Basic test sat in the default
// filter and fell back to a hard-coded public relay.
//
// These tests register no instances unless O3DB_NETWORK_TESTS=1, so they are absent from every
// default run and from the Session Frontend. The relay comes only from O3D_MOQ_RELAY_URL; with
// the flag set and no URL, every test fails instead of passing. They go through the registered
// "MoQ" transport with the production moq-ffi, exactly as the sender component and the LiveLink
// source do. The session-level relay checks of the old file are gone: the session layer is
// covered offline by the fake-FFI tests (Transport/MoQ), and these end-to-end tests cover the
// relay path. Waits poll a condition against a deadline; nothing sleeps.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_MOQ

#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "O3DReceiverRegistry.h"
#include "O3DSenderRegistry.h"
#include "Testing/MoQTesting.h"

namespace O3DNetworkMoQTests
{
	const FName MoQName(TEXT("MoQ"));
	constexpr double ConnectAndDeliverTimeoutSeconds = 30.0;
	constexpr double SendIntervalSeconds = 0.1;

	FString GetRelayUrl()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("O3D_MOQ_RELAY_URL")).TrimStartAndEnd();
	}

	/** A StreamId whose session part is unique, so parallel runs never share a namespace. */
	FString MakeStreamId()
	{
		return FString::Printf(TEXT("%s/actor"), *O3DTests::MakeUniqueName(TEXT("o3dbnet")));
	}

	FO3DTransportConfig MakeConfig(const FString& RelayUrl, const FString& StreamId, bool bSender)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.Role = bSender ? EO3DTransportRole::Sender : EO3DTransportRole::Receiver;
		Config.Uri = RelayUrl;
		Config.StreamId = StreamId;
		return Config;
	}

	/** Runs FFI callbacks and upkeep; sends Payload on each sender at most every SendIntervalSeconds. */
	struct FPump
	{
		TArray<TSharedPtr<IOpen3DSender>> Senders;
		TArray<TArray<uint8>> Payloads;
		TArray<TSharedPtr<IOpen3DReceiver>> Receivers;
		double NextSend = 0.0;

		void operator()()
		{
			MoQTesting::PumpDispatcher();
			const double Now = FPlatformTime::Seconds();
			const bool bSend = Now >= NextSend;
			if (bSend)
			{
				NextSend = Now + SendIntervalSeconds;
			}
			for (int32 Index = 0; Index < Senders.Num(); ++Index)
			{
				Senders[Index]->Tick(0.0f);
				if (bSend)
				{
					Senders[Index]->SendSerialized(FO3DSendPayload::MakeCopy(Payloads[Index].GetData(), Payloads[Index].Num(), TEXT("actor"), Now));
				}
			}
			for (const TSharedPtr<IOpen3DReceiver>& Receiver : Receivers)
			{
				Receiver->Poll();
			}
		}

		void StopAll()
		{
			for (const TSharedPtr<IOpen3DReceiver>& Receiver : Receivers)
			{
				Receiver->Stop();
			}
			for (const TSharedPtr<IOpen3DSender>& Sender : Senders)
			{
				Sender->Stop();
			}
			MoQTesting::PumpDispatcher();
		}
	};

	TSharedPtr<IOpen3DSender> StartSender(FAutomationTestBase& Test, const FO3DTransportConfig& Config)
	{
		TSharedPtr<IOpen3DSender> Sender = O3DTransport::CreateSender(MoQName);
		if (!Test.TestTrue(TEXT("MoQ sender registered"), Sender.IsValid())
			|| !Test.TestTrue(TEXT("Sender initializes"), Sender->Initialize(Config).IsOk())
			|| !Test.TestTrue(TEXT("Sender starts"), Sender->Start().IsOk()))
		{
			return nullptr;
		}
		return Sender;
	}

	TSharedPtr<IOpen3DReceiver> StartReceiver(FAutomationTestBase& Test, const FO3DTransportConfig& Config, const TSharedPtr<FO3DRecordingFrameConsumer>& Consumer)
	{
		TSharedPtr<IOpen3DReceiver> Receiver = O3DTransport::CreateReceiver(MoQName);
		if (!Test.TestTrue(TEXT("MoQ receiver registered"), Receiver.IsValid())
			|| !Test.TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(Config).IsOk()))
		{
			return nullptr;
		}
		Receiver->SetConsumer(Consumer);
		if (!Test.TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk()))
		{
			return nullptr;
		}
		return Receiver;
	}

	bool RunSenderPublishes(FAutomationTestBase& Test, const FString& RelayUrl)
	{
		FPump Pump;
		const TSharedPtr<IOpen3DSender> Sender = StartSender(Test, MakeConfig(RelayUrl, MakeStreamId(), true));
		if (!Sender.IsValid())
		{
			return false;
		}
		Pump.Senders.Add(Sender);
		Pump.Payloads.Add(O3DTests::MakeRecordedFrames(TEXT("actor"), 1)[0]);

		const bool bPublished = O3DTests::PollUntil(ConnectAndDeliverTimeoutSeconds,
			[&Sender]() { return Sender->GetStats().FramesSent >= 2; }, [&Pump]() { Pump(); });
		const FO3DTransportStats Stats = Sender->GetStats();
		Test.TestTrue(*FString::Printf(TEXT("The sender published at least two frames (sent=%lld dropped=%lld)"), Stats.FramesSent, Stats.DroppedFrames), bPublished);
		Pump.StopAll();
		return true;
	}

	bool RunSenderToReceiver(FAutomationTestBase& Test, const FString& RelayUrl)
	{
		const FString StreamId = MakeStreamId();
		const TArray<uint8> Payload = O3DTests::MakeRecordedFrames(TEXT("actor"), 1)[0];
		const TSharedPtr<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();

		FPump Pump;
		const TSharedPtr<IOpen3DSender> Sender = StartSender(Test, MakeConfig(RelayUrl, StreamId, true));
		const TSharedPtr<IOpen3DReceiver> Receiver = StartReceiver(Test, MakeConfig(RelayUrl, StreamId, false), Consumer);
		if (!Sender.IsValid() || !Receiver.IsValid())
		{
			return false;
		}
		Pump.Senders.Add(Sender);
		Pump.Payloads.Add(Payload);
		Pump.Receivers.Add(Receiver);

		const bool bDelivered = O3DTests::PollUntil(ConnectAndDeliverTimeoutSeconds,
			[&Consumer]() { return Consumer->Num() > 0; }, [&Pump]() { Pump(); });
		Test.TestTrue(TEXT("A frame travelled sender -> relay -> receiver"), bDelivered);
		if (bDelivered)
		{
			Test.TestTrue(TEXT("The frame arrived byte-exact"), Consumer->GetFrames()[0] == Payload);
		}
		Pump.StopAll();
		return true;
	}

	bool RunTwoSendersSeparateNamespaces(FAutomationTestBase& Test, const FString& RelayUrl)
	{
		const FString StreamA = MakeStreamId();
		const FString StreamB = MakeStreamId();
		const TArray<uint8> PayloadA = O3DTests::MakeRecordedFrames(TEXT("ActorA"), 1)[0];
		const TArray<uint8> PayloadB = O3DTests::MakeRecordedFrames(TEXT("ActorB"), 1)[0];
		const TSharedPtr<FO3DRecordingFrameConsumer> ConsumerA = MakeShared<FO3DRecordingFrameConsumer>();
		const TSharedPtr<FO3DRecordingFrameConsumer> ConsumerB = MakeShared<FO3DRecordingFrameConsumer>();

		FPump Pump;
		const TSharedPtr<IOpen3DSender> SenderA = StartSender(Test, MakeConfig(RelayUrl, StreamA, true));
		const TSharedPtr<IOpen3DSender> SenderB = StartSender(Test, MakeConfig(RelayUrl, StreamB, true));
		const TSharedPtr<IOpen3DReceiver> ReceiverA = StartReceiver(Test, MakeConfig(RelayUrl, StreamA, false), ConsumerA);
		const TSharedPtr<IOpen3DReceiver> ReceiverB = StartReceiver(Test, MakeConfig(RelayUrl, StreamB, false), ConsumerB);
		if (!SenderA.IsValid() || !SenderB.IsValid() || !ReceiverA.IsValid() || !ReceiverB.IsValid())
		{
			return false;
		}
		Pump.Senders = { SenderA, SenderB };
		Pump.Payloads = { PayloadA, PayloadB };
		Pump.Receivers = { ReceiverA, ReceiverB };

		const bool bBoth = O3DTests::PollUntil(ConnectAndDeliverTimeoutSeconds,
			[&ConsumerA, &ConsumerB]() { return ConsumerA->Num() > 0 && ConsumerB->Num() > 0; }, [&Pump]() { Pump(); });
		Test.TestTrue(TEXT("Both receivers got frames"), bBoth);

		auto OnlyOwn = [](const TSharedPtr<FO3DRecordingFrameConsumer>& Consumer, const TArray<uint8>& Own)
		{
			for (const TArray<uint8>& Frame : Consumer->GetFrames())
			{
				if (Frame != Own)
				{
					return false;
				}
			}
			return true;
		};
		Test.TestTrue(TEXT("Receiver A got only sender A's frames"), OnlyOwn(ConsumerA, PayloadA));
		Test.TestTrue(TEXT("Receiver B got only sender B's frames"), OnlyOwn(ConsumerB, PayloadB));
		Pump.StopAll();
		return true;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FO3DNetworkMoQTests, "Open3DBroadcast.Network.MoQ", O3DB_TEST_FLAGS)

void FO3DNetworkMoQTests::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	if (!O3DTests::AreNetworkTestsEnabled())
	{
		return; // ADR 0006 §6: no instances without O3DB_NETWORK_TESTS=1
	}
	for (const TCHAR* Name : { TEXT("SenderPublishes"), TEXT("SenderToReceiver"), TEXT("TwoSendersSeparateNamespaces") })
	{
		OutBeautifiedNames.Add(Name);
		OutTestCommands.Add(Name);
	}
}

bool FO3DNetworkMoQTests::RunTest(const FString& Parameters)
{
	using namespace O3DNetworkMoQTests;

	const FString RelayUrl = GetRelayUrl();
	if (RelayUrl.IsEmpty())
	{
		AddError(TEXT("O3DB_NETWORK_TESTS=1 but O3D_MOQ_RELAY_URL is not set. Set it to the relay to test against; there is no default relay."));
		return false;
	}
	AddInfo(FString::Printf(TEXT("Relay: %s"), *RelayUrl));

	if (Parameters == TEXT("SenderPublishes"))
	{
		return RunSenderPublishes(*this, RelayUrl);
	}
	if (Parameters == TEXT("SenderToReceiver"))
	{
		return RunSenderToReceiver(*this, RelayUrl);
	}
	if (Parameters == TEXT("TwoSendersSeparateNamespaces"))
	{
		return RunTwoSendersSeparateNamespaces(*this, RelayUrl);
	}
	AddError(FString::Printf(TEXT("Unknown network test '%s'"), *Parameters));
	return false;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_MOQ
