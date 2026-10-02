// Copyright (c) Open3DStream Contributors
//
// Offline conformance profiles for the transports in this plugin (ADR 0006 §4, WP-T2).
// Every fixture uses 127.0.0.1 and an ephemeral port, a unique loopback channel, or the fake
// moq-ffi table. WebRTC is registered by the Open3DBroadcastWebRTC add-on plugin (WP-F11); its
// profile belongs to an add-on test module (ADR 0006, WP-T2e). Until that exists the WebRTC name
// is recorded as deferred, so running the suite with the add-on enabled gives no HasProfile
// failure. Without the add-on the name is never registered and the deferral has no effect.

#include "Conformance/O3DConformanceProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "O3DConformance.h"
#include "O3DTestFakes.h"
#include "O3DTestHarness.h"

#if O3D_WITH_TRANSPORT_MOQ
#include "Testing/MoQTesting.h"
#include "Transport/MoQ/MoQFakeFfi.h"
#endif

namespace O3DConformanceProfiles
{
	const FName FakeName(TEXT("Fake"));
	const FName LoopbackName(TEXT("Loopback"));
	const FName TcpName(TEXT("TCP"));
	const FName UdpName(TEXT("UDP"));
	const FName NngName(TEXT("NNG"));
	const FName MoQName(TEXT("MoQ"));
	const FName WebRtcName(TEXT("WebRTC"));

	const EO3DConformanceCase SenderAndReceiverCases =
		EO3DConformanceCase::LifecycleStopIsIdempotent
		| EO3DConformanceCase::LifecycleRestartAfterStop
		| EO3DConformanceCase::ReceiverLifecycle
		| EO3DConformanceCase::SendRejectedWhenNotRunning
		| EO3DConformanceCase::SendConcurrent
		| EO3DConformanceCase::StatsMonotonic
		// ADR 0007 item 3 and 4 (WP-A1 PR 3): result codes, capabilities and connection state.
		| EO3DConformanceCase::ReceiverStartWithoutConsumer
		| EO3DConformanceCase::SendEmptyPayloadInvalid
		| EO3DConformanceCase::CapabilitiesMatch
		| EO3DConformanceCase::ConnectionStateLifecycle;

	/**
	 * What every built-in transport supports: a sender and a receiver, audio both ways and control
	 * (ADR 0011). The profiles below add the delivery guarantee (ADR 0005 (iii)) and the rest.
	 */
	FO3DTransportCapabilities MakeBaseCapabilities(EO3DDeliveryGuarantee Delivery)
	{
		FO3DTransportCapabilities Caps;
		Caps.bSend = true;
		Caps.bReceive = true;
		Caps.bAudioSend = true;
		Caps.bAudioReceive = true;
		Caps.bControl = true;
		Caps.Delivery = Delivery;
		return Caps;
	}

	/** Control channel cases (ADR 0011) for transports that carry control and deliver reliably. */
	const EO3DConformanceCase ControlCases =
		EO3DConformanceCase::ControlRoundTrip
		| EO3DConformanceCase::ControlRejectedWhenNotRunning
		| EO3DConformanceCase::ControlStopWhileSending;

	// ── Fake (checks the harness and the fakes against the same contract) ──────────────────

	class FFakeFixture final : public FO3DConformanceFixture
	{
	public:
		explicit FFakeFixture(TUniquePtr<FO3DFakeTransportScope> InScope)
			: FO3DConformanceFixture(InScope->GetName())
			, Scope(MoveTemp(InScope))
		{
		}

		virtual FO3DTransportConfig MakeSenderConfig() override
		{
			FO3DTransportConfig Config;
			Config.Transport = GetTransportName();
			Config.Role = EO3DTransportRole::Sender;
			Config.StreamId = TEXT("fake");
			return Config;
		}

		virtual FO3DTransportConfig MakeReceiverConfig() override
		{
			FO3DTransportConfig Config = MakeSenderConfig();
			Config.Role = EO3DTransportRole::Receiver;
			return Config;
		}

		virtual FO3DTransportConfig MakeBackpressureSenderConfig() override
		{
			FO3DTransportConfig Config = MakeSenderConfig();
			Config.AdvancedParams.Add(TEXT("fake.maxqueued"), TEXT("1"));
			return Config;
		}

