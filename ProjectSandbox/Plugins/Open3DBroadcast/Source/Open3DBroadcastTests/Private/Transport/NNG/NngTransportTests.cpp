// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors
//
// NNG pub/sub on 127.0.0.1 and the receive demux. The transport is reached through
// Testing/NngTesting.h (WP-T2).
// Option keys are spelled out: they are the user-facing names persisted in settings
// (NngHelpers.h), so these tests also pin them.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG

#include "Testing/NngTesting.h"

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "Transport/O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DSerializedFrameConsumer.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <atomic>
#include <string>
#include <vector>

namespace
{
	class FTestFrameConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& InStreamId, TConstArrayView<uint8> InPayload, double InTimestamp) override
		{
			StreamId = InStreamId;
			Payload = TArray<uint8>(InPayload.GetData(), InPayload.Num());
			Timestamp = InTimestamp;
			bInvoked = true;
		}

		bool WasInvoked() const { return bInvoked; }
		const FString& GetStreamId() const { return StreamId; }
		const TArray<uint8>& GetPayload() const { return Payload; }
		double GetTimestamp() const { return Timestamp; }
		void Reset()
		{
			bInvoked = false;
			StreamId.Reset();
			Payload.Reset();
			Timestamp = 0.0;
		}

	private:
		bool bInvoked = false;
		FString StreamId;
		TArray<uint8> Payload;
		double Timestamp = 0.0;
	};

	int32 FindAvailableNngPort()
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return 0;
		}

		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bIsValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
		if (!bIsValid)
		{
			return 0;
		}
		Addr->SetPort(0);

		FSocket* TempSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("NNGTransportTestPortProbe"), false);
		if (!TempSocket)
		{
			return 0;
		}

		TempSocket->SetReuseAddr(true);
		int32 Port = 0;
		if (TempSocket->Bind(*Addr))
		{
			TempSocket->Listen(1);
			TempSocket->GetAddress(*Addr);
			Port = Addr->GetPort();
		}

		SocketSubsystem->DestroySocket(TempSocket);
		return Port;
	}

	/** Polls and ticks both sides for DurationSeconds of wall time. Yields; never sleeps. */
	void PumpNngTransports(IOpen3DSender& Sender, IOpen3DReceiver& Receiver, double DurationSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + DurationSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Receiver.Poll();
			Sender.Tick(0.0f);
			FPlatformProcess::YieldThread();
		}
	}

	FO3DTransportConfig BuildNngSenderConfig(int32 Port, uint64 QueueLimitBytes = 0)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("NNG");
		Config.Role = EO3DTransportRole::Sender;
		Config.Uri = FString::Printf(TEXT("tcp://0.0.0.0:%d"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(TEXT("nng.mode"), TEXT("pub"));
		Config.AdvancedParams.Add(TEXT("host"), TEXT("0.0.0.0"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		Config.AdvancedParams.Add(TEXT("nng.role"), TEXT("server"));
		if (QueueLimitBytes > 0)
		{
			Config.AdvancedParams.Add(TEXT("nng.qmax"), FString::Printf(TEXT("%llu"), QueueLimitBytes));
		}
		return Config;
	}

	FO3DTransportConfig BuildNngReceiverConfig(int32 Port)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("NNG");
		Config.Role = EO3DTransportRole::Receiver;
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(TEXT("nng.mode"), TEXT("sub"));
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		Config.AdvancedParams.Add(TEXT("nng.role"), TEXT("client"));
		return Config;
	}

	void PopulateSubjectList(O3DS::SubjectList& List, const TCHAR* SubjectLabel, int32 NumCurves)
	{
		FTCHARToUTF8 SubjectUtf8(SubjectLabel);
		O3DS::Subject* Subject = List.addSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));
		Subject->addTransform("Root", -1);

		for (int32 Index = 0; Index < NumCurves; ++Index)
		{
			const FString CurveName = FString::Printf(TEXT("Curve_%d"), Index);
			FTCHARToUTF8 CurveUtf8(*CurveName);
			Subject->mCurveNames.emplace_back(CurveUtf8.Get(), CurveUtf8.Length());
			Subject->mCurveValues.push_back(static_cast<float>(Index) / FMath::Max(1, NumCurves));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngDataRoundTripTest, "Open3DBroadcast.Transport.NNG.Data.RoundTrip", O3DB_TEST_FLAGS)
bool FO3DNngDataRoundTripTest::RunTest(const FString& Parameters)
{
	const int32 Port = FindAvailableNngPort();
	TestTrue(TEXT("Data port allocated"), Port > 0);
	if (Port <= 0)
	{
		return false;
	}

	FO3DTransportConfig SenderConfig = BuildNngSenderConfig(Port);
	FO3DTransportConfig ReceiverConfig = BuildNngReceiverConfig(Port);

	const TSharedRef<IOpen3DSender> SenderRef = O3DNngTesting::CreateSender();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = O3DNngTesting::CreateReceiver();
	IOpen3DSender& Sender = *SenderRef;
	IOpen3DReceiver& Receiver = *ReceiverRef;

	const bool bSenderInitialized = Sender.Initialize(SenderConfig).IsOk();
	TestTrue(TEXT("Sender initializes"), bSenderInitialized);
	if (!bSenderInitialized)
	{
		return false;
	}

	const bool bReceiverInitialized = Receiver.Initialize(ReceiverConfig).IsOk();
	TestTrue(TEXT("Receiver initializes"), bReceiverInitialized);
	if (!bReceiverInitialized)
	{
		return false;
	}

	// Each test uses its own ephemeral port, so no wait for TIME_WAIT is needed afterwards.
	ON_SCOPE_EXIT
	{
		Sender.Stop();
	};
	ON_SCOPE_EXIT
	{
		Receiver.Stop();
	};

	TSharedPtr<FTestFrameConsumer, ESPMode::ThreadSafe> FrameConsumer = MakeShared<FTestFrameConsumer, ESPMode::ThreadSafe>();
	Receiver.SetConsumer(FrameConsumer);

	// ADR 0005 (vi): the sender reports the subscriber's pipe as a new peer, on an NNG thread.
	TestTrue(TEXT("NNG reports peer joins"), Sender.GetCapabilities().bPeerJoinSignal);
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> PeersJoined = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	Sender.SetPeerJoinedCallback([PeersJoined]() { PeersJoined->fetch_add(1); });

	const bool bReceiverStarted = Receiver.Start().IsOk();
	TestTrue(TEXT("Receiver starts"), bReceiverStarted);
	if (!bReceiverStarted)
	{
		return false;
	}

	const bool bSenderStarted = Sender.Start().IsOk();
	TestTrue(TEXT("Sender starts"), bSenderStarted);
	if (!bSenderStarted)
	{
		return false;
	}

	// Pump transports to allow connection establishment and initial handshake
	// Use longer initial pump to ensure pub/sub subscription is fully negotiated.
	// nng_dial with NNG_FLAG_NONBLOCK returns immediately, but the actual connection
	// and pub/sub subscription negotiation happens asynchronously. We need time for:
	// 1. TCP connection to establish
	// 2. NNG protocol handshake
	// 3. Publisher to recognize the subscriber
	PumpNngTransports(Sender, Receiver, 2.0);

	static constexpr const TCHAR* SubjectLabel = TEXT("NNGSubject");
	O3DS::SubjectList SubjectList;
	PopulateSubjectList(SubjectList, SubjectLabel, 8);

	const bool bSendQueued = O3DTests::SendSubjectList(Sender, SubjectList);
	TestTrue(TEXT("Sender queued frame"), bSendQueued);

	const double TimeoutSeconds = 5.0;
	const double StartTime = FPlatformTime::Seconds();
	while (!FrameConsumer->WasInvoked() && (FPlatformTime::Seconds() - StartTime) < TimeoutSeconds)
	{
		PumpNngTransports(Sender, Receiver, 0.05);
	}

	TestTrue(TEXT("Receiver consumed frame"), FrameConsumer->WasInvoked());
	TestTrue(TEXT("The subscriber's pipe was reported as a new peer"), PeersJoined->load() >= 1);

	if (FrameConsumer->WasInvoked())
	{
		const TArray<uint8>& Payload = FrameConsumer->GetPayload();
		TestTrue(TEXT("Payload non-empty"), Payload.Num() > 0);

		O3DS::SubjectList Parsed;
		const bool bParse = Parsed.Parse(reinterpret_cast<const char*>(Payload.GetData()), Payload.Num());
		TestTrue(TEXT("Payload parses"), bParse);

		if (bParse)
		{
			FTCHARToUTF8 SubjectUtf8(SubjectLabel);
			O3DS::Subject* ParsedSubject = Parsed.findSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));
			TestNotNull(TEXT("Subject round-tripped"), ParsedSubject);
			if (ParsedSubject)
			{
				TestEqual(TEXT("Curve count preserved"), static_cast<int32>(ParsedSubject->mCurveNames.size()), 8);
			}
		}
	}

	const FO3DTransportStats SenderStats = Sender.GetStats();
	TestTrue(TEXT("Sender recorded frames"), SenderStats.FramesSent > 0);
	TestTrue(TEXT("Sender recorded bytes"), SenderStats.BytesSent > 0);

	const FO3DTransportStats ReceiverStats = Receiver.GetStats();
	TestTrue(TEXT("Receiver recorded frames"), ReceiverStats.FramesReceived > 0);
	TestTrue(TEXT("Receiver recorded bytes"), ReceiverStats.BytesReceived > 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngQueueLimitTest, "Open3DBroadcast.Transport.NNG.Queue.Limit", O3DB_TEST_FLAGS)
