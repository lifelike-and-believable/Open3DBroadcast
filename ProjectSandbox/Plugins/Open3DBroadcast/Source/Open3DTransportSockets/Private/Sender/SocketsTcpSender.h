// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DSenderAudioSinkBase.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportWorker.h"
#include "../Shared/SocketsTransportCommon.h"

#include <atomic>
#include <vector>

class FSocket;
class ISocketSubsystem;

/**
 * TCP sender - server mode (listens and accepts one receiver at a time).
 *
 * Built on the shared transport blocks (ADR 0007 item 7, WP-A1 PR 4b): frames, audio and control
 * are items on one FO3DSendQueue; the audio sinks are FO3DQueuedSenderAudioSink over an
 * FO3DAudioPublishState that never references the sender; an FO3DTransportWorker owns the
 * sockets and writes each item as one TCP frame (header from the core's tcp_stream_parser).
 *
 * Threading (WP-S6):
 * - Initialize/Start/Stop/Tick/CreateAudioSink: game thread. Start creates and binds the listen
 *   socket (the bind address must be an IP literal or a wildcard, so nothing resolves a name on
 *   the game thread) and reports AddressInUse at once.
 * - Send/SendSerialized/SendControl: any thread; they only enqueue.
 * - The worker accepts, sends (partial sends, EWOULDBLOCK), writes keepalives and notices a
 *   closed peer. The game thread touches the sockets only while the worker is not running.
 */
class FO3DSocketsTcpSender : public IOpen3DSender
{
public:
	FO3DSocketsTcpSender();
	virtual ~FO3DSocketsTcpSender() override;

	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DSockets::GetTcpCapabilities(FO3DTransportConfig()); }
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;
	virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len) override;
	/** Called on the worker thread when a receiver is accepted (ADR 0005 (vi)). */
	virtual void SetPeerJoinedCallback(FO3DPeerJoinedCallback Callback) override;

	/** True while a receiver is connected. Any thread. */
	bool HasClient() const { return PublishState->IsPeerReady(); }

	/** Payload bytes waiting in the send queue. Any thread; for tests and diagnostics. */
	uint64 GetPendingQueueBytes() const { return static_cast<uint64>(Queue->GetPendingBytes()); }

	/** Times a send could not complete at once (partial send or EWOULDBLOCK). For tests and diagnostics. */
	int64 GetSendWaitCount() const { return SendWaitCount.load(); }

private:
	FO3DTransportResult CreateListenSocket();
	void DestroySocket();
	/** Enqueues one frame and counts it as sent or dropped. Any thread. */
	EO3DSendResult EnqueueFrame(FO3DSendItem&& Item, int32 Len);
	void ApplyQueueLimits();

	// Worker thread only.
	uint32 RunWorkerIteration();
	bool TryAcceptClient();
	void DropClient(const TCHAR* Reason);
	bool IsPeerClosed();
	void DropQueuedWithoutClient();
	void SetPending(TArray<uint8>&& Payload, bool bKeepalive);
	void ResetPending();
	void DropClientAndPending(const TCHAR* Reason);
	/** Sends the next bytes of the pending frame; false on a hard socket error. */
	bool SendPendingBytes(int32& OutSent);

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* ListenSocket = nullptr;
	/** Owned by the worker while it runs. */
	FSocket* ClientSocket = nullptr;

	/** The listen endpoint (bind host and port). */
	FO3DHostPort BindEndpoint;
	FString StreamId;

	FGuid AudioSourceGuid;


	/** Frames, audio and control for the worker (ADR 0007 item 7). */
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue;
	/** Shared with the audio sinks; its peer flag mirrors "a receiver is connected". */
	const TSharedRef<FO3DAudioPublishState, ESPMode::ThreadSafe> PublishState;
	FO3DTransportWorker Worker;

	// Limits read in Initialize() (see SocketsTcpTransport.h for keys and defaults).
	int64 MaxQueueBytes = 0;
	double MaxQueueAgeSeconds = 0.0;
	double StallTimeoutSeconds = 0.0;
	double KeepaliveIntervalSeconds = 0.0;

	/** Keepalive payload: a unified-envelope header with an empty payload (TRB-6). */
	TArray<uint8> KeepalivePayload;

	// The frame being written (worker thread). Once its first byte is on the wire it is either
	// finished or the client is dropped, so the receiver never sees half a frame (TRB-2).
	uint8 PendingHeader[32] = {};
	TArray<uint8> PendingPayload;
	int32 PendingOffset = 0;
	int32 PendingTotal = 0;
	bool bPendingIsKeepalive = false;
	bool bPendingIsFrame = false;
	double LastSendTime = 0.0;
	double LastProgressTime = 0.0;
	double LastPeerCheckTime = 0.0;

	std::atomic<int64> SendWaitCount{ 0 };
	std::atomic<int64> FramesSent{ 0 };
	std::atomic<int64> BytesSent{ 0 };
	/** Refused frames, frames dropped with a client, and frames lost to a stall. */
	std::atomic<int64> DroppedFrames{ 0 };
	/** Queue counters at Initialize, so GetStats reports this session only. */
	int64 MocapDroppedBaseline = 0;
	int64 AudioBytesBaseline = 0;
	/** Frames discarded by Stop/Start drains: not counted as dropped (as before). */
	std::atomic<int64> MocapDrained{ 0 };

	/** Set by a successful Start(), cleared by Stop(); sends outside a session return NotRunning. */
	std::atomic<bool> bRunning{ false };

	/**
	 * ADR 0007 item 3: Connecting while listening without a receiver, Connected while one is
	 * connected (set by the worker on accept), Reconnecting after it went away.
	 */
	FO3DConnectionStateTracker ConnectionState;

	/** SetPeerJoinedCallback's callback; set on any thread, called on the worker. */
	FCriticalSection PeerJoinedLock;
	FO3DPeerJoinedCallback PeerJoinedCallback;
};