	private:
		TUniquePtr<FO3DFakeTransportScope> Scope;
	};

	// ── Loopback ─────────────────────────────────────────────────────────────────────────

	class FLoopbackFixture final : public FO3DConformanceFixture
	{
	public:
		FLoopbackFixture()
			: FO3DConformanceFixture(LoopbackName)
			, Channel(O3DTests::MakeUniqueName(TEXT("o3dconformance")))
		{
		}

		virtual FO3DTransportConfig MakeSenderConfig() override { return MakeConfig(true, 64); }
		virtual FO3DTransportConfig MakeReceiverConfig() override { return MakeConfig(false, 64); }
		/** One queued frame fills the channel: nobody polls it, so the next sends are dropped. */
		virtual FO3DTransportConfig MakeBackpressureSenderConfig() override { return MakeConfig(true, 1); }

	private:
		FO3DTransportConfig MakeConfig(bool bSender, int32 QueueCapacity) const
		{
			FO3DTransportConfig Config;
			Config.Transport = TEXT("Loopback");
			Config.Role = bSender ? EO3DTransportRole::Sender : EO3DTransportRole::Receiver;
			Config.StreamId = Channel;
			Config.Uri = FString::Printf(TEXT("loopback://%s?role=%s"), *Channel, bSender ? TEXT("pub") : TEXT("sub"));
			Config.AdvancedParams.Add(TEXT("channel"), Channel);
			Config.AdvancedParams.Add(TEXT("loopback.maxqueue"), FString::FromInt(QueueCapacity));
			return Config;
		}

		const FString Channel;
	};

	// ── Sockets (TCP, UDP) ───────────────────────────────────────────────────────────────
	// Option keys are the user-facing names persisted in settings (SocketsTransportCommon.h,
	// SocketsTcpTransport.h); spelling them out here also pins them.

	class FTcpFixture final : public FO3DConformanceFixture
	{
	public:
		FTcpFixture()
			: FO3DConformanceFixture(TcpName)
			, Port(O3DTests::FindFreeLoopbackPort(/*bTcp=*/true))
		{
		}

		virtual FO3DTransportConfig MakeSenderConfig() override { return MakeConfig(true, 0); }
		virtual FO3DTransportConfig MakeReceiverConfig() override { return MakeConfig(false, 0); }
		/** 64 KiB is the smallest send queue the TCP sender accepts (Tcp::MinQueueBytes). */
		virtual FO3DTransportConfig MakeBackpressureSenderConfig() override { return MakeConfig(true, 64 * 1024); }

