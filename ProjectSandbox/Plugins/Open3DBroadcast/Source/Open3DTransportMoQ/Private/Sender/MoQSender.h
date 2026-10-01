// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "Containers/Queue.h"
#include "Transport/O3DSenderInterface.h"
#include "O3DAudioFrameCodec.h"
#include "O3DPerformanceMetrics.h"
#include "MoQFfiApi.h"
THIRD_PARTY_INCLUDES_START
#include "moq_ffi.h"
THIRD_PARTY_INCLUDES_END

class FMoQSessionWrapper;
class FMoQPublisherHandle;
class FSendWorker;
class FRunnableThread;
struct FMoQSenderAudioState;

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
 * This leverages MoQ's native support for multiple tracks, providing clean
 * separation between data types rather than multiplexing like NNG.
 * 
 * Audio Support (Phase 4):
 * - Audio is published on a separate MoQ track with "audio" namespace prefix
 * - PCM audio is encoded to PCM16 or Opus using O3DAudio framework
 * - Each track type has its own publisher for independent flow control
 * 
 * Threading:
 * - Initialize(), Start(), Stop(), Tick() must be called from game thread
 * - Send() may be called from any thread (typically frame capture thread)
 * - Audio SubmitPcm() may be called from audio capture thread
 * - Internal worker thread handles actual network publishing
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

	// IOpen3DSender interface
	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual bool Send(const O3DS::SubjectList& List) override;
	virtual bool SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override { return true; }
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;
	virtual bool SupportsControl() const override { return true; }
	virtual bool SendControl(const uint8* Envelope, int32 Len) override;

private:
	friend class FSendWorker;

	/** The MoQ track a queued payload is published on. */
	enum class ETrack : uint8
	{
		Mocap,
		Audio,
		/** Control envelopes (ADR 0011): never counted as frames or dropped frames. */
		Control,
	};

	struct FPendingPayload
	{
		TArray<uint8> Data;
		double EnqueueTimestampSeconds = 0.0;
		ETrack Track = ETrack::Mocap;
	};

	struct FLatencyStats
	{
		double TotalLatencyMs = 0.0;
		double MaxLatencyMs = 0.0;
		int64 Samples = 0;
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
	void StartWorker();
	void StopWorker();
	void WakeWorker();
	bool IsPublisherReady() const;
	bool IsAudioPublisherReady() const;
	bool IsControlPublisherReady() const;
	bool EnsurePublisher();
	bool EnsureAudioPublisher();
	bool EnsureControlPublisher();
	void DestroyPublisher();
	void DestroyAudioPublisher();
	void DestroyControlPublisher();
	bool EnqueuePayload(TArray<uint8>&& Data, double CaptureTimestampSec, ETrack Track = ETrack::Mocap);
	bool SendBytes(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec);
	bool DequeuePayload(TUniquePtr<FPendingPayload>& OutPayload);
	void DrainQueue();
	bool PublishPayload(const FPendingPayload& Payload);
	uint32 RunWorker();
	void ResetStats();
	
	// Audio support (Phase 4)
	FString ResolveAudioSubjectFallback() const;
	void DrainAudioQueue(bool bPublish);

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

	TQueue<TUniquePtr<FPendingPayload>, EQueueMode::Mpsc> SendQueue;
	mutable FCriticalSection QueueMutex;
	uint64 PendingQueueBytes = 0;

	TUniquePtr<FSendWorker> WorkerRunnable;
	FRunnableThread* WorkerThread = nullptr;
	FThreadSafeBool bWorkerStopRequested = false;

	FO3DTransportStats Stats;
	mutable FCriticalSection StatsMutex;
	FLatencyStats LatencyStats;

	FThreadSafeBool bInitialized = false;
	FThreadSafeBool bRunning = false;

	TAtomic<MoqConnectionState> CachedState;
	FThreadSafeBool bConnectInFlight = false;
	int32 ConsecutiveFailures = 0;
	double LastConnectAttemptTimeSeconds = 0.0;
	/** Earliest time (NowSeconds) for the next connect attempt; game thread only. */
	double NextConnectAttemptTimeSeconds = 0.0;

	double LastErrorLogTimeSeconds = 0.0;
	/** Control has its own throttle, so its failures never hide a mocap warning. */
	double LastControlErrorLogTimeSeconds = 0.0;
	double LastDropLogTimeSeconds = 0.0;

	FO3DTransportConfig ActiveConfig;
	
	// Audio support (Phase 4)
	FO3DTransportAudioConfig ActiveAudioConfig;
	FGuid AudioSourceGuid;
	/** Set once an audio sink has been handed out; creates the audio publisher on connect. Game thread only. */
	bool bAudioRequested = false;

	/**
	 * WP-S5: shared with audio sinks (never the sender itself). Its queue's wake event is also
	 * the worker's wake event, so no thread can trigger a pooled event after Stop() (TRB-12).
	 */
	TSharedRef<FMoQSenderAudioState, ESPMode::ThreadSafe> AudioState;

	/** This transport's counters, resolved once (SHR-3, SHR-17): no lock or lookup per frame. */
	const FO3DTransportMetricsRef TransportMetrics;
};
