// Copyright (c) Open3DStream Contributors
//
// WP-S6 integration tests for the TCP transport on 127.0.0.1 with real sockets and an
// ephemeral port (ADR 0006 S2). The framing parser itself is unit-tested and fuzzed in the
// core (test/tcp_stream_parser_tests.cpp, test/fuzz/fuzz_tcp_stream.cpp).
//
// - Burst: 1,000 frames sent back to back arrive complete and in order (TRB-1, TRB-3).
// - SlowReader: the receiver does not read while the sender writes far more than the socket
//   buffers hold; nothing is lost and the client is not dropped (TRB-2).
// - ReconnectAfterSenderRestart: Stop() then Start() on the same sender without Initialize()
//   (TRB-13); the receiver notices the close and reconnects with backoff (TRB-4).
// - IdleKeepalive: an idle sender keeps the receiver connected past its idle timeout (TRB-6).
//
// No fixed sleeps: every wait polls a condition against a deadline. WP-T2 moves these tests
// to the Open3DBroadcastTests module.

#if WITH_DEV_AUTOMATION_TESTS

#include "../Sender/SocketsTcpSender.h"
#include "../Receiver/SocketsTcpReceiver.h"
#include "../Shared/SocketsTransportCommon.h"
#include "../Shared/SocketsTcpTransport.h"

#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

#include "O3DTransportTypes.h"
#include "SerializedFrameConsumerRegistry.h"