	private:
		FO3DTransportConfig MakeConfig(bool bSender, int32 MaxQueueBytes) const
		{
			FO3DTransportConfig Config;
			Config.Transport = TEXT("TCP");
			Config.Role = bSender ? EO3DTransportRole::Sender : EO3DTransportRole::Receiver;
			Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), Port);
			Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
			Config.AdvancedParams.Add(bSender ? TEXT("bind") : TEXT("host"), TEXT("127.0.0.1"));
			Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
			if (bSender)
			{
				// Frames are never aged out of the queue, so the round trip cannot lose one.
				Config.AdvancedParams.Add(TEXT("tcp.maxqueueage"), TEXT("0"));
				if (MaxQueueBytes > 0)
				{
					Config.AdvancedParams.Add(TEXT("tcp.maxqueue"), FString::FromInt(MaxQueueBytes));
				}
			}
			return Config;
		}

		const int32 Port;
	};

	class FUdpFixture final : public FO3DConformanceFixture
	{
	public:
		FUdpFixture()
			: FO3DConformanceFixture(UdpName)
			, Port(O3DTests::FindFreeLoopbackPort(/*bTcp=*/false))
		{
		}

		virtual FO3DTransportConfig MakeSenderConfig() override { return MakeConfig(true); }
		virtual FO3DTransportConfig MakeReceiverConfig() override { return MakeConfig(false); }

	private:
		FO3DTransportConfig MakeConfig(bool bSender) const
		{
			FO3DTransportConfig Config;
			Config.Transport = TEXT("UDP");
			Config.Role = bSender ? EO3DTransportRole::Sender : EO3DTransportRole::Receiver;
			Config.Uri = FString::Printf(TEXT("udp://127.0.0.1:%d"), Port);
			Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
			Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
			Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
			return Config;
		}

		const int32 Port;
	};

	// ── NNG ──────────────────────────────────────────────────────────────────────────────

	class FNngFixture final : public FO3DConformanceFixture
	{
	public:
		FNngFixture()
			: FO3DConformanceFixture(NngName)
			, Port(O3DTests::FindFreeLoopbackPort(/*bTcp=*/true))
		{
		}

		virtual FO3DTransportConfig MakeSenderConfig() override { return MakeConfig(true, 0); }
		virtual FO3DTransportConfig MakeReceiverConfig() override { return MakeConfig(false, 0); }
		/** 64 KiB is the smallest queue the NNG sender accepts (kMinQueueBytes). */
		virtual FO3DTransportConfig MakeBackpressureSenderConfig() override { return MakeConfig(true, 64 * 1024); }

		virtual void AddExpectedMessages(FAutomationTestBase& Test, EO3DConformanceCase Case) override
		{
			if (Case == EO3DConformanceCase::SendBackpressure)
			{
				// A full queue is logged as a warning (TRB-43), at most once per 2 s per sender.
				Test.AddExpectedError(TEXT("NNG sender queue full"), EAutomationExpectedMessageFlags::Contains, 1);
			}
		}

	private:
		FO3DTransportConfig MakeConfig(bool bSender, int32 MaxQueueBytes) const
		{
			FO3DTransportConfig Config;
			Config.Transport = TEXT("NNG");
			Config.Role = bSender ? EO3DTransportRole::Sender : EO3DTransportRole::Receiver;
			Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), Port);
			Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
			Config.AdvancedParams.Add(TEXT("nng.mode"), bSender ? TEXT("pub") : TEXT("sub"));
			Config.AdvancedParams.Add(TEXT("nng.role"), bSender ? TEXT("server") : TEXT("client"));
			Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
			Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
			if (MaxQueueBytes > 0)
			{
				Config.AdvancedParams.Add(TEXT("nng.qmax"), FString::FromInt(MaxQueueBytes));
			}
			return Config;
		}

		const int32 Port;
	};

	// ── MoQ (fake moq-ffi) ───────────────────────────────────────────────────────────────

