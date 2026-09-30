// Copyright (c) Open3DStream Contributors
//
// WP-S5: unit tests for the lifetime primitives in Open3DShared (ADR 0007 addendum).
// Covers TRF-1/TRB-10/TRB-12 (gate, hand-off queue), TRF-12 (FFI tokens), SHR-15 (per-stream
// decoders), TRB-11/TRF-10 (per-sink encoders) and SHR-10 (audio bus threading).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DTestHarness.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

#include "O3DAudioBus.h"
#include "O3DAudioFrameCodec.h"
#include "O3DEncodedPayloadQueue.h"
#include "O3DFfiContextRegistry.h"
#include "O3DLifetimeGate.h"
#include "O3DSinkAudioEncoder.h"

#include <atomic>

namespace
{
	/** Runs a TFunction on its own thread; joined by the destructor. */
	class FTestThread final : public FRunnable
	{
	public:
		explicit FTestThread(TFunction<void()> InBody)
			: Body(MoveTemp(InBody))
		{
			Thread = FRunnableThread::Create(this, TEXT("O3D_LifetimeTestThread"));
		}

		virtual ~FTestThread() override
		{
			Join();
		}

		void Join()
		{
			if (Thread)
			{
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
		}

		virtual uint32 Run() override
		{
			Body();
			return 0;
		}

	private:
		TFunction<void()> Body;
		FRunnableThread* Thread = nullptr;
	};

