// Copyright (c) Open3DStream Contributors
//
// Transport conformance suite (ADR 0006 §4, WP-T2; replaces the SHR-5 placeholders
// Open3DBroadcast.Generic.Concurrency.MultipleSends, .Performance.Backpressure and
// .Stats.ConsistencyUnderLoad). One test per registered transport and case:
// Open3DBroadcast.Conformance.<Transport>.<Case>. Profiles are registered by
// O3DConformanceProfiles.cpp (built-in transports) and, once it has a test module, by the WebRTC
// add-on plugin (WP-T2e).
//
// Everything is offline: 127.0.0.1 sockets on ephemeral ports or a fake FFI table. Waits poll a
// condition against a deadline and never sleep.

#include "O3DConformance.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/AutomationTest.h"
#include "O3DReceiverRegistry.h"
#include "O3DSenderRegistry.h"
#include "O3DTestFakes.h"
#include "O3DTestHarness.h"

#include <atomic>

namespace O3DConformanceSuite
{
	constexpr int32 NumSendThreads = 4;
	constexpr int32 SendsPerThread = 250;
	constexpr int32 RoundTripFrames = 20;
	/** A send that takes longer than this counts as blocking (ADR 0007: SendSerialized never blocks). */
	constexpr double MaxNonBlockingSendSeconds = 0.5;
	constexpr double ProbeIntervalSeconds = 0.05;

	const TCHAR* const CommandSeparator = TEXT("|");

	/** Sends one payload repeatedly from its own thread and counts the results. */
	class FSendWorker final : public FRunnable
	{
	public:
		FSendWorker(IOpen3DSender& InSender, TArray<uint8> InPayload, int32 InSends)
			: Sender(InSender)
			, Payload(MoveTemp(InPayload))
			, Sends(InSends)
		{
		}

		virtual uint32 Run() override
		{
			for (int32 Index = 0; Index < Sends; ++Index)
			{
				if (Sender.SendSerialized(Payload.GetData(), Payload.Num(), TEXT("conformance"), static_cast<double>(Index)))
				{
					Accepted.fetch_add(1);
				}
				else
				{
					Rejected.fetch_add(1);
				}
			}
			bDone.store(true);
			return 0;
		}

		IOpen3DSender& Sender;
		const TArray<uint8> Payload;
		const int32 Sends;
		std::atomic<int32> Accepted{0};
		std::atomic<int32> Rejected{0};
		std::atomic<bool> bDone{false};
	};

	bool IsMonotonic(const FO3DTransportStats& Before, const FO3DTransportStats& After)
	{
		return After.FramesSent >= Before.FramesSent
			&& After.FramesReceived >= Before.FramesReceived
			&& After.BytesSent >= Before.BytesSent
			&& After.BytesReceived >= Before.BytesReceived
			&& After.DroppedFrames >= Before.DroppedFrames;
	}

	FString DescribeStats(const FO3DTransportStats& Stats)
	{
		return FString::Printf(TEXT("sent=%lld recv=%lld bytesSent=%lld bytesRecv=%lld dropped=%lld"),
			Stats.FramesSent, Stats.FramesReceived, Stats.BytesSent, Stats.BytesReceived, Stats.DroppedFrames);
	}

