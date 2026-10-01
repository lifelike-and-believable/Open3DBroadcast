// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DConnectionState.h"
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
 * TCP sender - server mode (listens and accepts one receiver at a time).
 *
 * Threading (WP-S6):
 * - Initialize/Start/Stop/Tick/CreateAudioSink: game thread.
 * - Send/SendSerialized: any thread; they only enqueue.
 * - The worker thread owns the client socket: it accepts, sends (handling partial sends and
 *   EWOULDBLOCK), writes keepalives and notices a closed peer. The game thread touches the
 *   sockets only while the worker is not running, so no socket lock is needed and the game
 *   thread never waits on a send.
 */
class FO3DSocketsTcpSender : public IOpen3DSender
{
public:
	FO3DSocketsTcpSender();
	virtual ~FO3DSocketsTcpSender() override;

	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual bool Send(const O3DS::SubjectList& List) override;
	virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DSockets::GetTcpCapabilities(FO3DTransportConfig()); }
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;
	virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len) override;

	/** True while a receiver is connected. Any thread. */
	bool HasClient() const { return PublishState->bClientConnected.load(); }

	/** Bytes waiting in the send queue. Any thread; for tests and diagnostics. */
	uint64 GetPendingQueueBytes() const { return PublishState->SendQueue.GetPendingBytes(); }

	/** Times a send could not complete at once (partial send or EWOULDBLOCK). For tests and diagnostics. */
	int64 GetSendWaitCount() const { return SendWaitCount.load(); }

private:
	class FTcpSenderRunnable;

	FO3DTransportResult CreateListenSocket();
	void DestroySocket();
	EO3DSendResult SendBytes(const uint8* Data, int32 Len);
	TSharedPtr<FInternetAddr> CreateBindAddress(const FString& Host, int32 Port, bool& bOutValid);

	// Async send worker
	bool StartWorker();
	void StopWorker();
	uint32 RunWorker();
	bool EnqueuePayload(const uint8* Data, int32 Size);
	void DrainQueue();

	// Worker thread only.
	bool TryAcceptClient();
	void DropClient(const TCHAR* Reason);
	bool IsPeerClosed();
	void DropQueuedWithoutClient();
	bool DequeueNextFrame(TArray<uint8>& OutItem, int32& OutOffset, double Now);
	void AddDroppedFrames(int64 Count);

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* ListenSocket = nullptr;
	/** Owned by the worker while it runs. */
	FSocket* ClientSocket = nullptr;

	FString BindHost;
	int32 BindPort = 0;
	FString StreamId;

	FGuid AudioSourceGuid;

	mutable std::vector<char> SerializationScratch; // Reused buffer for mocap serialization to avoid per-frame allocations

	// Async send worker. The queue itself lives in PublishState so audio sinks can feed it.
	FTcpSenderRunnable* Worker = nullptr;
	FRunnableThread* WorkerThread = nullptr;
	TAtomic<bool> bStopWorker{false};

	// Limits read in Initialize() (see SocketsTcpTransport.h for keys and defaults).
	uint64 MaxQueueBytes = 0;
	double MaxQueueAgeSeconds = 0.0;
	double StallTimeoutSeconds = 0.0;
	double KeepaliveIntervalSeconds = 0.0;

	/** Prebuilt keepalive frame (TRB-6). */
	TArray<uint8> KeepaliveFrame;

	std::atomic<int64> SendWaitCount{0};

	mutable FCriticalSection StatsMutex;

	/** Set by a successful Start(), cleared by Stop(); sends outside a session return NotRunning. */
	std::atomic<bool> bRunning{false};

	/**
	 * ADR 0007 item 3: Connecting while listening without a receiver, Connected while one is
	 * connected (set by the worker on accept), Reconnecting after it went away.
	 */
	FO3DConnectionStateTracker ConnectionState;

	TSharedRef<FSocketsTcpPublishState, ESPMode::ThreadSafe> PublishState;
};