bool FO3DNngQueueLimitTest::RunTest(const FString& Parameters)
{
	const int32 Port = FindAvailableNngPort();
	TestTrue(TEXT("Data port allocated"), Port > 0);
	if (Port <= 0)
	{
		return false;
	}

	const uint64 QueueLimit = 64ull * 1024ull; // Minimum enforced queue size inside the sender.
	FO3DTransportConfig SenderConfig = BuildNngSenderConfig(Port, QueueLimit);

	const TSharedRef<IOpen3DSender> SenderRef = O3DNngTesting::CreateSender();
	IOpen3DSender& Sender = *SenderRef;
	const bool bSenderInitialized = Sender.Initialize(SenderConfig).IsOk();
	TestTrue(TEXT("Sender initializes"), bSenderInitialized);
	if (!bSenderInitialized)
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Sender.Stop();
	};

	const bool bSenderStarted = Sender.Start().IsOk();
	TestTrue(TEXT("Sender starts"), bSenderStarted);
	if (!bSenderStarted)
	{
		return false;
	}

	static constexpr int32 NumCurves = 8000;
	O3DS::SubjectList PreviewList;
	PopulateSubjectList(PreviewList, TEXT("PreviewSubject"), NumCurves);

	std::vector<char> PreviewBuffer;
	const int32 PreviewBytes = PreviewList.Serialize(PreviewBuffer, 0.0);
	TestTrue(TEXT("Preview payload larger than queue"), PreviewBytes > static_cast<int32>(QueueLimit));

	O3DS::SubjectList LargeList;
	PopulateSubjectList(LargeList, TEXT("LargeSubject"), NumCurves);

	// The drop is logged as a warning (TRB-43).
	AddExpectedError(TEXT("NNG sender queue full"), EAutomationExpectedMessageFlags::Contains, 1);

	const bool bSendQueued = O3DTests::SendSubjectList(Sender, LargeList);
	TestFalse(TEXT("Large payload rejected due to queue limit"), bSendQueued);

	const FO3DTransportStats SenderStats = Sender.GetStats();
	TestEqual(TEXT("Dropped frame recorded"), static_cast<int64>(SenderStats.DroppedFrames), static_cast<int64>(1));

	return true;
}

