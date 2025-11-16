#pragma once

#include "CoreMinimal.h"
#include "O3DSenderInterface.h"
#include "../Shared/SocketsTransportCommon.h"
#include "O3DAudioFrameCodec.h"

#include "HAL/CriticalSection.h"
#include "Containers/Queue.h"

#include <vector>

class FSocket;
class ISocketSubsystem;
class FInternetAddr;
class FSocketsTcpSenderAudioSink;
class FRunnableThread;
class FEvent;

/**
 * TCP sender - server mode (listens and accepts connections).
 * Adapted from UDP sender pattern for reliability.
 */
class FO3DSocketsTcpSender : public IOpen3DSender
{
public:
	FO3DSocketsTcpSender();
	virtual ~FO3DSocketsTcpSender() override;

	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual bool Send(const O3DS::SubjectList& List) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override;
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
	struct FQueuedPayload
	{
		TArray<uint8> Bytes;
	};

	class FTcpSenderRunnable;

	bool CreateListenSocket();
	void DestroySocket();
	void TickAcceptClient();
	bool SendFramed(FSocket* InSocket, const uint8* Data, int32 Size);
	void RefreshAudioEncoder();
	bool ProcessCapturedAudio(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);
	bool SendEncodedAudio(const O3DAudio::FEncodedFrame& Frame, double TimestampSec);
	TSharedPtr<FInternetAddr> CreateBindAddress(const FString& Host, int32 Port, bool& bOutValid);

	// Async send worker
	void StartWorker();
	void StopWorker();
	uint32 RunWorker();
	bool EnqueuePayload(const uint8* Data, int32 Size);
	void DrainQueue();

private:
	friend class FSocketsTcpSenderAudioSink;

	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* ListenSocket = nullptr;
	FSocket* ClientSocket = nullptr;
	TAtomic<bool> bConnected{false}; // Connection state for fast checks without lock

	FString BindHost;
	int32 BindPort = 0;
	FString StreamId;

	FGuid AudioSourceGuid;

	double LastAcceptPollTime = 0.0;
	bool bAudioEncoderInitialized = false;
	O3DAudio::FFrameEncoder AudioEncoder;
	TArray<uint8> UnifiedAudioScratch;
	mutable std::vector<char> SerializationScratch; // Reused buffer for mocap serialization to avoid per-frame allocations

	// Async send queue
	TQueue<FQueuedPayload, EQueueMode::Mpsc> SendQueue;
	FTcpSenderRunnable* Worker = nullptr;
	FRunnableThread* WorkerThread = nullptr;
	FEvent* WakeEvent = nullptr;
	TAtomic<bool> bStopWorker{false};
	TAtomic<uint64> QueueBytes{0};
	uint64 MaxQueueBytes = 4 * 1024 * 1024; // 4MB default

	mutable FCriticalSection StatsMutex;
};
