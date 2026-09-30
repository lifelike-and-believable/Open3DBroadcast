// Copyright (c) Open3DStream Contributors
//
// WP-S11 (TRB-39, TRB-40, TRB-42, TRB-34): NNG option parsing, and one localhost integration test
// per mode and role pair. Every socket uses 127.0.0.1 and a port the OS picked a moment earlier
// (O3DTests::FindFreeLoopbackPort), so nothing needs an external network and the tests are not
// gated by O3DB_NETWORK_TESTS. The transport is reached through Testing/NngTesting.h.
// Option keys are spelled out: they are the user-facing names persisted in settings
// (NngHelpers.h), so these tests also pin them.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG

#include "Testing/NngTesting.h"

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "O3DAudioFrameCodec.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"

#include <atomic>

namespace O3DNngModeRoleTests
{
	constexpr double ConnectTimeoutSeconds = 10.0;
	constexpr double ProbeIntervalSeconds = 0.05;
	constexpr int32 FramesPerPair = 10;

	/**
	 * A config with only mode, loopback host and port set, plus the role when it is not the
	 * default. Everything else is left at its default.
	 */
	FO3DTransportConfig MakeConfig(bool bSender, const TCHAR* Mode, const TCHAR* Role, int32 Port)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("nng");
		Config.Role = bSender ? TEXT("sender") : TEXT("receiver");
		Config.AdvancedParams.Add(TEXT("nng.mode"), Mode);
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		if (Role && *Role)
		{
			Config.AdvancedParams.Add(TEXT("nng.role"), Role);
		}
		return Config;
	}

	bool Resolve(FAutomationTestBase& Test, const FO3DTransportConfig& Config, bool bSender, O3DNngTesting::FResolvedEndpoint& Out)
	{
		FString Error;
		const bool bParsed = O3DNngTesting::ResolveEndpoint(Config, bSender, Out, Error);
		Test.TestTrue(*FString::Printf(TEXT("%s config parses (%s)"), bSender ? TEXT("Sender") : TEXT("Receiver"), *Error), bParsed);
		return bParsed;
	}

	/**
	 * Starts a sender and a receiver with the given mode and role (empty role = default), checks
	 * that exactly one side listens, waits for a probe frame to arrive, then sends FramesPerPair
	 * recorded frames and expects each one byte-exact and in order.
	 */
	bool RunPair(FAutomationTestBase& Test, const TCHAR* SenderMode, const TCHAR* SenderRole, const TCHAR* ReceiverMode, const TCHAR* ReceiverRole, bool bExpectSenderListens)
	{
		const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
		if (!Test.TestTrue(TEXT("Ephemeral loopback port allocated"), Port > 0))
		{
			return false;
		}

		const FO3DTransportConfig SenderConfig = MakeConfig(true, SenderMode, SenderRole, Port);
		const FO3DTransportConfig ReceiverConfig = MakeConfig(false, ReceiverMode, ReceiverRole, Port);

		O3DNngTesting::FResolvedEndpoint SenderEndpoint;
		O3DNngTesting::FResolvedEndpoint ReceiverEndpoint;
		if (!Resolve(Test, SenderConfig, true, SenderEndpoint) || !Resolve(Test, ReceiverConfig, false, ReceiverEndpoint))
		{
			return false;
		}
		Test.TestTrue(TEXT("Sender listens as expected"), SenderEndpoint.bListen == bExpectSenderListens);
		if (!Test.TestTrue(TEXT("Exactly one side listens"), SenderEndpoint.bListen != ReceiverEndpoint.bListen))
		{
			return false;
		}
		Test.TestEqual(TEXT("Both sides use the same address"), SenderEndpoint.TcpAddress, ReceiverEndpoint.TcpAddress);

		const TSharedRef<IOpen3DSender> Sender = O3DNngTesting::CreateSender();
		const TSharedRef<IOpen3DReceiver> Receiver = O3DNngTesting::CreateReceiver();
		const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();

		ON_SCOPE_EXIT
		{
			Receiver->Stop();
			Sender->Stop();
		};

		if (!Test.TestTrue(TEXT("Sender initializes"), Sender->Initialize(SenderConfig))
			|| !Test.TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(ReceiverConfig)))
		{
			return false;
		}
		Receiver->SetConsumer(Consumer);

		// The listening side starts first, so the dialing side's first attempt can succeed.
		const bool bStarted = SenderEndpoint.bListen
			? (Test.TestTrue(TEXT("Sender starts"), Sender->Start()) && Test.TestTrue(TEXT("Receiver starts"), Receiver->Start()))
			: (Test.TestTrue(TEXT("Receiver starts"), Receiver->Start()) && Test.TestTrue(TEXT("Sender starts"), Sender->Start()));
		if (!bStarted)
		{
			return false;
		}

		auto Pump = [&Sender, &Receiver]()
		{
			Receiver->Poll();
			Sender->Tick(0.0f);
		};

		const TArray<TArray<uint8>> ProbeFrames = O3DTests::MakeRecordedFrames(TEXT("__o3d_nng_probe__"), 1);
		if (!Test.TestEqual(TEXT("Probe frame built"), ProbeFrames.Num(), 1))
		{
			return false;
		}
		const TArray<uint8>& Probe = ProbeFrames[0];

		double NextProbe = 0.0;
		const bool bConnected = O3DTests::PollUntil(ConnectTimeoutSeconds,
			[&Consumer]() { return Consumer->Num() > 0; },
			[&Pump, &Sender, &Probe, &NextProbe]()
			{
				Pump();
				const double Now = FPlatformTime::Seconds();
				if (Now >= NextProbe)
				{
					NextProbe = Now + ProbeIntervalSeconds;
					Sender->SendSerialized(Probe.GetData(), Probe.Num(), TEXT("probe"), Now);
				}
			});
		if (!Test.TestTrue(TEXT("Sender and receiver exchange a probe frame"), bConnected))
		{
			return false;
		}

		const TArray<TArray<uint8>> Recorded = O3DTests::MakeRecordedFrames(TEXT("NngModeRoleActor"), FramesPerPair);
		if (!Test.TestEqual(TEXT("Recorded frames built"), Recorded.Num(), FramesPerPair))
		{
			return false;
		}
		for (int32 Index = 0; Index < Recorded.Num(); ++Index)
		{
			const TArray<uint8>& Frame = Recorded[Index];
			const bool bQueued = O3DTests::PollUntil(ConnectTimeoutSeconds,
				[&Sender, &Frame, Index]() { return Sender->SendSerialized(Frame.GetData(), Frame.Num(), TEXT("NngModeRoleActor"), static_cast<double>(Index)); },
				Pump);
			if (!Test.TestTrue(*FString::Printf(TEXT("Frame %d accepted"), Index), bQueued))
			{
				return false;
			}
		}

		auto CollectNonProbe = [&Consumer, &Probe]()
		{
			TArray<TArray<uint8>> Out;
			for (const TArray<uint8>& Frame : Consumer->GetFrames())
			{
				if (Frame != Probe)
				{
					Out.Add(Frame);
				}
			}
			return Out;
		};

		O3DTests::PollUntil(ConnectTimeoutSeconds,
			[&CollectNonProbe]() { return CollectNonProbe().Num() >= FramesPerPair; },
			Pump);

		const TArray<TArray<uint8>> Received = CollectNonProbe();
		Test.TestEqual(TEXT("Every frame arrived exactly once"), Received.Num(), FramesPerPair);
		int32 FirstMismatch = INDEX_NONE;
		for (int32 Index = 0; Index < FMath::Min(Received.Num(), Recorded.Num()); ++Index)
		{
			if (Received[Index] != Recorded[Index])
			{
				FirstMismatch = Index;
				break;
			}
		}
		Test.TestEqual(TEXT("Frames byte-exact and in order (first mismatch)"), FirstMismatch, static_cast<int32>(INDEX_NONE));

		const FO3DTransportStats SenderStats = Sender->GetStats();
		const FO3DTransportStats ReceiverStats = Receiver->GetStats();
		Test.TestTrue(TEXT("Sender counted sent frames"), SenderStats.FramesSent >= FramesPerPair);
		Test.TestEqual(TEXT("Receiver counted each delivered frame once"), ReceiverStats.FramesReceived, static_cast<int64>(Consumer->Num()));
		return true;
	}

	/** Receiver-side audio sink that counts submissions. */
	class FCountingAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& /*Meta*/, const uint8* /*Data*/, int32 /*NumBytes*/) override
		{
			Submissions.fetch_add(1);
		}

		int32 Num() const { return Submissions.load(); }

	private:
		std::atomic<int32> Submissions{0};
	};
}