// TRB-37: a unified-wrapped mocap frame reaches the consumer without the 20-byte
// unified header, and a raw (legacy) frame reaches it unchanged. No sockets: the
// demux is driven directly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngReceiverUnifiedMocapTest, "Open3DBroadcast.Transport.NNG.Demux.UnifiedMocapStripsHeader", O3DB_TEST_FLAGS)
bool FO3DNngReceiverUnifiedMocapTest::RunTest(const FString& Parameters)
{
	O3DS::SubjectList SubjectList;
	PopulateSubjectList(SubjectList, TEXT("NNGDemuxSubject"), 2);
	std::vector<char> Wire;
	SubjectList.Serialize(Wire, 1.0);
	TArray<uint8> Raw;
	Raw.Append(reinterpret_cast<const uint8*>(Wire.data()), static_cast<int32>(Wire.size()));

	TArray<uint8> Unified;
	TestTrue(TEXT("Unified message built"), O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Mocap, O3DS::EUnifiedCodec::O3DS, Raw.GetData(), Raw.Num(), 1.0, Unified));

	const TSharedRef<IOpen3DReceiver> Receiver = O3DNngTesting::CreateReceiver();
	TSharedPtr<FTestFrameConsumer, ESPMode::ThreadSafe> FrameConsumer = MakeShared<FTestFrameConsumer, ESPMode::ThreadSafe>();
	Receiver->SetConsumer(FrameConsumer);

	TestTrue(TEXT("Unified mocap accepted"), O3DNngTesting::ProcessReceivedPayload(*Receiver, Unified));
	TestTrue(TEXT("Consumer invoked for unified mocap"), FrameConsumer->WasInvoked());
	TestTrue(TEXT("Unified header stripped"), FrameConsumer->GetPayload() == Raw);

	O3DS::SubjectList Parsed;
	TestTrue(TEXT("Stripped payload parses"), Parsed.Parse(reinterpret_cast<const char*>(FrameConsumer->GetPayload().GetData()), FrameConsumer->GetPayload().Num()));

	FrameConsumer->Reset();
	TestTrue(TEXT("Raw mocap accepted"), O3DNngTesting::ProcessReceivedPayload(*Receiver, Raw));
	TestTrue(TEXT("Raw payload passed through unchanged"), FrameConsumer->GetPayload() == Raw);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG
