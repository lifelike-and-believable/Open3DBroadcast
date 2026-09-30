#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "Containers/Queue.h"
#include "O3DReceiverInterface.h"
#include "O3DAudioFrameCodec.h"
#include "MoQFfiApi.h"
#include "moq_ffi.h"

class ISerializedFrameConsumer;
class FMoQSessionWrapper;
class FMoQSubscriberHandle;

DECLARE_LOG_CATEGORY_EXTERN(LogO3DMoQReceiver, Log, All);

/**
 * MoQ Transport Receiver Implementation (Phase 3 + Phase 4 Audio)
 * 
 * Connects to a MoQ relay as a subscriber and receives mocap and audio data.
 * Uses the moq-ffi library for WebTransport/QUIC connectivity.
 * 
 * Track Architecture:
 * - Mocap track: "mocap/<session>/<track>" - motion capture data
 * - Audio track: "audio/<session>/<track>" - audio data (separate subscription)
 * 
 * Threading:
 * - Initialize(), Start(), Stop(), Poll() must be called from game thread
 * - SetConsumer(), SetAudioSink() should be called before Start()
 * - Data callbacks from moq-ffi may arrive on worker threads
 */
class FO3DMoQReceiver : public IOpen3DReceiver
{
public:
	/** Uses the production moq-ffi table and the platform clock. */
	FO3DMoQReceiver();
	/**
	 * Uses the given moq-ffi table (tests pass a fake, ADR 0006 F2), a clock in seconds for
	 * reconnect, timeout and subscribe-retry decisions (null = FPlatformTime::Seconds) and a
	 * backoff jitter seed.
	 */
	FO3DMoQReceiver(FMoQFfiApiRef InApi, TFunction<double()> InClock, uint64 InJitterSeed);
	virtual ~FO3DMoQReceiver();

	FO3DMoQReceiver(const FO3DMoQReceiver&) = delete;
	FO3DMoQReceiver& operator=(const FO3DMoQReceiver&) = delete;

	// IOpen3DReceiver interface
	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override { return true; }
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;

private:
	struct FReceivedPayload
	{
		TArray<uint8> Data;
		double ReceiveTimestampSeconds = 0.0;
		bool bIsAudio = false;  // true if this payload came from audio track
	};

	struct FLatencyStats
	{
		double TotalLatencyMs = 0.0;
		double MaxLatencyMs = 0.0;
		int64 Samples = 0;
	};

	struct FMoQReceiverOptions
	{
		FString RelayUrl;
		FString MocapNamespace;      // e.g., "mocap/session1"
		FString AudioNamespace;      // e.g., "audio/session1"
		FString TrackName;           // e.g., "character1"
		FString StreamId;
		/** Abandon a connect attempt that has not completed after this long (TRF-11). */
		double ConnectTimeoutSeconds = 15.0;
	};

	/** Retry state for one track subscription (TRF-20). Game thread only. */
	struct FSubscribeRetryState
	{
		int32 ConsecutiveFailures = 0;
		double NextAttemptTimeSeconds = 0.0;

		void Reset()
		{
			ConsecutiveFailures = 0;
			NextAttemptTimeSeconds = 0.0;
		}
	};

	bool ParseOptions(const FO3DTransportConfig& Config, FString& OutError);
	bool AttemptConnect();
	void HandleConnectionStateChanged(MoqConnectionState NewState);
	/** Game thread: gives up on an in-flight connect that exceeded ConnectTimeoutSeconds. */
	void HandleConnectTimeout(double Now);
	/** Game thread: schedules the next connect attempt using capped, jittered backoff. */
	void ScheduleReconnect(double Now);
	/** Game thread: records a failed subscribe and schedules the next try. */
	void ScheduleSubscribeRetry(FSubscribeRetryState& Retry, double Now, uint64 SeedSalt);
	double NowSeconds() const;
	bool AttemptSubscribe();
	bool AttemptAudioSubscribe();
	void HandleMocapDataReceived(const TArray64<uint8>& Payload);
	void HandleAudioDataReceived(const TArray64<uint8>& Payload);
	void DestroySubscriber();
	void DestroyAudioSubscriber();
	bool ProcessReceivedPayload(const FReceivedPayload& Payload);
	bool ProcessAudioPayload(const FReceivedPayload& Payload);
	void ResetStats();

	FMoQReceiverOptions Options;
	FMoQFfiApiRef Api;
	TFunction<double()> Clock;
	uint64 JitterSeed = 0;
	TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Session;
	TSharedPtr<FMoQSubscriberHandle, ESPMode::ThreadSafe> MocapSubscriberHandle;
	TSharedPtr<FMoQSubscriberHandle, ESPMode::ThreadSafe> AudioSubscriberHandle;
	FDelegateHandle ConnectionDelegateHandle;

	TWeakPtr<ISerializedFrameConsumer> Consumer;
	TWeakPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;

	TQueue<TUniquePtr<FReceivedPayload>, EQueueMode::Mpsc> ReceiveQueue;
	mutable FCriticalSection QueueMutex;
	uint64 PendingQueueBytes = 0;
	static constexpr uint64 kMaxQueueBytes = 16ull * 1024ull * 1024ull;

	FO3DTransportStats Stats;
	mutable FCriticalSection StatsMutex;
	FLatencyStats LatencyStats;

	FThreadSafeBool bInitialized = false;
	FThreadSafeBool bRunning = false;
	FThreadSafeBool bMocapSubscribed = false;
	FThreadSafeBool bAudioSubscribed = false;

	TAtomic<MoqConnectionState> CachedState;
	FThreadSafeBool bConnectInFlight = false;
	int32 ConsecutiveFailures = 0;
	double LastConnectAttemptTimeSeconds = 0.0;
	/** Earliest time (NowSeconds) for the next connect attempt. */
	double NextConnectAttemptTimeSeconds = 0.0;
	double LastSubscribeAttemptTimeSeconds = 0.0;
	FSubscribeRetryState MocapSubscribeRetry;
	FSubscribeRetryState AudioSubscribeRetry;
	double LastErrorLogTimeSeconds = 0.0;

	FO3DTransportConfig ActiveConfig;
	/** Audio config from the source; the decode codec comes from each frame, not from here (TRF-37). */
	FO3DTransportAudioConfig ActiveAudioConfig;
	O3DAudio::FMultiStreamFrameDecoder AudioDecoder; // SHR-15: one decoder per (SourceGuid, StreamLabel)
	TArray<int16> DecodedPcmScratch;

	// Shared alive flag for safe callback handling - set to false during destruction
	// This prevents use-after-free when callbacks are pending on the game thread
	TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> AliveFlag;

	static constexpr double kErrorLogIntervalSeconds = 5.0;
	static constexpr int32 kMaxFramesPerPoll = 16;
};
