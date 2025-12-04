#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "Containers/Queue.h"
#include "O3DSenderInterface.h"
#include "O3DAudioFrameCodec.h"
#include "moq_ffi.h"

class FMoQSessionWrapper;
class FMoQPublisherHandle;
class FSendWorker;
class FEvent;
class FRunnableThread;

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
	FO3DMoQSender();
	virtual ~FO3DMoQSender();

	// IOpen3DSender interface
	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual bool Send(const O3DS::SubjectList& List) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override { return true; }
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
	friend class FSendWorker;
	friend class FO3DMoQSenderAudioSink;

	struct FPendingPayload
	{
		TArray<uint8> Data;
		double EnqueueTimestampSeconds = 0.0;
		bool bIsAudio = false;  // true if this payload should go to audio track
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
		FString TrackName;           // e.g., "character1"
		MoqDeliveryMode DeliveryMode = MOQ_DELIVERY_STREAM;
		uint64 MaxQueueBytes = 8ull * 1024ull * 1024ull;
	};

	bool ParseOptions(const FO3DTransportConfig& Config, FString& OutError);
	bool AttemptConnect();
	void HandleConnectionStateChanged(MoqConnectionState NewState);
	void StartWorker();
	void StopWorker();
	void WakeWorker();
	bool IsPublisherReady() const;
	bool IsAudioPublisherReady() const;
	bool EnsurePublisher();
	bool EnsureAudioPublisher();
	void DestroyPublisher();
	void DestroyAudioPublisher();
	bool EnqueuePayload(TArray<uint8>&& Data, double CaptureTimestampSec, bool bIsAudio = false);
	bool DequeuePayload(TUniquePtr<FPendingPayload>& OutPayload);
	void DrainQueue();
	bool PublishPayload(const FPendingPayload& Payload);
	uint32 RunWorker();
	void ResetStats();
	
	// Audio support (Phase 4)
	void RefreshAudioEncoder();
	bool ProcessCapturedAudio(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);
	bool SendEncodedAudio(const O3DAudio::FEncodedFrame& Frame, double TimestampSec);

	FMoQSenderOptions Options;
	TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Session;
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> MocapPublisherHandle;
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> AudioPublisherHandle;
	FDelegateHandle ConnectionDelegateHandle;

	TQueue<TUniquePtr<FPendingPayload>, EQueueMode::Mpsc> SendQueue;
	mutable FCriticalSection QueueMutex;
	uint64 PendingQueueBytes = 0;

	FEvent* WakeEvent = nullptr;
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

	double LastErrorLogTimeSeconds = 0.0;
	double LastDropLogTimeSeconds = 0.0;

	FO3DTransportConfig ActiveConfig;
	
	// Audio support (Phase 4)
	FO3DTransportAudioConfig ActiveAudioConfig;
	FGuid AudioSourceGuid;
	bool bAudioEncoderInitialized = false;
	O3DAudio::FFrameEncoder AudioEncoder;
	
	mutable FCriticalSection SubjectNameLock;
	FString LastSubjectName;
};
