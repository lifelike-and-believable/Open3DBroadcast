// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

// Test-only helpers for the WP-S5 lifetime stress tests. Compiled only with dev automation
// tests. Header-only. It lives in Open3DShared, not in Open3DBroadcastTests, because the WebRTC
// add-on's tests (Open3DBroadcastWebRTC plugin, WP-F11) live in its Runtime module and include
// it, and the Fab package has no Open3DBroadcastTests to depend on (ADR 0006). It uses only
// Open3DShared types; WP-A1 PR 5a moved it here from Open3DSender, so the add-on needs no
// Open3DSender dependency.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "Transport/O3DSenderInterface.h"

#include <atomic>

namespace O3DLifetimeTest
{
	/** Number of start/stop cycles each transport stress test runs (WP-S5 acceptance). */
	inline constexpr int32 StressCycles = 1000;

	/**
	 * A thread that behaves like the audio render thread: it keeps calling SubmitPcm on whatever
	 * sink is currently published, and may still hold a sink after the test has stopped and
	 * destroyed the sender that produced it. It never sleeps; it yields while no sink is bound.
	 */
	class FFakeAudioThread final : public FRunnable
	{
	public:
		explicit FFakeAudioThread(int32 InFramesPerBuffer = 480, int32 InNumChannels = 1, int32 InSampleRate = 48000)
			: FramesPerBuffer(InFramesPerBuffer)
			, NumChannels(InNumChannels)
			, SampleRate(InSampleRate)
		{
			Samples.SetNumUninitialized(FramesPerBuffer * NumChannels);
			for (int32 Index = 0; Index < Samples.Num(); ++Index)
			{
				Samples[Index] = 0.25f * FMath::Sin(static_cast<float>(Index) * 0.05f);
			}
			Thread = FRunnableThread::Create(this, TEXT("O3D_FakeAudioThread"));
		}

		virtual ~FFakeAudioThread() override
		{
			StopAndJoin();
		}

		/** Game thread: swap the sink the fake audio thread submits to (nullptr to idle). */
		void SetSink(const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& InSink)
		{
			FScopeLock Lock(&SinkMutex);
			Sink = InSink;
		}

		void StopAndJoin()
		{
			bStop.store(true);
			if (Thread)
			{
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
		}

		int64 GetSubmitted() const { return Submitted.load(); }
		int64 GetAccepted() const { return Accepted.load(); }
		bool IsRunning() const { return Thread != nullptr; }

		virtual uint32 Run() override
		{
			double Clock = 0.0;
			while (!bStop.load())
			{
				TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Local;
				{
					FScopeLock Lock(&SinkMutex);
					Local = Sink;
				}

				if (!Local.IsValid())
				{
					FPlatformProcess::YieldThread();
					continue;
				}

				Clock += static_cast<double>(FramesPerBuffer) / static_cast<double>(SampleRate);
				if (Local->SubmitPcm(TEXT("lifetime"), Samples.GetData(), FramesPerBuffer, NumChannels, SampleRate, Clock))
				{
					Accepted.fetch_add(1);
				}
				Submitted.fetch_add(1);
				// Local may be the last reference to a sink whose sender is already gone;
				// dropping it here is part of what the stress test exercises.
			}
			return 0;
		}

	private:
		const int32 FramesPerBuffer;
		const int32 NumChannels;
		const int32 SampleRate;
		TArray<float> Samples;

		FCriticalSection SinkMutex;
		TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink;

		std::atomic<bool> bStop{false};
		std::atomic<int64> Submitted{0};
		std::atomic<int64> Accepted{0};
		FRunnableThread* Thread = nullptr;
	};

	/**
	 * Result of one sender lifetime stress run. A stale sink must reject PCM once its sender
	 * has stopped; that check is deterministic, unlike the race the stress itself provokes.
	 */
	struct FStressResult
	{
		int32 CyclesRun = 0;
		int32 StartFailures = 0;
		int32 SinksCreated = 0;
		int32 StaleSinkAccepted = 0;
		int64 Submitted = 0;
		int64 Accepted = 0;
	};

	/**
	 * Start and stop a sender StressCycles times while a fake audio thread submits PCM.
	 * MakeSender creates a new sender each cycle (a registry factory or a Testing header's create
	 * function); MakeConfig returns the config for a cycle. bStart=false runs
	 * Initialize/CreateAudioSink/Stop only (for transports that need a network peer or an FFI
	 * connection to start).
	 */
	template <typename TMakeSender, typename TMakeConfig>
	FStressResult RunSenderStressWith(TMakeSender&& MakeSender, TMakeConfig&& MakeConfig, bool bStart, int32 Cycles = StressCycles)
	{
		FStressResult Result;
		FFakeAudioThread AudioThread;

		const float Probe[4] = {0.1f, -0.1f, 0.2f, -0.2f};
		TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> StaleSink;

		for (int32 Cycle = 0; Cycle < Cycles; ++Cycle)
		{
			++Result.CyclesRun;
			TSharedPtr<IOpen3DSender> Sender = MakeSender();
			const FO3DTransportConfig Config = MakeConfig(Cycle);
			if (!Sender.IsValid())
			{
				++Result.StartFailures;
				continue;
			}
			if (!Sender->Initialize(Config) || (bStart && !Sender->Start()))
			{
				++Result.StartFailures;
				Sender->Stop();
				continue;
			}

			TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink = Sender->CreateAudioSink(Config.Audio);
			if (Sink.IsValid())
			{
				++Result.SinksCreated;
			}
			AudioThread.SetSink(Sink);

			// A few ticks so workers and accept loops run while audio is flowing.
			for (int32 TickIndex = 0; TickIndex < 3; ++TickIndex)
			{
				Sender->Tick(0.0f);
				FPlatformProcess::YieldThread();
			}

			// Alternate between clearing the fake thread's sink before Stop and leaving it
			// bound, so half the cycles stop and destroy the sender while the audio thread
			// still holds (and may be inside) its sink.
			if ((Cycle & 1) == 0)
			{
				AudioThread.SetSink(nullptr);
			}

			Sender->Stop();
			Sender.Reset();

			if (Sink.IsValid() && Sink->SubmitPcm(TEXT("stale"), Probe, 4, 1, 48000, 0.0))
			{
				++Result.StaleSinkAccepted;
			}

			AudioThread.SetSink(Sink); // keep feeding the dead sink until the next cycle
			StaleSink = Sink;
		}

		AudioThread.SetSink(nullptr);
		AudioThread.StopAndJoin();
		StaleSink.Reset();

		Result.Submitted = AudioThread.GetSubmitted();
		Result.Accepted = AudioThread.GetAccepted();
		return Result;
	}

	/** RunSenderStressWith for a concrete sender type the caller can construct directly. */
	template <typename TSender, typename TMakeConfig>
	FStressResult RunSenderStress(TMakeConfig&& MakeConfig, bool bStart, int32 Cycles = StressCycles)
	{
		return RunSenderStressWith([]() -> TSharedPtr<IOpen3DSender> { return MakeShared<TSender>(); },
			Forward<TMakeConfig>(MakeConfig), bStart, Cycles);
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