#if O3D_WITH_TRANSPORT_MOQ
	class FMoQFixture final : public FO3DConformanceFixture
	{
	public:
		FMoQFixture()
			: FO3DConformanceFixture(MoQName)
			, Fake(FMoQFakeFfi::Create())
			, StreamId(FString::Printf(TEXT("%s/actor"), *O3DTests::MakeUniqueName(TEXT("o3dconformance"))))
		{
			MoQTesting::InitializeDispatcher();
		}

		virtual ~FMoQFixture() override
		{
			Fake->DiscardHeldWork();
			MoQTesting::PumpDispatcher();
		}

		virtual TSharedPtr<IOpen3DSender> CreateSender() override
		{
			return MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, ++Seed);
		}

		virtual TSharedPtr<IOpen3DReceiver> CreateReceiver() override
		{
			return MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, ++Seed);
		}

		virtual FO3DTransportConfig MakeSenderConfig() override { return MakeConfig(0); }
		virtual FO3DTransportConfig MakeReceiverConfig() override { return MakeConfig(0); }
		/** 256 KiB is the smallest MoQ send queue (MoQHelpers::kMinQueueBytes). */
		virtual FO3DTransportConfig MakeBackpressureSenderConfig() override { return MakeConfig(MoQTesting::GetBackoffLimits().MinQueueBytes); }

		virtual void Pump() override
		{
			MoQTesting::PumpDispatcher();
		}

		virtual void AddExpectedMessages(FAutomationTestBase& Test, EO3DConformanceCase Case) override
		{
			if (Case == EO3DConformanceCase::SendBackpressure)
			{
				// The first drop of each sender is logged as a warning (rate limited to one per 2 s).
				Test.AddExpectedError(TEXT("MoQ sender queue overflow"), EAutomationExpectedMessageFlags::Contains, 1);
			}
		}

		virtual bool RunDestroyWithCallbacksInFlight(FAutomationTestBase& Test) override
		{
			// The connect never returns while the sender lives; it returns (and fires its
			// callbacks) on a background thread after the sender is stopped and destroyed.
			Fake->bHoldBlockingWork = true;
			FO3DTransportConfig Config = MakeSenderConfig();
			Config.Audio.bEnableAudio = true;
			Config.Audio.SampleRate = 48000;
			Config.Audio.NumChannels = 1;

			TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink;
			{
				TSharedPtr<IOpen3DSender> Sender = CreateSender();
				Test.TestTrue(TEXT("Initialize"), Sender->Initialize(Config).IsOk());
				Test.TestTrue(TEXT("Start (connect held)"), Sender->Start().IsOk());
				Sink = Sender->CreateAudioSink(Config.Audio);
				Test.TestTrue(TEXT("Audio sink created"), Sink.IsValid());
				Sender->Stop();
			}

			const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> FakeRef = Fake;
			FGraphEventRef Late = FFunctionGraphTask::CreateAndDispatchWhenReady([FakeRef]()
			{
				FakeRef->RunHeldWork();
				FakeRef->FireConnectionState(1, MOQ_STATE_CONNECTED);
				FakeRef->FireConnectionState(1, MOQ_STATE_DISCONNECTED);
			}, TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask);
			FTaskGraphInterface::Get().WaitUntilTaskCompletes(Late, ENamedThreads::GameThread);
			Pump();

			const float Probe[4] = { 0.1f, -0.1f, 0.2f, -0.2f };
			Test.TestFalse(TEXT("The dead sender's sink rejects PCM"), Sink.IsValid() && Sink->SubmitPcm(TEXT("stale"), Probe, 4, 1, 48000, 0.0));
			Sink.Reset();
			Pump();

			Test.TestTrue(TEXT("The abandoned client was destroyed"), Fake->IsClientDestroyed(1));
			Test.TestEqual(TEXT("Every publisher destroyed"), Fake->GetPublishersDestroyed(), Fake->GetPublishersCreated());
			return true;
		}

	private:
		FO3DTransportConfig MakeConfig(uint64 QueueBytes) const
		{
			FO3DTransportConfig Config;
			Config.Transport = TEXT("MoQ");
			Config.Uri = TEXT("https://fake.relay.invalid:443"); // only the fake FFI sees it
			Config.StreamId = StreamId;
			if (QueueBytes > 0)
			{
				Config.AdvancedParams.Add(TEXT("queue_bytes"), FString::Printf(TEXT("%llu"), QueueBytes));
			}
			return Config;
		}

		TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake;
		const FString StreamId;
		uint64 Seed = 0;
	};
#endif // O3D_WITH_TRANSPORT_MOQ
}