// ── Option parsing (no sockets) ──────────────────────────────────────────────────────────

// TRB-39: with no host option, the host comes from the Uri, then its ?host= query, then the
// StreamId. Before the fix the default host always won.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngOptionsUriHostTest, "Open3DBroadcast.Transport.NNG.Options.UriHostHonoured", O3DB_TEST_FLAGS)
bool FO3DNngOptionsUriHostTest::RunTest(const FString& Parameters)
{
	using namespace O3DNngModeRoleTests;

	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("nng+sub://10.0.0.5:6001");
		O3DNngTesting::FResolvedEndpoint Endpoint;
		if (Resolve(*this, Config, false, Endpoint))
		{
			TestEqual(TEXT("Uri-only: mode from scheme"), Endpoint.Mode, FString(TEXT("sub")));
			TestEqual(TEXT("Uri-only: host from Uri"), Endpoint.Host, FString(TEXT("10.0.0.5")));
			TestEqual(TEXT("Uri-only: port from Uri"), Endpoint.Port, 6001);
			TestEqual(TEXT("Uri-only: address"), Endpoint.TcpAddress, FString(TEXT("tcp://10.0.0.5:6001")));
		}
	}
	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("nng+pull://10.0.0.9:6002?host=10.0.0.6");
		O3DNngTesting::FResolvedEndpoint Endpoint;
		if (Resolve(*this, Config, false, Endpoint))
		{
			TestEqual(TEXT("Query: mode from scheme"), Endpoint.Mode, FString(TEXT("pull")));
			TestEqual(TEXT("Query: ?host= overrides the Uri host"), Endpoint.Host, FString(TEXT("10.0.0.6")));
			TestEqual(TEXT("Query: port from Uri"), Endpoint.Port, 6002);
		}
	}
	{
		FO3DTransportConfig Config;
		Config.StreamId = TEXT("10.0.0.7:6003/topic");
		O3DNngTesting::FResolvedEndpoint Endpoint;
		if (Resolve(*this, Config, false, Endpoint))
		{
			TestEqual(TEXT("StreamId: host"), Endpoint.Host, FString(TEXT("10.0.0.7")));
			TestEqual(TEXT("StreamId: port"), Endpoint.Port, 6003);
		}
	}
	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("nng+sub://10.0.0.5:6001");
		Config.AdvancedParams.Add(TEXT("host"), TEXT("10.0.0.8"));
		O3DNngTesting::FResolvedEndpoint Endpoint;
		if (Resolve(*this, Config, false, Endpoint))
		{
			TestEqual(TEXT("An explicit host option still wins over the Uri"), Endpoint.Host, FString(TEXT("10.0.0.8")));
		}
	}
	{
		// Nothing but the mode: the defaults for a dialing sub socket.
		FO3DTransportConfig Config;
		Config.AdvancedParams.Add(TEXT("nng.mode"), TEXT("sub"));
		O3DNngTesting::FResolvedEndpoint Endpoint;
		if (Resolve(*this, Config, false, Endpoint))
		{
			TestEqual(TEXT("Defaults: dial host"), Endpoint.Host, FString(TEXT("127.0.0.1")));
			TestEqual(TEXT("Defaults: pub/sub port"), Endpoint.Port, 6000);
		}
	}
	return true;
}