	struct FTestContext
	{
		std::atomic<int32> Hits{0};
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLifetimeGateCloseWaitsForReaderTest, "Open3DBroadcast.Shared.Lifetime.Gate.CloseWaitsForInFlightProducer", O3DB_TEST_FLAGS)
bool FO3DLifetimeGateCloseWaitsForReaderTest::RunTest(const FString& Parameters)
{
	FO3DLifetimeGate Gate;
	TestTrue(TEXT("Gate starts closed"), Gate.GetEpoch() == 0);
	{
		FO3DLifetimeGate::FReadScope Scope(Gate, 0);
		TestFalse(TEXT("Epoch 0 never enters"), static_cast<bool>(Scope));
	}

	const uint64 Epoch = Gate.Open();
	TestTrue(TEXT("Open returns a non-zero epoch"), Epoch != 0);
	TestTrue(TEXT("Open is idempotent while open"), Gate.Open() == Epoch);

	FEventRef Entered(EEventMode::ManualReset);
	FEventRef Release(EEventMode::ManualReset);
	std::atomic<bool> bReaderLeft{false};

	FTestThread Reader([&]()
	{
		FO3DLifetimeGate::FReadScope Scope(Gate, Epoch);
		Entered->Trigger();
		Release->Wait();
		bReaderLeft.store(true);
	});

	Entered->Wait();

	std::atomic<bool> bCloseReturned{false};
	FTestThread Closer([&]()
	{
		Gate.Close();
		bCloseReturned.store(true);
	});

	// Close() must not return while the reader is inside. New producers are refused as soon
	// as Close() has cleared the epoch, even before it returns.
	TestFalse(TEXT("Close has not returned while a producer is inside"), bCloseReturned.load() && !bReaderLeft.load());
	Release->Trigger();
	Closer.Join();
	Reader.Join();

	TestTrue(TEXT("Close returned"), bCloseReturned.load());
	TestTrue(TEXT("Reader left before Close returned"), bReaderLeft.load());
	{
		FO3DLifetimeGate::FReadScope Scope(Gate, Epoch);
		TestFalse(TEXT("Closed gate refuses the old epoch"), static_cast<bool>(Scope));
	}

	const uint64 NewEpoch = Gate.Open();
	TestTrue(TEXT("Reopen yields a new epoch"), NewEpoch != Epoch && NewEpoch != 0);
	{
		FO3DLifetimeGate::FReadScope Stale(Gate, Epoch);
		TestFalse(TEXT("A sink from the previous session stays dead after restart"), static_cast<bool>(Stale));
	}
	{
		FO3DLifetimeGate::FReadScope Fresh(Gate, NewEpoch);
		TestTrue(TEXT("A sink from the current session enters"), static_cast<bool>(Fresh));
	}
	Gate.Close();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLifetimeGateStressTest, "Open3DBroadcast.Shared.Lifetime.Gate.OpenCloseStress", O3DB_TEST_FLAGS)
bool FO3DLifetimeGateStressTest::RunTest(const FString& Parameters)
{
	// A producer thread hammers the gate while the game thread opens and closes it. Every time
	// a producer is inside, the protected value must be "alive"; Close() only then "destroys" it.
	FO3DLifetimeGate Gate;
	std::atomic<uint64> CurrentEpoch{0};
	std::atomic<int32> Alive{0};
	std::atomic<int32> Violations{0};
	std::atomic<bool> bStop{false};
	std::atomic<int64> Entries{0};

	FTestThread Producer([&]()
	{
		while (!bStop.load())
		{
			FO3DLifetimeGate::FReadScope Scope(Gate, CurrentEpoch.load());
			if (Scope)
			{
				if (Alive.load() != 1)
				{
					Violations.fetch_add(1);
				}
				Entries.fetch_add(1);
			}
			else
			{
				FPlatformProcess::YieldThread();
			}
		}
	});

	for (int32 Cycle = 0; Cycle < 1000; ++Cycle)
	{
		Alive.store(1);
		CurrentEpoch.store(Gate.Open());
		FPlatformProcess::YieldThread();
		Gate.Close();
		Alive.store(0); // "destroy" only after Close() returned
	}

	bStop.store(true);
	Producer.Join();

	TestEqual(TEXT("No producer observed a destroyed resource"), Violations.load(), 0);
	AddInfo(FString::Printf(TEXT("Producer entered the gate %lld times"), Entries.load()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DEncodedPayloadQueueTest, "Open3DBroadcast.Shared.Lifetime.PayloadQueue.MpscAccounting", O3DB_TEST_FLAGS)
bool FO3DEncodedPayloadQueueTest::RunTest(const FString& Parameters)
{
	{
		FO3DEncodedPayloadQueue Capped(8);
		TArray<uint8> A = {1, 2, 3, 4, 5};
		TArray<uint8> B = {6, 7, 8, 9, 10};
		TestTrue(TEXT("First payload fits"), Capped.Enqueue(MoveTemp(A)));
		TestFalse(TEXT("Second payload exceeds cap"), Capped.Enqueue(MoveTemp(B)));
		TestEqual(TEXT("Rejected payload left untouched"), B.Num(), 5);
		TestEqual(TEXT("Pending bytes"), static_cast<int64>(Capped.GetPendingBytes()), static_cast<int64>(5));
		TestTrue(TEXT("Consumer may put an item back past the cap"), Capped.Enqueue(MoveTemp(B), true));
		Capped.Empty();
		TestEqual(TEXT("Empty resets accounting"), static_cast<int64>(Capped.GetPendingBytes()), static_cast<int64>(0));
		TArray<uint8> Empty;
		TestFalse(TEXT("Empty payload rejected"), Capped.Enqueue(MoveTemp(Empty)));
	}

	// Four producers, one consumer; accounting must return to zero with nothing lost.
	constexpr int32 NumProducers = 4;
	constexpr int32 PerProducer = 5000;
	FO3DEncodedPayloadQueue Queue(0);
	std::atomic<int32> ProducersDone{0};
	int64 Consumed = 0;
	int64 ConsumedBytes = 0;

	TArray<TUniquePtr<FTestThread>> Producers;
	for (int32 P = 0; P < NumProducers; ++P)
	{
		Producers.Add(MakeUnique<FTestThread>([&Queue, &ProducersDone, P]()
		{
			for (int32 I = 0; I < PerProducer; ++I)
			{
				TArray<uint8> Bytes;
				Bytes.Init(static_cast<uint8>(P), 1 + (I % 7));
				Queue.Enqueue(MoveTemp(Bytes));
			}
			ProducersDone.fetch_add(1);
		}));
	}

	TArray<uint8> Out;
	while (ProducersDone.load() < NumProducers || Queue.GetPendingBytes() > 0)
	{
		if (Queue.Dequeue(Out))
		{
			++Consumed;
			ConsumedBytes += Out.Num();
		}
		else
		{
			Queue.WaitForWork(1);
		}
	}
	for (TUniquePtr<FTestThread>& Producer : Producers)
	{
		Producer->Join();
	}
	while (Queue.Dequeue(Out))
	{
		++Consumed;
		ConsumedBytes += Out.Num();
	}

	int64 ExpectedBytes = 0;
	for (int32 I = 0; I < PerProducer; ++I)
	{
		ExpectedBytes += 1 + (I % 7);
	}
	ExpectedBytes *= NumProducers;

	TestEqual(TEXT("Every payload consumed"), Consumed, static_cast<int64>(NumProducers * PerProducer));
	TestEqual(TEXT("Every byte consumed"), ConsumedBytes, ExpectedBytes);
	TestEqual(TEXT("Pending bytes back to zero"), static_cast<int64>(Queue.GetPendingBytes()), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DFfiContextRegistryTest, "Open3DBroadcast.Shared.Lifetime.FfiContextRegistry.StaleTokens", O3DB_TEST_FLAGS)
bool FO3DFfiContextRegistryTest::RunTest(const FString& Parameters)
{
	TO3DFfiContextRegistry<FTestContext> Registry;
	TestFalse(TEXT("Null token resolves to nothing"), Registry.Resolve(nullptr).IsValid());
	TestFalse(TEXT("Unknown token resolves to nothing"), Registry.Resolve(reinterpret_cast<void*>(static_cast<UPTRINT>(12345))).IsValid());

	void* Token = nullptr;
	{
		TSharedRef<FTestContext, ESPMode::ThreadSafe> Context = MakeShared<FTestContext, ESPMode::ThreadSafe>();
		Token = Registry.Register(Context);
		TestTrue(TEXT("Registered token resolves"), Registry.Resolve(Token).IsValid());
	}
	TestFalse(TEXT("Token of a destroyed context resolves to nothing"), Registry.Resolve(Token).IsValid());
	Registry.Unregister(Token);

	TSharedRef<FTestContext, ESPMode::ThreadSafe> Live = MakeShared<FTestContext, ESPMode::ThreadSafe>();
	void* LiveToken = Registry.Register(Live);
	TestTrue(TEXT("Tokens are never reused"), LiveToken != Token);
	Registry.Unregister(LiveToken);
	TestFalse(TEXT("Unregistered token resolves to nothing while the context lives"), Registry.Resolve(LiveToken).IsValid());

	// A fake FFI thread fires callbacks with the token while the owner unregisters and
	// destroys the context. The callback must either resolve a live context or nothing.
	for (int32 Cycle = 0; Cycle < 200; ++Cycle)
	{
		TSharedPtr<FTestContext, ESPMode::ThreadSafe> Context = MakeShared<FTestContext, ESPMode::ThreadSafe>();
		void* CycleToken = Registry.Register(Context.ToSharedRef());
		std::atomic<bool> bStop{false};
		FTestThread FfiThread([&Registry, CycleToken, &bStop]()
		{
			while (!bStop.load())
			{
				if (TSharedPtr<FTestContext, ESPMode::ThreadSafe> Resolved = Registry.Resolve(CycleToken))
				{
					Resolved->Hits.fetch_add(1);
				}
			}
		});
		FPlatformProcess::YieldThread();
		Registry.Unregister(CycleToken);
		Context.Reset();
		bStop.store(true);
		FfiThread.Join();
	}
	TestEqual(TEXT("Registry empty after stress"), Registry.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DMultiStreamDecoderTest, "Open3DBroadcast.Shared.Lifetime.Decoder.OnePerStream", O3DB_TEST_FLAGS)
bool FO3DMultiStreamDecoderTest::RunTest(const FString& Parameters)
{
	O3DAudio::FMultiStreamFrameDecoder Decoder(2);
	const int16 Samples[2] = {100, -100};
	TArray<int16> Out;

	O3DS::FAudioFrameMeta A;
	A.SourceGuid = FGuid::NewGuid();
	A.StreamLabel = TEXT("alice");
	O3DS::FAudioFrameMeta B = A;
	B.StreamLabel = TEXT("bob");
	O3DS::FAudioFrameMeta C;
	C.SourceGuid = FGuid::NewGuid();
	C.StreamLabel = TEXT("alice");

	const uint8* Payload = reinterpret_cast<const uint8*>(Samples);
	TestTrue(TEXT("Decode A"), Decoder.Decode(O3DS::EUnifiedCodec::PCM16, A, Payload, sizeof(Samples), Out));
	TestTrue(TEXT("Decode B"), Decoder.Decode(O3DS::EUnifiedCodec::PCM16, B, Payload, sizeof(Samples), Out));
	TestEqual(TEXT("Two labels from one source get two decoders"), Decoder.GetNumStreams(), 2);
	TestTrue(TEXT("Decode C"), Decoder.Decode(O3DS::EUnifiedCodec::PCM16, C, Payload, sizeof(Samples), Out));
	TestEqual(TEXT("Same label from another source is another stream; LRU cap holds"), Decoder.GetNumStreams(), 2);
	TestEqual(TEXT("PCM16 passthrough"), Out.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSinkAudioEncoderTest, "Open3DBroadcast.Shared.Lifetime.Encoder.SnapshotAndLabels", O3DB_TEST_FLAGS)
bool FO3DSinkAudioEncoderTest::RunTest(const FString& Parameters)
{
	FO3DSinkAudioEncoder::FSettings Settings;
	Settings.Config.bEnableAudio = true;
	Settings.Config.SampleRate = 48000;
	Settings.Config.NumChannels = 1;
	Settings.DefaultStreamLabel = TEXT("default_label");
	Settings.DefaultSubject = TEXT("default_subject");
	Settings.SourceGuid = FGuid::NewGuid();
	Settings.MaxStreams = 2;
	FO3DSinkAudioEncoder Encoder(Settings);

	const float Pcm[4] = {0.5f, -0.5f, 1.5f, -1.5f};
	TArray<O3DAudio::FEncodedFrame> Frames;
	TestTrue(TEXT("Encode label A"), Encoder.Encode(TEXT("a"), FString(), Pcm, 4, 1, 48000, 1.0, Frames));
	if (!TestEqual(TEXT("PCM16 gives one frame per buffer"), Frames.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("Snapshot SourceGuid stamped"), Frames[0].Meta.SourceGuid == Settings.SourceGuid);
	TestEqual(TEXT("Default subject used"), Frames[0].Meta.SubjectName, FString(TEXT("default_subject")));
	TestEqual(TEXT("Label kept"), Frames[0].Meta.StreamLabel, FString(TEXT("a")));
	TestEqual(TEXT("PCM16 bytes"), Frames[0].Encoded.Num(), 8);

	TestTrue(TEXT("Encode label B with subject"), Encoder.Encode(TEXT("b"), TEXT("hero"), Pcm, 4, 1, 48000, 2.0, Frames));
	TestTrue(TEXT("Subject override used"), Frames.Num() == 1 && Frames[0].Meta.SubjectName == TEXT("hero"));
	TestTrue(TEXT("Encode label C evicts the oldest encoder"), Encoder.Encode(TEXT("c"), FString(), Pcm, 4, 1, 48000, 3.0, Frames));

	// Two threads, two labels, one sink: must not corrupt each other (TRF-40/TRF-10).
	std::atomic<int32> Failures{0};
	{
		FTestThread T1([&]() { TArray<O3DAudio::FEncodedFrame> F; for (int32 I = 0; I < 2000; ++I) { if (!Encoder.Encode(TEXT("x"), FString(), Pcm, 4, 1, 48000, I, F) || F.Num() != 1 || F[0].Meta.StreamLabel != TEXT("x")) { Failures.fetch_add(1); } } });
		FTestThread T2([&]() { TArray<O3DAudio::FEncodedFrame> F; for (int32 I = 0; I < 2000; ++I) { if (!Encoder.Encode(TEXT("y"), FString(), Pcm, 4, 1, 48000, I, F) || F.Num() != 1 || F[0].Meta.StreamLabel != TEXT("y")) { Failures.fetch_add(1); } } });
	}
	TestEqual(TEXT("Concurrent encodes on one sink stay consistent"), Failures.load(), 0);

	int16 Converted[4];
	O3DAudio::ConvertFloatToPcm16(Pcm, 4, Converted);
	TestEqual(TEXT("Round to nearest"), Converted[0], static_cast<int16>(FMath::RoundToInt(0.5f * 32767.0f)));
	TestEqual(TEXT("Clamp high"), Converted[2], static_cast<int16>(32767));
	TestEqual(TEXT("Clamp low"), Converted[3], static_cast<int16>(-32767));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioBusGameThreadTest, "Open3DBroadcast.Shared.Lifetime.AudioBus.GameThreadPublish", O3DB_TEST_FLAGS)
bool FO3DAudioBusGameThreadTest::RunTest(const FString& Parameters)
{
	// SHR-10: the bus is game-thread-only. Publishing with no listener is a no-op; with a
	// listener it delivers synchronously on the game thread.
	O3DS::FAudioFrameMeta Meta;
	Meta.StreamLabel = TEXT("bus_test");
	const uint8 Bytes[4] = {1, 2, 3, 4};

	int32 Calls = 0;
	FDelegateHandle Handle = FO3DAudioBus::OnPcm16().AddLambda([&Calls](const O3DS::FAudioFrameMeta& InMeta, TConstArrayView<uint8> Data)
	{
		if (InMeta.StreamLabel == TEXT("bus_test") && Data.Num() == 4)
		{
			++Calls;
		}
	});
	FO3DAudioBus::PublishPcm16(Meta, Bytes, 4);
	FO3DAudioBus::OnPcm16().Remove(Handle);
	FO3DAudioBus::PublishPcm16(Meta, Bytes, 4);

	TestEqual(TEXT("Listener called exactly once"), Calls, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
