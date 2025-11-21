// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Containers/Queue.h"

#include "O3DSenderInterface.h"
#include "MoQ/MoQTrackManager.h"
#include "Shared/QuicHelpers.h"
#include "Sender/QuicLocalRelayPublisher.h"

#include <atomic>

class OPEN3DTRANSPORTQUIC_API FO3DQuicSender : public IOpen3DSender
{
public:
	FO3DQuicSender();
	virtual ~FO3DQuicSender() override;

	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual bool Send(const O3DS::SubjectList& List) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override;
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
	struct FPendingFrame
	{
		TSharedPtr<TArray<uint8>> Payload;
		uint64 TimestampMicros = 0;
		uint32 Sequence = 0;
		O3DMoQ::FMoQTrackId TrackId = 0;
	};

	class FSendWorker final : public FRunnable
	{
	public:
		explicit FSendWorker(FO3DQuicSender& InOwner)
			: Owner(InOwner)
		{
		}

		virtual uint32 Run() override;
		virtual void Stop() override;

	private:
		FO3DQuicSender& Owner;
	};

private:
	bool PublishPrimaryTrack(FString& OutError);
	bool EnsureRelayStarted(FString& OutError);
	void ResetTrackState();
	void SyncRelayTrackMetadata();
	bool StartWorker();
	void StopWorker();
	bool EnqueueFrame(const TSharedPtr<TArray<uint8>>& Payload, uint64 TimestampMicros);
	bool DequeueFrame(FPendingFrame& OutFrame);
	void DrainQueue();
	void RunSendLoop();
	void ProcessFrame(FPendingFrame&& Frame);
	TSharedPtr<TArray<uint8>> SerializeSubjectList(const O3DS::SubjectList& List, double TimestampSeconds) const;
	TArray<uint8> BuildObjectMessage(const TSharedPtr<TArray<uint8>>& Payload, uint32 Sequence, uint64 TimestampMicros) const;

#if WITH_DEV_AUTOMATION_TESTS
public:
	void SetRelayForTesting(const TSharedPtr<O3DQuic::IQuicPublisherRelay, ESPMode::ThreadSafe>& InRelay) { Relay = InRelay; SyncRelayTrackMetadata(); }
	bool EnqueueFrameForTesting(const TArray<uint8>& Payload, uint64 TimestampMicros);
	bool ProcessSingleFrameForTesting();
#endif

private:
	mutable FCriticalSection Guard;
	mutable FCriticalSection QueueMutex;

	O3DQuic::FQuicSenderOptions ActiveOptions;
	O3DMoQ::FMoQTrackManager TrackManager;
	O3DMoQ::FMoQTrackId PrimaryTrackId = 0;
	TArray<uint8> CachedAnnouncePayload;
	TArray<uint8> CachedUnannouncePayload;

	bool bInitialized = false;
	bool bStarted = false;
	std::atomic<bool> bStopRequested{ false };
	uint32 NextSequence = 1;

	static constexpr uint64 MaxQueueBytes = 32ull * 1024ull * 1024ull;
	uint64 QueuedBytes = 0;
	TQueue<FPendingFrame> PendingFrames;
	FEvent* WakeEvent = nullptr;
	TUniquePtr<FRunnableThread> WorkerThread;
	TUniquePtr<FSendWorker> Worker;
	TSharedPtr<O3DQuic::IQuicPublisherRelay, ESPMode::ThreadSafe> Relay;

	FO3DTransportStats Stats;
	FString LastError;
	double LastNoSubscriberLogSeconds = 0.0;
};
