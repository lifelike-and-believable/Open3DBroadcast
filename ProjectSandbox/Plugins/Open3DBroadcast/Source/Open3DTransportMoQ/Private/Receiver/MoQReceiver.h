#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "Containers/Queue.h"
#include "O3DReceiverInterface.h"
#include "moq_ffi.h"

class ISerializedFrameConsumer;
class FMoQSessionWrapper;
class FMoQSubscriberHandle;

DECLARE_LOG_CATEGORY_EXTERN(LogO3DMoQReceiver, Log, All);

/**
 * MoQ Transport Receiver Implementation (Phase 3)
 * 
 * Connects to a MoQ relay as a subscriber and receives mocap data.
 * Uses the moq-ffi library for WebTransport/QUIC connectivity.
 */
class FO3DMoQReceiver : public IOpen3DReceiver
{
public:
	FO3DMoQReceiver();
	virtual ~FO3DMoQReceiver();

	// IOpen3DReceiver interface
	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override { return false; } // Phase 4
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override {} // Phase 4

private:
	struct FReceivedPayload
	{
		TArray<uint8> Data;
		double ReceiveTimestampSeconds = 0.0;
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
		FString TrackNamespace;
		FString TrackName;
		FString StreamId;
	};

	bool ParseOptions(const FO3DTransportConfig& Config, FString& OutError);
	bool AttemptConnect();
	void HandleConnectionStateChanged(MoqConnectionState NewState);
	bool AttemptSubscribe();
	void HandleDataReceived(const TArray64<uint8>& Payload);
	void DestroySubscriber();
	bool ProcessReceivedPayload(const FReceivedPayload& Payload);
	void ResetStats();
	double ComputeReconnectDelaySeconds() const;

	FMoQReceiverOptions Options;
	TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Session;
	TSharedPtr<FMoQSubscriberHandle, ESPMode::ThreadSafe> SubscriberHandle;
	FDelegateHandle ConnectionDelegateHandle;

	TWeakPtr<ISerializedFrameConsumer> Consumer;

	TQueue<TUniquePtr<FReceivedPayload>, EQueueMode::Mpsc> ReceiveQueue;
	mutable FCriticalSection QueueMutex;
	uint64 PendingQueueBytes = 0;
	static constexpr uint64 kMaxQueueBytes = 16ull * 1024ull * 1024ull;

	FO3DTransportStats Stats;
	mutable FCriticalSection StatsMutex;
	FLatencyStats LatencyStats;

	FThreadSafeBool bInitialized = false;
	FThreadSafeBool bRunning = false;
	FThreadSafeBool bSubscribed = false;

	TAtomic<MoqConnectionState> CachedState;
	FThreadSafeBool bConnectInFlight = false;
	int32 ConsecutiveFailures = 0;
	double LastConnectAttemptTimeSeconds = 0.0;
	double LastSubscribeAttemptTimeSeconds = 0.0;
	double LastErrorLogTimeSeconds = 0.0;

	FO3DTransportConfig ActiveConfig;

	// Shared alive flag for safe callback handling - set to false during destruction
	// This prevents use-after-free when callbacks are pending on the game thread
	TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> AliveFlag;

	static constexpr double kMinReconnectDelaySeconds = 0.5;
	static constexpr double kMaxReconnectDelaySeconds = 10.0;
	static constexpr double kErrorLogIntervalSeconds = 5.0;
	static constexpr int32 kMaxFramesPerPoll = 16;
};