	TArray<uint8> MakePayload()
	{
		const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("ConformanceActor"), 1);
		return Frames.Num() > 0 ? Frames[0] : TArray<uint8>();
	}

	TArray<uint8> MakePayloadOfSize(int32 Size)
	{
		TArray<uint8> Payload;
		Payload.SetNumUninitialized(FMath::Max(Size, 1));
		for (int32 Index = 0; Index < Payload.Num(); ++Index)
		{
			Payload[Index] = static_cast<uint8>((Index * 31 + 7) & 0xFF);
		}
		return Payload;
	}

	/** A started sender and receiver on one endpoint that have exchanged at least one frame. */
	struct FConnectedPair
	{
		TSharedPtr<IOpen3DSender> Sender;
		TSharedPtr<IOpen3DReceiver> Receiver;
		TSharedPtr<FO3DRecordingFrameConsumer> Consumer;
		TArray<uint8> Probe;
		FO3DConformanceFixture* Fixture = nullptr;

		void Pump() const
		{
			Fixture->Pump();
			if (Receiver.IsValid())
			{
				Receiver->Poll();
			}
			if (Sender.IsValid())
			{
				Sender->Tick(0.0f);
			}
		}

		~FConnectedPair()
		{
			if (Receiver.IsValid())
			{
				Receiver->Stop();
			}
			if (Sender.IsValid())
			{
				Sender->Stop();
			}
			if (Fixture)
			{
				Fixture->Pump();
			}
		}
	};

	/** Starts both sides and sends probe frames until one arrives or the profile's timeout passes. */
	bool Connect(FAutomationTestBase& Test, const FO3DConformanceProfile& Profile, FO3DConformanceFixture& Fixture, const FO3DTransportConfig& SenderConfig, FConnectedPair& Pair)
	{
		Pair.Fixture = &Fixture;
		Pair.Sender = Fixture.CreateSender();
		Pair.Receiver = Fixture.CreateReceiver();
		Pair.Consumer = MakeShared<FO3DRecordingFrameConsumer>();
		if (!Test.TestTrue(TEXT("Sender created"), Pair.Sender.IsValid()) || !Test.TestTrue(TEXT("Receiver created"), Pair.Receiver.IsValid()))
		{
			return false;
		}

		const TArray<TArray<uint8>> ProbeFrames = O3DTests::MakeRecordedFrames(TEXT("__o3d_conformance_probe__"), 1);
		if (!Test.TestEqual(TEXT("Probe frame built"), ProbeFrames.Num(), 1))
		{
			return false;
		}
		Pair.Probe = ProbeFrames[0];

		if (!Test.TestTrue(TEXT("Sender initializes"), Pair.Sender->Initialize(SenderConfig))
			|| !Test.TestTrue(TEXT("Receiver initializes"), Pair.Receiver->Initialize(Fixture.MakeReceiverConfig())))
		{
			return false;
		}
		Pair.Receiver->SetConsumer(Pair.Consumer);
		if (!Test.TestTrue(TEXT("Sender starts"), Pair.Sender->Start()) || !Test.TestTrue(TEXT("Receiver starts"), Pair.Receiver->Start()))
		{
			return false;
		}

		double NextProbe = 0.0;
		const bool bConnected = O3DTests::PollUntil(Profile.ConnectTimeoutSeconds,
			[&Pair]() { return Pair.Consumer->Num() > 0; },
			[&Pair, &NextProbe]()
			{
				Pair.Pump();
				const double Now = FPlatformTime::Seconds();
				if (Now >= NextProbe)
				{
					NextProbe = Now + ProbeIntervalSeconds;
					Pair.Sender->SendSerialized(Pair.Probe.GetData(), Pair.Probe.Num(), TEXT("probe"), Now);
				}
			});
		return Test.TestTrue(TEXT("Sender and receiver exchange a probe frame"), bConnected);
	}

	// ── Cases ────────────────────────────────────────────────────────────────────────────

	bool RunStopIsIdempotent(FAutomationTestBase& Test, const FO3DConformanceProfile&, FO3DConformanceFixture& Fixture)
	{
		{
			// Stop on a sender that never started must be harmless.
			TSharedPtr<IOpen3DSender> Fresh = Fixture.CreateSender();
			if (!Test.TestTrue(TEXT("Sender created"), Fresh.IsValid()))
			{
				return false;
			}
			Fresh->Stop();
			Fresh->Stop();
		}

		TSharedPtr<IOpen3DSender> Sender = Fixture.CreateSender();
		if (!Test.TestTrue(TEXT("Sender created"), Sender.IsValid()))
		{
			return false;
		}
		Test.TestTrue(TEXT("Initialize"), Sender->Initialize(Fixture.MakeSenderConfig()));
		Test.TestTrue(TEXT("Start"), Sender->Start());
		Fixture.Pump();
		Sender->Stop();
		Sender->Stop();
		Fixture.Pump();

		const TArray<uint8> Payload = MakePayload();
		const FO3DTransportStats Before = Sender->GetStats();
		Test.TestFalse(TEXT("SendSerialized after Stop is rejected"), Sender->SendSerialized(Payload.GetData(), Payload.Num(), TEXT("stopped"), 0.0));
		Test.TestEqual(TEXT("No frame counted as sent after Stop"), Sender->GetStats().FramesSent, Before.FramesSent);
		Sender.Reset();
		Fixture.Pump();
		return true;
	}

	bool RunRestartAfterStop(FAutomationTestBase& Test, const FO3DConformanceProfile&, FO3DConformanceFixture& Fixture)
	{
		TSharedPtr<IOpen3DSender> Sender = Fixture.CreateSender();
		if (!Test.TestTrue(TEXT("Sender created"), Sender.IsValid()))
		{
			return false;
		}
		const FO3DTransportConfig Config = Fixture.MakeSenderConfig();
		Test.TestTrue(TEXT("Initialize"), Sender->Initialize(Config));
		Test.TestTrue(TEXT("Start"), Sender->Start());
		Fixture.Pump();
		Sender->Stop();
		Fixture.Pump();

		Test.TestTrue(TEXT("Start after Stop without Initialize"), Sender->Start());
		Fixture.Pump();
		Sender->Stop();
		Fixture.Pump();

		Test.TestTrue(TEXT("Initialize after Stop"), Sender->Initialize(Config));
		Test.TestTrue(TEXT("Start after re-Initialize"), Sender->Start());
		Fixture.Pump();
		Sender->Stop();
		Sender.Reset();
		Fixture.Pump();
		return true;
	}

	bool RunReceiverLifecycle(FAutomationTestBase& Test, const FO3DConformanceProfile&, FO3DConformanceFixture& Fixture)
	{
		TSharedPtr<IOpen3DReceiver> Receiver = Fixture.CreateReceiver();
		if (!Test.TestTrue(TEXT("Receiver created"), Receiver.IsValid()))
		{
			return false;
		}
		const TSharedPtr<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
		Receiver->Stop(); // before Initialize: harmless
		Test.TestTrue(TEXT("Initialize"), Receiver->Initialize(Fixture.MakeReceiverConfig()));
		Receiver->SetConsumer(Consumer);
		Test.TestEqual(TEXT("Poll before Start delivers nothing"), Receiver->Poll(), 0);
		Test.TestTrue(TEXT("Start"), Receiver->Start());
		Fixture.Pump();
		Receiver->Poll();
		Receiver->Stop();
		Receiver->Stop();
		Fixture.Pump();
		Test.TestEqual(TEXT("Poll after Stop delivers nothing"), Receiver->Poll(), 0);
		Test.TestEqual(TEXT("No frame reached the consumer without a sender"), Consumer->Num(), 0);
		Receiver.Reset();
		Fixture.Pump();
		return true;
	}

	bool RunSendRejectedWhenNotRunning(FAutomationTestBase& Test, const FO3DConformanceProfile&, FO3DConformanceFixture& Fixture)
	{
		TSharedPtr<IOpen3DSender> Sender = Fixture.CreateSender();
		if (!Test.TestTrue(TEXT("Sender created"), Sender.IsValid()))
		{
			return false;
		}
		const TArray<uint8> Payload = MakePayload();
		Test.TestFalse(TEXT("SendSerialized before Initialize is rejected"), Sender->SendSerialized(Payload.GetData(), Payload.Num(), TEXT("early"), 0.0));
		Test.TestTrue(TEXT("Initialize"), Sender->Initialize(Fixture.MakeSenderConfig()));
		Test.TestFalse(TEXT("SendSerialized before Start is rejected"), Sender->SendSerialized(Payload.GetData(), Payload.Num(), TEXT("early"), 0.0));
		Test.TestEqual(TEXT("No frame counted as sent"), Sender->GetStats().FramesSent, static_cast<int64>(0));
		Sender->Stop();
		Sender.Reset();
		Fixture.Pump();
		return true;
	}

	bool RunBackpressure(FAutomationTestBase& Test, const FO3DConformanceProfile& Profile, FO3DConformanceFixture& Fixture)
	{
		if (!Test.TestTrue(TEXT("Profile sets a backpressure payload size"), Profile.BackpressurePayloadBytes > 0))
		{
			return false;
		}

		FConnectedPair Pair;
		TSharedPtr<IOpen3DSender> Sender;
		if (Profile.bBackpressureNeedsPeer)
		{
			if (!Connect(Test, Profile, Fixture, Fixture.MakeBackpressureSenderConfig(), Pair))
			{
				return false;
			}
			Sender = Pair.Sender;
		}
		else
		{
			Sender = Fixture.CreateSender();
			if (!Test.TestTrue(TEXT("Sender created"), Sender.IsValid())
				|| !Test.TestTrue(TEXT("Initialize"), Sender->Initialize(Fixture.MakeBackpressureSenderConfig()))
				|| !Test.TestTrue(TEXT("Start"), Sender->Start()))
			{
				return false;
			}
			Fixture.Pump();
		}

		const TArray<uint8> Payload = MakePayloadOfSize(Profile.BackpressurePayloadBytes);
		const FO3DTransportStats Before = Sender->GetStats();
		int32 Rejected = 0;
		double SlowestSend = 0.0;
		for (int32 Index = 0; Index < FMath::Max(1, Profile.BackpressureSendCount); ++Index)
		{
			const double Start = FPlatformTime::Seconds();
			if (!Sender->SendSerialized(Payload.GetData(), Payload.Num(), TEXT("backpressure"), Start))
			{
				++Rejected;
			}
			SlowestSend = FMath::Max(SlowestSend, FPlatformTime::Seconds() - Start);
		}
		const FO3DTransportStats After = Sender->GetStats();

		Test.TestTrue(TEXT("At least one send was dropped"), Rejected > 0);
		Test.TestTrue(*FString::Printf(TEXT("DroppedFrames grew by the drops (%lld -> %lld, %d rejected)"), Before.DroppedFrames, After.DroppedFrames, Rejected),
			After.DroppedFrames - Before.DroppedFrames >= Rejected && Rejected > 0);
		Test.TestTrue(*FString::Printf(TEXT("No send blocked (slowest %.3f s)"), SlowestSend), SlowestSend < MaxNonBlockingSendSeconds);

		if (!Profile.bBackpressureNeedsPeer)
		{
			Sender->Stop();
			Sender.Reset();
			Fixture.Pump();
		}
		return true;
	}

	/** Four threads send while the game thread samples GetStats; returns false if a sample went backwards. */
	bool RunConcurrentSends(FAutomationTestBase& Test, FO3DConformanceFixture& Fixture, bool bCheckMonotonic)
	{
		TSharedPtr<IOpen3DSender> Sender = Fixture.CreateSender();
		if (!Test.TestTrue(TEXT("Sender created"), Sender.IsValid())
			|| !Test.TestTrue(TEXT("Initialize"), Sender->Initialize(Fixture.MakeSenderConfig()))
			|| !Test.TestTrue(TEXT("Start"), Sender->Start()))
		{
			return false;
		}
		Fixture.Pump();

		const TArray<uint8> Payload = MakePayload();
		TArray<TUniquePtr<FSendWorker>> Workers;
		TArray<FRunnableThread*> Threads;
		for (int32 Index = 0; Index < NumSendThreads; ++Index)
		{
			Workers.Add(MakeUnique<FSendWorker>(*Sender, Payload, SendsPerThread));
			FRunnableThread* Thread = FRunnableThread::Create(Workers.Last().Get(), *FString::Printf(TEXT("O3DConformanceSend_%d"), Index));
			if (!Thread)
			{
				Test.AddError(TEXT("Could not create a send thread"));
				Workers.Last()->bDone.store(true);
			}
			Threads.Add(Thread);
		}

		int32 Samples = 0;
		int32 Regressions = 0;
		FString FirstRegression;
		FO3DTransportStats Previous = Sender->GetStats();
		auto AllDone = [&Workers]()
		{
			for (const TUniquePtr<FSendWorker>& Worker : Workers)
			{
				if (!Worker->bDone.load())
				{
					return false;
				}
			}
			return true;
		};
		while (!AllDone())
		{
			Fixture.Pump();
			Sender->Tick(0.0f);
			const FO3DTransportStats Current = Sender->GetStats();
			++Samples;
			if (!IsMonotonic(Previous, Current))
			{
				if (Regressions++ == 0)
				{
					FirstRegression = FString::Printf(TEXT("%s -> %s"), *DescribeStats(Previous), *DescribeStats(Current));
				}
			}
			Previous = Current;
			FPlatformProcess::YieldThread();
		}

		int32 Accepted = 0;
		int32 Rejected = 0;
		for (int32 Index = 0; Index < Threads.Num(); ++Index)
		{
			if (Threads[Index])
			{
				Threads[Index]->WaitForCompletion();
				delete Threads[Index];
			}
			Accepted += Workers[Index]->Accepted.load();
			Rejected += Workers[Index]->Rejected.load();
		}

		const FO3DTransportStats Final = Sender->GetStats();
		Test.TestEqual(TEXT("Every send returned"), Accepted + Rejected, NumSendThreads * SendsPerThread);
		Test.TestTrue(TEXT("Final stats are not below the last sample"), IsMonotonic(Previous, Final));
		if (bCheckMonotonic)
		{
			Test.TestEqual(*FString::Printf(TEXT("Stats never went backwards over %d samples (first: %s)"), Samples, *FirstRegression), Regressions, 0);
		}
		Test.AddInfo(FString::Printf(TEXT("%d accepted, %d rejected; final %s"), Accepted, Rejected, *DescribeStats(Final)));

		Sender->Stop();
		Sender.Reset();
		Fixture.Pump();
		return true;
	}

	bool RunRoundTrip(FAutomationTestBase& Test, const FO3DConformanceProfile& Profile, FO3DConformanceFixture& Fixture)
	{
		FConnectedPair Pair;
		if (!Connect(Test, Profile, Fixture, Fixture.MakeSenderConfig(), Pair))
		{
			return false;
		}

		const TArray<TArray<uint8>> Recorded = O3DTests::MakeRecordedFrames(TEXT("ConformanceActor"), RoundTripFrames);
		if (!Test.TestEqual(TEXT("Recorded frames built"), Recorded.Num(), RoundTripFrames))
		{
			return false;
		}
		for (int32 Index = 0; Index < Recorded.Num(); ++Index)
		{
			// A transient full queue is backpressure, not loss: retry until accepted or the deadline.
			const TArray<uint8>& Frame = Recorded[Index];
			const bool bQueued = O3DTests::PollUntil(Profile.ConnectTimeoutSeconds,
				[&Pair, &Frame, Index]() { return Pair.Sender->SendSerialized(Frame.GetData(), Frame.Num(), TEXT("ConformanceActor"), static_cast<double>(Index)); },
				[&Pair]() { Pair.Pump(); });
			if (!Test.TestTrue(*FString::Printf(TEXT("Frame %d accepted"), Index), bQueued))
			{
				return false;
			}
		}

		auto CollectNonProbe = [&Pair]()
		{
			TArray<TArray<uint8>> Out;
			for (const TArray<uint8>& Frame : Pair.Consumer->GetFrames())
			{
				if (Frame != Pair.Probe)
				{
					Out.Add(Frame);
				}
			}
			return Out;
		};

		O3DTests::PollUntil(Profile.ConnectTimeoutSeconds,
			[&CollectNonProbe]() { return CollectNonProbe().Num() >= RoundTripFrames; },
			[&Pair]() { Pair.Pump(); });

		const TArray<TArray<uint8>> Received = CollectNonProbe();
		Test.TestEqual(TEXT("Every recorded frame arrived exactly once"), Received.Num(), RoundTripFrames);
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
		return true;
	}

	bool RunCase(FAutomationTestBase& Test, EO3DConformanceCase Case, const FO3DConformanceProfile& Profile, FO3DConformanceFixture& Fixture)
	{
		switch (Case)
		{
		case EO3DConformanceCase::LifecycleStopIsIdempotent: return RunStopIsIdempotent(Test, Profile, Fixture);
		case EO3DConformanceCase::LifecycleRestartAfterStop: return RunRestartAfterStop(Test, Profile, Fixture);
		case EO3DConformanceCase::ReceiverLifecycle: return RunReceiverLifecycle(Test, Profile, Fixture);
		case EO3DConformanceCase::SendRejectedWhenNotRunning: return RunSendRejectedWhenNotRunning(Test, Profile, Fixture);
		case EO3DConformanceCase::SendBackpressure: return RunBackpressure(Test, Profile, Fixture);
		case EO3DConformanceCase::SendConcurrent: return RunConcurrentSends(Test, Fixture, /*bCheckMonotonic=*/false);
		case EO3DConformanceCase::StatsMonotonic: return RunConcurrentSends(Test, Fixture, /*bCheckMonotonic=*/true);
		case EO3DConformanceCase::RoundTripByteExact: return RunRoundTrip(Test, Profile, Fixture);
		case EO3DConformanceCase::LifetimeDestroyWithCallbacksInFlight: return Fixture.RunDestroyWithCallbacksInFlight(Test);
		default:
			Test.AddError(TEXT("Unknown conformance case"));
			return false;
		}
	}

	/** Registered transport names plus self-registering profiles, without per-test fake names. */
	TArray<FName> GetTransportNames()
	{
		TSet<FName> Names;
		for (const FName& Name : O3DTransport::GetRegisteredSenders())
		{
			Names.Add(Name);
		}
		for (const FName& Name : O3DTransport::GetRegisteredReceivers())
		{
			Names.Add(Name);
		}
		for (const FName& Name : O3DTests::GetSelfRegisteringProfiles())
		{
			Names.Add(Name);
		}

		TArray<FName> Sorted;
		for (const FName& Name : Names)
		{
			if (!Name.ToString().StartsWith(FO3DFakeTransportScope::GetNamePrefix()))
			{
				Sorted.Add(Name);
			}
		}
		Sorted.Sort([](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });
		return Sorted;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FO3DConformanceSuite, "Open3DBroadcast.Conformance", O3DB_TEST_FLAGS)