// TRB-40: default roles make exactly one side of every mode pair listen; push-listen and
// pull-dial are honoured; a role a mode cannot take falls back to the default.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngOptionsRolesTest, "Open3DBroadcast.Transport.NNG.Options.DefaultRoles", O3DB_TEST_FLAGS)
bool FO3DNngOptionsRolesTest::RunTest(const FString& Parameters)
{
	using namespace O3DNngModeRoleTests;

	struct FCase
	{
		bool bSender;
		const TCHAR* Mode;
		const TCHAR* Role;
		const TCHAR* ExpectedRole;
		bool bExpectListen;
	};
	const FCase Cases[] =
	{
		{ true,  TEXT("pub"),  TEXT(""),       TEXT("server"), true  },
		{ true,  TEXT("pair"), TEXT(""),       TEXT("server"), true  },
		{ true,  TEXT("push"), TEXT(""),       TEXT("client"), false },
		{ false, TEXT("sub"),  TEXT(""),       TEXT("client"), false },
		{ false, TEXT("pair"), TEXT(""),       TEXT("client"), false },
		{ false, TEXT("pull"), TEXT(""),       TEXT("server"), true  },
		{ true,  TEXT("pair"), TEXT("client"), TEXT("client"), false },
		{ false, TEXT("pair"), TEXT("server"), TEXT("server"), true  },
		{ true,  TEXT("push"), TEXT("server"), TEXT("server"), true  },
		{ false, TEXT("pull"), TEXT("client"), TEXT("client"), false },
		{ true,  TEXT("pub"),  TEXT("client"), TEXT("server"), true  },
		{ false, TEXT("sub"),  TEXT("server"), TEXT("client"), false },
	};

	for (const FCase& Case : Cases)
	{
		const FString Label = FString::Printf(TEXT("%s %s role='%s'"), Case.bSender ? TEXT("sender") : TEXT("receiver"), Case.Mode, Case.Role);
		FO3DTransportConfig Config;
		Config.AdvancedParams.Add(TEXT("nng.mode"), Case.Mode);
		if (*Case.Role)
		{
			Config.AdvancedParams.Add(TEXT("nng.role"), Case.Role);
		}
		O3DNngTesting::FResolvedEndpoint Endpoint;
		if (!Resolve(*this, Config, Case.bSender, Endpoint))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s: role"), *Label), Endpoint.Role, FString(Case.ExpectedRole));
		TestTrue(*FString::Printf(TEXT("%s: listens"), *Label), Endpoint.bListen == Case.bExpectListen);
		TestEqual(*FString::Printf(TEXT("%s: default host"), *Label), Endpoint.Host,
			FString(Case.bExpectListen ? TEXT("0.0.0.0") : TEXT("127.0.0.1")));
	}

	{
		FO3DTransportConfig Config;
		Config.AdvancedParams.Add(TEXT("nng.mode"), TEXT("sub"));
		O3DNngTesting::FResolvedEndpoint Endpoint;
		FString Error;
		TestFalse(TEXT("A sender rejects sub mode"), O3DNngTesting::ResolveEndpoint(Config, true, Endpoint, Error));
	}
	{
		FO3DTransportConfig Config;
		Config.AdvancedParams.Add(TEXT("nng.mode"), TEXT("push"));
		O3DNngTesting::FResolvedEndpoint Endpoint;
		FString Error;
		TestFalse(TEXT("A receiver rejects push mode"), O3DNngTesting::ResolveEndpoint(Config, false, Endpoint, Error));
	}
	return true;
}

