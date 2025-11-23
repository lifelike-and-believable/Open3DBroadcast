#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "Containers/Queue.h"
#include "O3DSenderInterface.h"
#include "moq_ffi.h"

class FMoQSessionWrapper;
class FMoQPublisherHandle;
class FSendWorker;
class FEvent;
class FRunnableThread;

DECLARE_LOG_CATEGORY_EXTERN(LogO3DMoQSender, Log, All);

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
	virtual bool SupportsAudio() const override { return false; }
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override { return nullptr; }

private:
	friend class FSendWorker;

	struct FPendingPayload
	{
		TArray<uint8> Data;
		double EnqueueTimestampSeconds = 0.0;
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
		FString TrackNamespace;
		FString TrackName;
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
	bool EnsurePublisher();
	void DestroyPublisher();
	bool EnqueuePayload(TArray<uint8>&& Data, double CaptureTimestampSec);
	bool DequeuePayload(TUniquePtr<FPendingPayload>& OutPayload);
	void DrainQueue();
	bool PublishPayload(const FPendingPayload& Payload);
	uint32 RunWorker();
	void ResetStats();
	double ComputeReconnectDelaySeconds() const;

	FMoQSenderOptions Options;
	TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Session;
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> PublisherHandle;
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
};