void FO3DConformanceSuite::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	using namespace O3DConformanceSuite;

	for (const FName& Transport : GetTransportNames())
	{
		const FString TransportString = Transport.ToString();
		FO3DConformanceProfile Profile;
		if (O3DTests::FindConformanceProfile(Transport, Profile))
		{
			for (EO3DConformanceCase Case : O3DTests::GetAllConformanceCases())
			{
				if (EnumHasAnyFlags(Profile.Cases, Case))
				{
					const FString CaseName = O3DTests::GetConformanceCaseName(Case);
					OutBeautifiedNames.Add(FString::Printf(TEXT("%s.%s"), *TransportString, *CaseName));
					OutTestCommands.Add(FString::Printf(TEXT("%s%s%s"), *TransportString, CommandSeparator, *CaseName));
				}
			}
		}
		else if (!O3DTests::IsConformanceProfileDeferred(Transport))
		{
			OutBeautifiedNames.Add(FString::Printf(TEXT("%s.HasProfile"), *TransportString));
			OutTestCommands.Add(FString::Printf(TEXT("%s%sHasProfile"), *TransportString, CommandSeparator));
		}
	}
}

bool FO3DConformanceSuite::RunTest(const FString& Parameters)
{
	using namespace O3DConformanceSuite;

	FString TransportString;
	FString CaseName;
	if (!Parameters.Split(CommandSeparator, &TransportString, &CaseName))
	{
		AddError(FString::Printf(TEXT("Malformed conformance command '%s'"), *Parameters));
		return false;
	}
	const FName Transport(*TransportString);

	FO3DConformanceProfile Profile;
	if (!O3DTests::FindConformanceProfile(Transport, Profile))
	{
		AddError(FString::Printf(TEXT("Transport '%s' is registered but has no conformance profile. Register one with "
			"O3DTests::RegisterConformanceProfile (ADR 0006 §4), or record a deferral with O3DTests::DeferConformanceProfile."), *TransportString));
		return false;
	}

	EO3DConformanceCase Case = EO3DConformanceCase::None;
	for (EO3DConformanceCase Candidate : O3DTests::GetAllConformanceCases())
	{
		if (O3DTests::GetConformanceCaseName(Candidate) == CaseName)
		{
			Case = Candidate;
		}
	}
	if (Case == EO3DConformanceCase::None || !Profile.MakeFixture)
	{
		AddError(FString::Printf(TEXT("Conformance case '%s' is unknown or the profile for '%s' has no fixture"), *CaseName, *TransportString));
		return false;
	}

	TUniquePtr<FO3DConformanceFixture> Fixture = Profile.MakeFixture();
	if (!TestTrue(TEXT("Fixture created"), Fixture.IsValid()))
	{
		return false;
	}
	Fixture->AddExpectedMessages(*this, Case);
	RunCase(*this, Case, Profile, *Fixture);
	Fixture.Reset();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