// ── Localhost integration, one test per mode and role pair ───────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngPubSubDefaultTest, "Open3DBroadcast.Transport.NNG.ModeRole.PubListen_SubDial", O3DB_TEST_FLAGS)
bool FO3DNngPubSubDefaultTest::RunTest(const FString& Parameters)
{
	return O3DNngModeRoleTests::RunPair(*this, TEXT("pub"), TEXT(""), TEXT("sub"), TEXT(""), /*bExpectSenderListens=*/true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngPairDefaultTest, "Open3DBroadcast.Transport.NNG.ModeRole.PairListen_PairDial", O3DB_TEST_FLAGS)
bool FO3DNngPairDefaultTest::RunTest(const FString& Parameters)
{
	// Default roles on both ends (TRB-40: both used to listen).
	return O3DNngModeRoleTests::RunPair(*this, TEXT("pair"), TEXT(""), TEXT("pair"), TEXT(""), /*bExpectSenderListens=*/true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngPairReversedTest, "Open3DBroadcast.Transport.NNG.ModeRole.PairDial_PairListen", O3DB_TEST_FLAGS)
bool FO3DNngPairReversedTest::RunTest(const FString& Parameters)
{
	return O3DNngModeRoleTests::RunPair(*this, TEXT("pair"), TEXT("client"), TEXT("pair"), TEXT("server"), /*bExpectSenderListens=*/false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngPushPullDefaultTest, "Open3DBroadcast.Transport.NNG.ModeRole.PushDial_PullListen", O3DB_TEST_FLAGS)
bool FO3DNngPushPullDefaultTest::RunTest(const FString& Parameters)
{
	return O3DNngModeRoleTests::RunPair(*this, TEXT("push"), TEXT(""), TEXT("pull"), TEXT(""), /*bExpectSenderListens=*/false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngPushPullReversedTest, "Open3DBroadcast.Transport.NNG.ModeRole.PushListen_PullDial", O3DB_TEST_FLAGS)
bool FO3DNngPushPullReversedTest::RunTest(const FString& Parameters)
{
	// TRB-40: "Pull (client dial)" used to be forced to listen and push to dial.
	return O3DNngModeRoleTests::RunPair(*this, TEXT("push"), TEXT("server"), TEXT("pull"), TEXT("client"), /*bExpectSenderListens=*/true);
}

// ── Receiver stats (no sockets) ──────────────────────────────────────────────────────────

// TRB-42: the demux delivers an audio frame to the sink without counting it; Poll() counts
// every received message once. Before the fix an audio frame was counted in the demux and
// again in Poll().
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngDemuxAudioCountTest, "Open3DBroadcast.Transport.NNG.Demux.AudioNotCountedTwice", O3DB_TEST_FLAGS)
bool FO3DNngDemuxAudioCountTest::RunTest(const FString& Parameters)
{
	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.bEnableAudio = true;
	AudioConfig.Codec = TEXT("PCM16");
	AudioConfig.SampleRate = 48000;
	AudioConfig.NumChannels = 1;

	O3DAudio::FFrameEncoder Encoder;
	if (!TestTrue(TEXT("Encoder initializes"), Encoder.Initialize(AudioConfig, TEXT("nng-test"), TEXT("nng-test"))))
	{
		return false;
	}

	TArray<float> Pcm;
	Pcm.Init(0.25f, 480);
	TArray<O3DAudio::FEncodedFrame> Frames;
	if (!TestTrue(TEXT("Frame encodes"), Encoder.EncodeBuffer(FString(), FString(), Pcm.GetData(), Pcm.Num(), 1, 48000, 1.0, Frames)))
	{
		return false;
	}
	// PCM16 emits one frame per buffer.
	if (!TestEqual(TEXT("One PCM16 frame"), Frames.Num(), 1))
	{
		return false;
	}
	TArray<uint8> Message;
	if (!TestTrue(TEXT("Unified audio message built"), O3DAudio::CreateUnifiedAudioMessage(Frames[0], 1.0, Message)))
	{
		return false;
	}

	const TSharedRef<IOpen3DReceiver> Receiver = O3DNngTesting::CreateReceiver();
	const TSharedRef<O3DNngModeRoleTests::FCountingAudioSink, ESPMode::ThreadSafe> Sink = MakeShared<O3DNngModeRoleTests::FCountingAudioSink, ESPMode::ThreadSafe>();
	Receiver->SetAudioSink(Sink, AudioConfig);

	TestTrue(TEXT("Audio frame accepted by the demux"), O3DNngTesting::ProcessReceivedPayload(*Receiver, Message));
	TestEqual(TEXT("Audio sink received the frame"), Sink->Num(), 1);
	TestEqual(TEXT("The demux does not count the frame (Poll counts it once)"), Receiver->GetStats().FramesReceived, static_cast<int64>(0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG
