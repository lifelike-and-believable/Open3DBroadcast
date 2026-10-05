// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "Containers/Queue.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DUnifiedReceiveDemux.h"
#include "Shared/MoQHelpers.h"
#include "MoQFfiApi.h"
THIRD_PARTY_INCLUDES_START
#include "moq_ffi.h"
THIRD_PARTY_INCLUDES_END

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
 * - Control track: "control/<session>/<track>" - control envelopes (ADR 0011), subscribed
 *   only while a control sink is set
 * 
 * Shared transport blocks (ADR 0007 item 7, WP-A1 PR 4e): the session wrapper marshals moq-ffi's
 * data callbacks to the game thread; each payload is put on a bounded hand-off queue (an
 * FO3DSendQueue, one item kind per track) and Poll() routes up to kMaxFramesPerPoll of them
 * through the shared FO3DUnifiedReceiveDemux: mocap as a bare frame (DeliverMocap), audio as a
 * bare audio payload whose codec is read from its header (DeliverAudioPayload, TRF-37), control as
 * an envelope (DeliverControlEnvelope). The demux holds the consumer and sinks until Stop.
 *
 * Threading:
 * - Initialize(), Start(), Stop(), Poll() must be called from game thread
 * - SetConsumer(), SetAudioSink() should be called before Start()
 * - Data callbacks from moq-ffi arrive on moq-ffi threads and are run on the game thread
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
	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) override { Demux.SetConsumer(InConsumer); }
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return MoQHelpers::GetCapabilities(FO3DTransportConfig()); }
	/** As the sender: Connecting, Connected, Reconnecting from the relay session; changes arrive on the game thread. */
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
	virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override;

private:
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
		FString ControlNamespace;    // e.g., "control/session1"
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
	/** Reports a lost session: Reconnecting when it had been connected (game thread). */
	void ReportSessionLost(const FString& Reason);
	/** Game thread: schedules the next connect attempt using capped, jittered backoff. */
	void ScheduleReconnect(double Now);
	/** Game thread: records a failed subscribe and schedules the next try. */
	void ScheduleSubscribeRetry(FSubscribeRetryState& Retry, double Now, uint64 SeedSalt);
	double NowSeconds() const;
	bool AttemptSubscribe();
	bool AttemptAudioSubscribe();
	bool AttemptControlSubscribe();
	void HandleMocapDataReceived(const TArray64<uint8>& Payload);
	void HandleAudioDataReceived(const TArray64<uint8>& Payload);
	void HandleControlDataReceived(const TArray64<uint8>& Payload);
	/** Game thread (the dispatcher): queues one payload for Poll. Returns false when its kind is full. */
	bool EnqueueReceived(const TArray64<uint8>& Payload, EO3DSendItemKind Kind);
	void DestroySubscriber();
	void DestroyAudioSubscriber();
	void DestroyControlSubscriber();
	/**
	 * Poll: routes one queued payload through the demux and counts it. True for a mocap frame.
	 * A mocap item's bytes are handed to the consumer (SubmitFrameOwned, WP-A1 PR 5b), so Item is
	 * left without them.
	 */
	bool RouteReceived(FO3DSendItem& Item);
	void ResetStats();

	FMoQReceiverOptions Options;
	FMoQFfiApiRef Api;
	TFunction<double()> Clock;
	uint64 JitterSeed = 0;
	TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Session;
	TSharedPtr<FMoQSubscriberHandle, ESPMode::ThreadSafe> MocapSubscriberHandle;
	TSharedPtr<FMoQSubscriberHandle, ESPMode::ThreadSafe> AudioSubscriberHandle;
	TSharedPtr<FMoQSubscriberHandle, ESPMode::ThreadSafe> ControlSubscriberHandle;
	FDelegateHandle ConnectionDelegateHandle;

	/** Holds the consumer, audio sink and control sink strongly; Stop() releases them (TRF-38, ADR 0011). Game thread. */
	FO3DUnifiedReceiveDemux Demux;

	/**
	 * Payloads waiting for Poll (game thread on both sides). Mocap and audio each up to
	 * kMaxQueueBytes, control up to the queue's own cap; the newest is refused when full.
	 */
	FO3DSendQueue ReceiveQueue;
	static constexpr int64 kMaxQueueBytes = 16ll * 1024ll * 1024ll;

	FO3DTransportStats Stats;
	mutable FCriticalSection StatsMutex;
	FLatencyStats LatencyStats;

	FThreadSafeBool bInitialized = false;
	FThreadSafeBool bRunning = false;
	FThreadSafeBool bMocapSubscribed = false;
	FThreadSafeBool bAudioSubscribed = false;
	FThreadSafeBool bControlSubscribed = false;

	TAtomic<MoqConnectionState> CachedState;
	FThreadSafeBool bConnectInFlight = false;
	int32 ConsecutiveFailures = 0;
	double LastConnectAttemptTimeSeconds = 0.0;
	/** Earliest time (NowSeconds) for the next connect attempt. */
	double NextConnectAttemptTimeSeconds = 0.0;
	double LastSubscribeAttemptTimeSeconds = 0.0;
	FSubscribeRetryState MocapSubscribeRetry;
	FSubscribeRetryState AudioSubscribeRetry;
	FSubscribeRetryState ControlSubscribeRetry;
	double LastErrorLogTimeSeconds = 0.0;

	FO3DTransportConfig ActiveConfig;
	/** Audio config from the source; the decode codec comes from each frame, not from here (TRF-37). */
	FO3DTransportAudioConfig ActiveAudioConfig;

	// Shared alive flag for safe callback handling - set to false during destruction
	// This prevents use-after-free when callbacks are pending on the game thread
	TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> AliveFlag;

	static constexpr double kErrorLogIntervalSeconds = 5.0;
	static constexpr int32 kMaxFramesPerPoll = 16;
	/** Control payloads handled per Poll, on top of the frames, so a burst cannot stall it. */
	static constexpr int32 kMaxControlPerPoll = 64;

	/** ADR 0007 item 3. */
	FO3DConnectionStateTracker ConnectionState;
};