namespace O3DTests
{
	void RegisterBuiltInConformanceProfiles()
	{
		using namespace O3DConformanceProfiles;

		{
			FO3DConformanceProfile Profile;
			Profile.MakeFixture = []() -> TUniquePtr<FO3DConformanceFixture> { return MakeUnique<FFakeFixture>(MakeUnique<FO3DFakeTransportScope>()); };
			Profile.Cases = SenderAndReceiverCases | EO3DConformanceCase::SendBackpressure | EO3DConformanceCase::RoundTripByteExact | ControlCases
				| EO3DConformanceCase::ConnectionStateConnected;
			Profile.BackpressurePayloadBytes = 64;
			Profile.BackpressureSendCount = 3;
			Profile.ExpectedCapabilities = GetFakeTransportCapabilities();
			Profile.bSelfRegistering = true;
			RegisterConformanceProfile(FakeName, Profile);
		}

		{
			FO3DConformanceProfile Profile;
			Profile.MakeFixture = []() -> TUniquePtr<FO3DConformanceFixture> { return MakeUnique<FLoopbackFixture>(); };
			Profile.Cases = SenderAndReceiverCases | EO3DConformanceCase::SendBackpressure | EO3DConformanceCase::RoundTripByteExact | ControlCases
				| EO3DConformanceCase::ConnectionStateConnected;
			Profile.BackpressurePayloadBytes = 64;
			Profile.BackpressureSendCount = 3;
			Profile.ExpectedCapabilities = MakeBaseCapabilities(EO3DDeliveryGuarantee::ReliableOrdered);
			RegisterConformanceProfile(LoopbackName, Profile);
		}

#if O3D_WITH_TRANSPORT_SOCKETS
		{
			FO3DConformanceProfile Profile;
			Profile.MakeFixture = []() -> TUniquePtr<FO3DConformanceFixture> { return MakeUnique<FTcpFixture>(); };
			Profile.Cases = SenderAndReceiverCases | EO3DConformanceCase::SendBackpressure | EO3DConformanceCase::RoundTripByteExact | ControlCases
				| EO3DConformanceCase::ConnectionStateConnected;
			Profile.BackpressurePayloadBytes = 128 * 1024;
			Profile.ExpectedCapabilities = MakeBaseCapabilities(EO3DDeliveryGuarantee::ReliableOrdered);
			Profile.ExpectedCapabilities.bBidirectional = true;
			Profile.ExpectedCapabilities.MaxPayloadBytes = 50 * 1024 * 1024; // the TCP frame header's limit
			Profile.bBackpressureNeedsPeer = true; // without a client every send is rejected before the queue
			RegisterConformanceProfile(TcpName, Profile);
		}
		{
			// UDP: no send queue to fill and no delivery guarantee, so no backpressure or round trip
			// (for frames or control; SocketsControlTests covers best-effort control delivery).
			FO3DConformanceProfile Profile;
			Profile.MakeFixture = []() -> TUniquePtr<FO3DConformanceFixture> { return MakeUnique<FUdpFixture>(); };
			Profile.Cases = SenderAndReceiverCases | EO3DConformanceCase::ControlRejectedWhenNotRunning | EO3DConformanceCase::ControlStopWhileSending;
			Profile.ExpectedCapabilities = MakeBaseCapabilities(EO3DDeliveryGuarantee::Unreliable);
			RegisterConformanceProfile(UdpName, Profile);
		}
#endif

#if O3D_WITH_TRANSPORT_NNG
		{
			// RestartAfterStop is left out: the NNG lifetime test notes that a closed listener can
			// linger and make an immediate re-listen on the same port fail. Follow-up for WP-A1.
			FO3DConformanceProfile Profile;
			Profile.MakeFixture = []() -> TUniquePtr<FO3DConformanceFixture> { return MakeUnique<FNngFixture>(); };
			Profile.Cases = (SenderAndReceiverCases & ~EO3DConformanceCase::LifecycleRestartAfterStop)
				| EO3DConformanceCase::SendBackpressure | EO3DConformanceCase::RoundTripByteExact | ControlCases
				| EO3DConformanceCase::ConnectionStateConnected;
			Profile.BackpressurePayloadBytes = 128 * 1024;
			// The fixture uses pub/sub, which ADR 0005 (iii) rates Unreliable (pair and push/pull are
			// ReliableOrdered; Open3DBroadcast.Shared.TransportCapabilities covers those).
			Profile.ExpectedCapabilities = MakeBaseCapabilities(EO3DDeliveryGuarantee::Unreliable);
			Profile.ControlStopCycles = 1; // a closed listener can linger (see above), so one sender per test
			RegisterConformanceProfile(NngName, Profile);
		}
#endif

#if O3D_WITH_TRANSPORT_MOQ
		{
			FO3DConformanceProfile Profile;
			Profile.MakeFixture = []() -> TUniquePtr<FO3DConformanceFixture> { return MakeUnique<FMoQFixture>(); };
			Profile.Cases = SenderAndReceiverCases | EO3DConformanceCase::SendBackpressure | EO3DConformanceCase::RoundTripByteExact
				| EO3DConformanceCase::LifetimeDestroyWithCallbacksInFlight | ControlCases | EO3DConformanceCase::ConnectionStateConnected;
			Profile.BackpressurePayloadBytes = 300 * 1024;
			// Unreliable in both delivery modes until ADR 0005 Q5 is answered.
			Profile.ExpectedCapabilities = MakeBaseCapabilities(EO3DDeliveryGuarantee::Unreliable);
			RegisterConformanceProfile(MoQName, Profile);
		}
#endif

		DeferConformanceProfile(WebRtcName, TEXT("WP-T2e: the WebRTC transport comes from the Open3DBroadcastWebRTC add-on; its offline profile (fake LiveKit FFI) belongs to an add-on test module that does not exist yet."));
	}

	void UnregisterBuiltInConformanceProfiles()
	{
		using namespace O3DConformanceProfiles;
		for (const FName& Name : { FakeName, LoopbackName, TcpName, UdpName, NngName, MoQName })
		{
			UnregisterConformanceProfile(Name);
		}
		UndeferConformanceProfile(WebRtcName);
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
