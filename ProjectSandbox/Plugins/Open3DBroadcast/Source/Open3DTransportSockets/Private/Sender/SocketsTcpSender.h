#pragma once

#include "CoreMinimal.h"
#include "O3DSenderInterface.h"
#include "../Shared/SocketsTransportCommon.h"
#include "O3DAudioFrameCodec.h"
#include "O3DEncodedPayloadQueue.h"
#include "O3DLifetimeGate.h"

#include "HAL/CriticalSection.h"

#include <atomic>

#include <vector>

class FSocket;
class ISocketSubsystem;
class FInternetAddr;
class FSocketsTcpSenderAudioSink;
class FRunnableThread;
class FO3DSocketsTcpSender;

/**
 * Publish state shared between FO3DSocketsTcpSender, its worker and the audio sinks it hands
 * out (ADR 0007 addendum, WP-S5: TRB-10, TRB-12). The audio capture component can keep a
 * sink alive after the sender is gone, so the sink holds this state and never the sender.
 * It contains no socket, no sender pointer and nothing whose destructor does I/O, so any
 * thread may drop the last reference.
 */
struct FSocketsTcpPublishState
{
	/** Closed by Stop() before the socket and worker are torn down. */
	TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> Gate = MakeShared<FO3DLifetimeGate, ESPMode::ThreadSafe>();

	/** Framed payloads (mocap and audio) for the worker; owns the worker's wake event. */
	FO3DEncodedPayloadQueue SendQueue;

	/** Mirrors "a client is connected"; written by the game thread and the worker. */
	std::atomic<bool> bClientConnected{false};

	/** Bytes of audio accepted by sinks, folded into GetStats(). */
	std::atomic<int64> AudioBytesQueued{0};
};

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
	virtual bool SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override;
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
	class FTcpSenderRunnable;

	bool CreateListenSocket();
	void DestroySocket();
	bool SendBytes(const uint8* Data, int32 Len);
	void TickAcceptClient();
	bool SendFramed(FSocket* InSocket, const uint8* Data, int32 Size);
	TSharedPtr<FInternetAddr> CreateBindAddress(const FString& Host, int32 Port, bool& bOutValid);

	// Async send worker
	void StartWorker();
	void StopWorker();
	uint32 RunWorker();
	bool EnqueuePayload(const uint8* Data, int32 Size);
	void DrainQueue();

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* ListenSocket = nullptr;
	FSocket* ClientSocket = nullptr;

	FString BindHost;
	int32 BindPort = 0;
	FString StreamId;

	FGuid AudioSourceGuid;

	double LastAcceptPollTime = 0.0;
	mutable std::vector<char> SerializationScratch; // Reused buffer for mocap serialization to avoid per-frame allocations

	// Async send worker. The queue itself lives in PublishState so audio sinks can feed it.
	FTcpSenderRunnable* Worker = nullptr;
	FRunnableThread* WorkerThread = nullptr;
	TAtomic<bool> bStopWorker{false};
	static constexpr uint64 DefaultMaxQueueBytes = 4 * 1024 * 1024; // 4MB default

	mutable FCriticalSection StatsMutex;

	/** Guards ClientSocket/ListenSocket between the game thread and the worker. Never taken on the audio thread. */
	FCriticalSection SocketLock;

	TSharedRef<FSocketsTcpPublishState, ESPMode::ThreadSafe> PublishState;
};