namespace O3DSocketsTcpTests
{
	/** Records every frame. Called only from Receiver.Poll(), on the test thread. */
	class FRecordingConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString&, const TArray<uint8>& Buffer, double) override
		{
			Frames.Add(Buffer);
		}

		TArray<TArray<uint8>> Frames;
	};

	int32 FindFreeTcpPort()
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return 0;
		}

		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bIsValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
		Addr->SetPort(0);

		FSocket* Probe = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TcpTestPortProbe"), false);
		if (!Probe)
		{
			return 0;
		}

		int32 Port = 0;
		if (bIsValid && Probe->Bind(*Addr))
		{
			Probe->GetAddress(*Addr);
			Port = Addr->GetPort();
		}
		SocketSubsystem->DestroySocket(Probe);
		return Port;
	}

	FO3DTransportConfig MakeConfig(bool bSender, int32 Port, const TMap<FString, FString>& Extra)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("sockets.tcp");
		Config.Role = bSender ? TEXT("sender") : TEXT("receiver");
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(bSender ? O3DSockets::BindOptionKey : O3DSockets::HostOptionKey, TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));
		for (const TPair<FString, FString>& Pair : Extra)
		{
			Config.AdvancedParams.Add(Pair.Key, Pair.Value);
		}
		return Config;
	}

	/** "SEQ:" + little-endian index + a pattern derived from the index. Never an O3DA envelope. */
	TArray<uint8> MakePayload(int32 Index, int32 Size)
	{
		check(Size >= 8);
		TArray<uint8> Payload;
		Payload.SetNumUninitialized(Size);
		Payload[0] = 'S';
		Payload[1] = 'E';
		Payload[2] = 'Q';
		Payload[3] = ':';
		FMemory::Memcpy(Payload.GetData() + 4, &Index, sizeof(int32));
		for (int32 Byte = 8; Byte < Size; ++Byte)
		{
			Payload[Byte] = static_cast<uint8>((Index * 31 + Byte) & 0xFF);
		}
		return Payload;
	}

	/** Returns the index of the first frame that is not the expected payload, or INDEX_NONE. */
	int32 FindFirstMismatch(const TArray<TArray<uint8>>& Frames, int32 Size)
	{
		for (int32 Index = 0; Index < Frames.Num(); ++Index)
		{
			if (Frames[Index] != MakePayload(Index, Size))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** Polls the receiver until Condition holds or the deadline passes. No fixed sleeps. */
	template <typename TCondition>
	bool PollUntil(FO3DSocketsTcpReceiver& Receiver, double TimeoutSeconds, TCondition&& Condition)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Receiver.Poll();
			if (Condition())
			{
				return true;
			}
			FPlatformProcess::Sleep(0.0f); // yield only
		}
		return Condition();
	}

	/** Waits (without polling the receiver) until Condition holds or the deadline passes. */
	template <typename TCondition>
	bool WaitUntil(double TimeoutSeconds, TCondition&& Condition)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			if (Condition())
			{
				return true;
			}
			FPlatformProcess::Sleep(0.0f); // yield only
		}
		return Condition();
	}

	/** A connected sender/receiver pair on an ephemeral loopback port. */
	struct FPair
	{
		FO3DSocketsTcpSender Sender;
		FO3DSocketsTcpReceiver Receiver;
		TSharedPtr<FRecordingConsumer> Consumer = MakeShared<FRecordingConsumer>();

		bool Setup(FAutomationTestBase& Test, const TMap<FString, FString>& SenderOptions, const TMap<FString, FString>& ReceiverOptions)
		{
			const int32 Port = FindFreeTcpPort();
			if (!Test.TestTrue(TEXT("Loopback TCP port allocated"), Port > 0))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("Sender initializes"), Sender.Initialize(MakeConfig(true, Port, SenderOptions)))
				|| !Test.TestTrue(TEXT("Receiver initializes"), Receiver.Initialize(MakeConfig(false, Port, ReceiverOptions))))
			{
				return false;
			}
			Receiver.SetConsumer(Consumer);
			if (!Test.TestTrue(TEXT("Sender starts"), Sender.Start())
				|| !Test.TestTrue(TEXT("Receiver starts"), Receiver.Start()))
			{
				return false;
			}
			return Test.TestTrue(TEXT("Receiver connects"), PollUntil(Receiver, 10.0, [this]()
			{
				return Receiver.IsConnected() && Sender.HasClient();
			}));
		}

		~FPair()
		{
			Receiver.Stop();
			Sender.Stop();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsTcpBurstTest, "Open3DBroadcast.Transport.Sockets.Tcp.Burst1000", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSocketsTcpBurstTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsTcpTests;

	// TRB-1: frames coalesced into one read used to be dropped after the first.
	constexpr int32 NumFrames = 1000;
	constexpr int32 FrameSize = 1024;

	FPair Pair;
	TMap<FString, FString> SenderOptions;
	SenderOptions.Add(O3DSockets::Tcp::MaxQueueAgeOptionKey, TEXT("0"));
	if (!Pair.Setup(*this, SenderOptions, {}))
	{
		return false;
	}

	int32 Rejected = 0;
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const TArray<uint8> Payload = MakePayload(Index, FrameSize);
		if (!Pair.Sender.SendSerialized(Payload.GetData(), Payload.Num(), TEXT("burst"), 0.0))
		{
			++Rejected;
		}
	}
	TestEqual(TEXT("Sender accepted every frame"), Rejected, 0);

	PollUntil(Pair.Receiver, 30.0, [&Pair]() { return Pair.Consumer->Frames.Num() >= NumFrames; });

	TestEqual(TEXT("Every frame received"), Pair.Consumer->Frames.Num(), NumFrames);
	TestEqual(TEXT("Frames intact and in order (first mismatch)"), FindFirstMismatch(Pair.Consumer->Frames, FrameSize), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("Receiver dropped nothing"), Pair.Receiver.GetStats().DroppedFrames, static_cast<int64>(0));
	TestEqual(TEXT("Sender dropped nothing"), Pair.Sender.GetStats().DroppedFrames, static_cast<int64>(0));
	TestEqual(TEXT("One connection throughout"), Pair.Receiver.GetConnectCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsTcpSlowReaderTest, "Open3DBroadcast.Transport.Sockets.Tcp.SlowReader", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSocketsTcpSlowReaderTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsTcpTests;

	// TRB-2: a full kernel send buffer used to drop the client. 800 x 64 KiB (about 51 MiB) is
	// far more than the 2 MiB socket buffers on each side, so the worker must wait for space.
	constexpr int32 NumFrames = 800;
	constexpr int32 FrameSize = 64 * 1024;

	FPair Pair;
	TMap<FString, FString> SenderOptions;
	SenderOptions.Add(O3DSockets::Tcp::MaxQueueOptionKey, FString::FromInt(128 * 1024 * 1024));
	SenderOptions.Add(O3DSockets::Tcp::MaxQueueAgeOptionKey, TEXT("0"));
	SenderOptions.Add(O3DSockets::Tcp::StallTimeoutOptionKey, TEXT("30000"));
	TMap<FString, FString> ReceiverOptions;
	ReceiverOptions.Add(O3DSockets::TimeoutOptionKey, TEXT("30"));
	if (!Pair.Setup(*this, SenderOptions, ReceiverOptions))
	{
		return false;
	}

	int32 Rejected = 0;
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const TArray<uint8> Payload = MakePayload(Index, FrameSize);
		if (!Pair.Sender.SendSerialized(Payload.GetData(), Payload.Num(), TEXT("slow"), 0.0))
		{
			++Rejected;
		}
	}
	TestEqual(TEXT("Sender accepted every frame"), Rejected, 0);

	// The receiver is not polled here, so it reads nothing until the sender is blocked.
	TestTrue(TEXT("Sender hit a full send buffer"), WaitUntil(15.0, [&Pair]() { return Pair.Sender.GetSendWaitCount() > 0; }));
	TestTrue(TEXT("Client kept while blocked"), Pair.Sender.HasClient());

	PollUntil(Pair.Receiver, 60.0, [&Pair]() { return Pair.Consumer->Frames.Num() >= NumFrames; });

	TestEqual(TEXT("Every frame received"), Pair.Consumer->Frames.Num(), NumFrames);
	TestEqual(TEXT("Frames intact and in order (first mismatch)"), FindFirstMismatch(Pair.Consumer->Frames, FrameSize), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("Sender dropped nothing"), Pair.Sender.GetStats().DroppedFrames, static_cast<int64>(0));
	TestEqual(TEXT("One connection throughout"), Pair.Receiver.GetConnectCount(), 1);
	TestTrue(TEXT("Client still connected"), Pair.Sender.HasClient());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsTcpReconnectTest, "Open3DBroadcast.Transport.Sockets.Tcp.ReconnectAfterSenderRestart", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSocketsTcpReconnectTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsTcpTests;

	constexpr int32 FrameSize = 256;

	FPair Pair;
	TMap<FString, FString> ReceiverOptions;
	ReceiverOptions.Add(O3DSockets::Tcp::BackoffOptionKey, TEXT("50"));
	ReceiverOptions.Add(O3DSockets::Tcp::MaxBackoffOptionKey, TEXT("200"));
	if (!Pair.Setup(*this, {}, ReceiverOptions))
	{
		return false;
	}

	const TArray<uint8> First = MakePayload(0, FrameSize);
	TestTrue(TEXT("First frame queued"), Pair.Sender.SendSerialized(First.GetData(), First.Num(), TEXT("restart"), 0.0));
	TestTrue(TEXT("First frame received"), PollUntil(Pair.Receiver, 10.0, [&Pair]() { return Pair.Consumer->Frames.Num() >= 1; }));

	// Restart the same sender without Initialize() (TRB-13).
	Pair.Sender.Stop();
	TestTrue(TEXT("Receiver notices the sender closed"), PollUntil(Pair.Receiver, 10.0, [&Pair]() { return !Pair.Receiver.IsConnected(); }));
	TestTrue(TEXT("Sender restarts without Initialize"), Pair.Sender.Start());

	TestTrue(TEXT("Receiver reconnects"), PollUntil(Pair.Receiver, 15.0, [&Pair]()
	{
		return Pair.Receiver.GetConnectCount() >= 2 && Pair.Receiver.IsConnected() && Pair.Sender.HasClient();
	}));

	const TArray<uint8> Second = MakePayload(1, FrameSize);
	TestTrue(TEXT("Second frame queued"), Pair.Sender.SendSerialized(Second.GetData(), Second.Num(), TEXT("restart"), 0.0));
	TestTrue(TEXT("Second frame received"), PollUntil(Pair.Receiver, 10.0, [&Pair]() { return Pair.Consumer->Frames.Num() >= 2; }));

	TestEqual(TEXT("Exactly two frames"), Pair.Consumer->Frames.Num(), 2);
	TestEqual(TEXT("Frames intact and in order (first mismatch)"), FindFirstMismatch(Pair.Consumer->Frames, FrameSize), static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsTcpIdleKeepaliveTest, "Open3DBroadcast.Transport.Sockets.Tcp.IdleKeepalive", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSocketsTcpIdleKeepaliveTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsTcpTests;

	// TRB-6: with a 1 s receiver idle timeout, an idle sender used to cause a reconnect every
	// second. Keepalives every 100 ms keep the one connection alive and deliver no frames.
	FPair Pair;
	TMap<FString, FString> SenderOptions;
	SenderOptions.Add(O3DSockets::Tcp::KeepaliveOptionKey, TEXT("100"));
	TMap<FString, FString> ReceiverOptions;
	ReceiverOptions.Add(O3DSockets::TimeoutOptionKey, TEXT("1"));
	if (!Pair.Setup(*this, SenderOptions, ReceiverOptions))
	{
		return false;
	}

	// Observe for three idle timeouts; the condition only turns true if the link flaps.
	const bool bFlapped = PollUntil(Pair.Receiver, 3.0, [&Pair]()
	{
		return !Pair.Receiver.IsConnected() || Pair.Receiver.GetConnectCount() != 1;
	});

	TestFalse(TEXT("Connection stayed up while the sender was idle"), bFlapped);
	TestEqual(TEXT("One connection throughout"), Pair.Receiver.GetConnectCount(), 1);
	TestEqual(TEXT("Keepalives are not delivered as frames"), Pair.Consumer->Frames.Num(), 0);
	TestEqual(TEXT("Keepalives are not counted as frames"), Pair.Receiver.GetStats().FramesReceived, static_cast<int64>(0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
