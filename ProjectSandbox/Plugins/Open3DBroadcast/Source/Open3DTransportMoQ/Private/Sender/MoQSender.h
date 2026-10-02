// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DSenderAudioSinkBase.h"
#include "Transport/O3DTransportWorker.h"
#include "Shared/MoQHelpers.h"
#include "O3DPerformanceMetrics.h"
#include "MoQFfiApi.h"
THIRD_PARTY_INCLUDES_START
#include "moq_ffi.h"
THIRD_PARTY_INCLUDES_END

#include <atomic>

class FMoQSessionWrapper;
class FMoQPublisherHandle;

DECLARE_LOG_CATEGORY_EXTERN(LogO3DMoQSender, Log, All);

/**
 * MoQ Transport Sender Implementation
 *
 * Connects to a MoQ relay as a publisher and sends mocap and audio data.
 * Uses the moq-ffi library for WebTransport/QUIC connectivity.
 *
 * Track Architecture:
 * - Mocap track: "mocap/<session>/<track>" - for motion capture data
 * - Audio track: "audio/<session>/<track>" - for audio data (separate publisher)
 * - Control track: "control/<session>/<track>" - control envelopes (ADR 0011), stream delivery
 *
 * Shared transport blocks (ADR 0007 item 7, WP-A1 PR 4e): SendSerialized, SendControl and the
 * audio sinks (FO3DQueuedSenderAudioSink, bare audio payloads for the audio track) only enqueue on
 * one FO3DSendQueue; an FO3DTransportWorker publishes each item on its track's publisher.
 *
 * Queue policy: EO3DMocapOverflow::RefuseNewest with queue_bytes as the frame byte limit, as
 * before. The worker drops an item whose publisher is not ready (not connected, or the track not
 * announced yet), oldest first, so the queue holds no stale backlog across a reconnect; the queue
 * fills only when the worker itself falls behind the relay. Audio (1 MiB) and control (1,024
 * envelopes) have budgets of their own.
 *
 * Reconnect: driven on the game thread from Tick() and the session's state callbacks (which the
 * session wrapper marshals to the game thread), with MoQHelpers::ComputeBackoffDelaySeconds and a
 * connect timeout. moq-ffi does not reconnect by itself (every attempt is a fresh client), so this
 * is the only reconnect loop.
 *
 * Threading:
 * - Initialize(), Start(), Stop(), Tick(), CreateAudioSink() must be called from game thread
 * - Send(), SendSerialized(), SendControl() may be called from any thread
 * - Audio SubmitPcm() may be called from the audio capture thread
 * - The worker thread publishes (moq_publish_data)
 */
class FO3DMoQSender : public IOpen3DSender
{
public:
	/** Uses the production moq-ffi table and the platform clock. */
	FO3DMoQSender();
	/**
	 * Uses the given moq-ffi table (tests pass a fake, ADR 0006 F2), a clock in seconds for
	 * reconnect and timeout decisions (null = FPlatformTime::Seconds) and a backoff jitter seed.
	 */
	FO3DMoQSender(FMoQFfiApiRef InApi, TFunction<double()> InClock, uint64 InJitterSeed);
	virtual ~FO3DMoQSender();

	FO3DMoQSender(const FO3DMoQSender&) = delete;
	FO3DMoQSender& operator=(const FO3DMoQSender&) = delete;

	/** Encoded audio frames that may wait for the worker (refused beyond it), as before WP-A1 PR 4e. */
	static constexpr int64 AudioQueueBytes = 1024 * 1024;

	// IOpen3DSender interface
	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return MoQHelpers::GetCapabilities(FO3DTransportConfig()); }
	/**
	 * Connecting until the relay session is up, Connected while it is, Reconnecting after it
	 * dropped. Session changes arrive on the game thread (the session wrapper marshals them).
	 */
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;
	virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len) override;

	/**
	 * Test hook: while paused the worker publishes nothing, so the queue policy can be observed.
	 * Pausing returns once the worker has seen the flag (HANDOFF pitfall 15).
	 */
	void SetWorkerPausedForTesting(bool bPaused);

private:
	/** The MoQ track an item is published on. */
	enum class ETrack : uint8
	{
		Mocap,
		Audio,
		/** Control envelopes (ADR 0011): never counted as frames or dropped frames. */
		Control,
	};

	struct FMoQSenderOptions
	{
		FString RelayUrl;
		FString MocapNamespace;      // e.g., "mocap/session1"
		FString AudioNamespace;      // e.g., "audio/session1"
		FString ControlNamespace;    // e.g., "control/session1"
		FString TrackName;           // e.g., "character1"
		MoqDeliveryMode DeliveryMode = MOQ_DELIVERY_STREAM;
		uint64 MaxQueueBytes = 8ull * 1024ull * 1024ull;
		/** Abandon a connect attempt that has not completed after this long (TRF-11). */
		double ConnectTimeoutSeconds = 15.0;
	};

	static ETrack TrackOf(EO3DSendItemKind Kind);

	bool ParseOptions(const FO3DTransportConfig& Config, FString& OutError);
	bool AttemptConnect();
	void HandleConnectionStateChanged(MoqConnectionState NewState);
	/** Game thread: gives up on an in-flight connect that exceeded ConnectTimeoutSeconds. */
	void HandleConnectTimeout(double Now);
	/** Game thread: schedules the next connect attempt using capped, jittered backoff. */
	void ScheduleReconnect(double Now);
	double NowSeconds() const;
	/** Snapshot of a publisher handle; any thread (TRF-9). */
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> GetPublisher(ETrack Track) const;
	bool IsPublisherReady(ETrack Track) const;
	bool EnsurePublisher();
	bool EnsureAudioPublisher();
	bool EnsureControlPublisher();
	void DestroyPublisher();
	void DestroyAudioPublisher();
	void DestroyControlPublisher();
	EO3DSendResult EnqueueFrame(TArray<uint8>&& Bytes, FString SubjectName, double CaptureTimestampSec, bool bFullSync);
	/** Reports a lost session: Reconnecting when it had been connected (game thread). */
	void ReportSessionLost(const FString& Reason);
	/** Empties the queue (worker not running); frames discarded count as dropped, as before. */
	void DrainQueue();
	/** Worker thread. */
	uint32 RunWorkerIteration();
	/** Worker thread. */
	bool PublishItem(const FO3DSendItem& Item);
	void ResetStats();

	FString ResolveAudioSubjectFallback() const;

	FMoQSenderOptions Options;
	FMoQFfiApiRef Api;
	TFunction<double()> Clock;
	uint64 JitterSeed = 0;
	TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Session;
	/**
	 * TRF-9: written on the game thread, read by the worker. Both sides go through
	 * PublisherMutex, and the worker publishes on a snapshot, never on the member itself.
	 */
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> MocapPublisherHandle;
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> AudioPublisherHandle;
	/** Control track (ADR 0011); created on every connect, so receivers can subscribe before the first cue. */
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> ControlPublisherHandle;
	mutable FCriticalSection PublisherMutex;
	FDelegateHandle ConnectionDelegateHandle;

	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue;
	/** WP-S5 publish state shared with the audio sinks (never the sender itself). */
	const TSharedRef<FO3DAudioPublishState, ESPMode::ThreadSafe> PublishState;
	FO3DTransportWorker Worker;
	std::atomic<bool> bWorkerPausedForTesting{ false };
	/** Worker iterations that saw bWorkerPausedForTesting; SetWorkerPausedForTesting waits on it. */
	std::atomic<int64> PausedIterations{ 0 };

	std::atomic<int64> FramesSent{ 0 };
	std::atomic<int64> BytesSent{ 0 };
	/** Frames refused by the queue, dropped by the worker (publisher not ready), failed, or drained by Stop. */
	std::atomic<int64> DroppedFrames{ 0 };
	std::atomic<int64> SendErrors{ 0 };

	struct FLatencyStats
	{
		double TotalLatencyMs = 0.0;
		double MaxLatencyMs = 0.0;
		int64 Samples = 0;
	};
	/** Written by the worker, read by GetStats. */
	FLatencyStats LatencyStats;
	mutable FCriticalSection LatencyMutex;

	FThreadSafeBool bInitialized = false;
	FThreadSafeBool bRunning = false;

	TAtomic<MoqConnectionState> CachedState;
	FThreadSafeBool bConnectInFlight = false;
	int32 ConsecutiveFailures = 0;
	double LastConnectAttemptTimeSeconds = 0.0;
	/** Earliest time (NowSeconds) for the next connect attempt; game thread only. */
	double NextConnectAttemptTimeSeconds = 0.0;

	/** Publisher-creation failures (game thread) and mocap/audio publish failures (worker). */
	std::atomic<double> LastErrorLogTimeSeconds{ 0.0 };
	/** Control has its own throttle, so its failures never hide a mocap warning. */
	std::atomic<double> LastControlErrorLogTimeSeconds{ 0.0 };
	/** Written by any thread that calls SendSerialized. */
	std::atomic<double> LastDropLogTimeSeconds{ 0.0 };

	FO3DTransportConfig ActiveConfig;

	FO3DTransportAudioConfig ActiveAudioConfig;
	FGuid AudioSourceGuid;
	/** Set once an audio sink has been handed out; creates the audio publisher on connect. Game thread only. */
	bool bAudioRequested = false;

	/** This transport's counters, resolved once (SHR-3, SHR-17): no lock or lookup per frame. */
	const FO3DTransportMetricsRef TransportMetrics;

	/** ADR 0007 item 3. */
	FO3DConnectionStateTracker ConnectionState;
};
