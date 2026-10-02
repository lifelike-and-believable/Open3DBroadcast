// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportWorker.h"
#include "Transport/O3DUnifiedReceiveDemux.h"
#include "../Shared/SocketsTransportCommon.h"

#include <atomic>

THIRD_PARTY_INCLUDES_START
#include "o3ds/tcp_stream_parser.h"
THIRD_PARTY_INCLUDES_END

class FSocket;
class ISocketSubsystem;

/**
 * TCP receiver - client mode (connects to sender).
 *
 * Built on the shared transport blocks (ADR 0007 item 7, WP-A1 PR 4b):
 * - An FO3DTransportWorker owns the socket. It resolves the host (O3DTransportOptions::
 *   ResolveHostPort, so a host name never blocks the game thread; TRB-26), connects, reconnects
 *   with FO3DReconnectPolicy (TRB-4), reads and frames the stream with the core
 *   O3DS::TcpStreamParser (WP-S6), and hands each frame payload to a bounded hand-off queue.
 *   When that queue is full the worker stops reading, so TCP flow control slows the sender
 *   instead of the receiver buffering without limit.
 * - Poll() (game thread) takes a bounded number of payloads off the queue (TRB-18) and gives them
 *   to the shared FO3DUnifiedReceiveDemux, which calls the consumer, the audio sink and the
 *   control sink. The consumer is only ever called from Poll.
 *
 * Connection state: Connecting from Start until connected, Connected while connected,
 * Reconnecting after it dropped; changes after Start happen on the worker thread.
 */
class FO3DSocketsTcpReceiver : public IOpen3DReceiver
{
public:
	FO3DSocketsTcpReceiver();
	virtual ~FO3DSocketsTcpReceiver() override;

	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) override { Demux.SetConsumer(InConsumer); }
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DSockets::GetTcpCapabilities(FO3DTransportConfig()); }
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
	virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override { Demux.SetControlSink(Sink); }

	/** True while a TCP connection to the sender is established. Any thread; for tests and diagnostics. */
	bool IsConnected() const { return bConnected.load(); }

	/** Number of connections established since Start(). Any thread; for tests and diagnostics. */
	int32 GetConnectCount() const { return ConnectCount.load(); }

	/** Consecutive failed connects since the last connection that carried data (the backoff count). Any thread. */
	int32 GetFailedConnectAttempts() const { return FailedConnectAttempts.load(); }

private:
	enum class EState : uint8
	{
		Disconnected,
		Connecting,
		Connected
	};

	// Worker thread only (or the game thread while the worker is stopped).
	uint32 RunWorkerIteration();
	bool ConnectToServer();
	void DisconnectSocket(bool bReportLoss);
	/** Reads and frames what the socket has, within the per-iteration bounds. False when the peer closed or the socket failed. */
	bool ReadAvailable();
	/** Hands a received payload to Poll; false when the hand-off queue is full (the payload is kept and retried). */
	bool HandOff(const uint8* Payload, int32 Size);
	void ReportParserStats();
	/** Records a failed or lost connection in the backoff policy (worker thread). */
	void NoteConnectFailure(double Now);

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	/** Owned by the worker while it runs. */
	FSocket* Socket = nullptr;
	/** Between a successful Start and Stop; read by the worker. */
	std::atomic<bool> bRunning{ false };

	FO3DHostPort RemoteEndpoint;
	FString StreamId;

	// Worker-thread state.
	EState State = EState::Disconnected;
	/** Socket-free framing (TRB-1, TRB-8, TRB-9). Reset on every new connection. */
	O3DS::TcpStreamParser Parser;
	uint64 ReportedDiscardedBytes = 0;
	uint64 ReportedRejectedFrames = 0;
	bool bWarnedResyncThisConnection = false;
	bool bReceivedOnThisConnection = false;
	double ConnectStartTime = 0.0;
	double LastDataReceiveTime = 0.0;
	/** A payload the full hand-off queue refused; retried before anything else is read. */
	TArray<uint8> PendingHandOff;
	FO3DReconnectPolicy Backoff;

	double ConnectionTimeoutSeconds = 5.0;
	double ConnectTimeoutSeconds = 5.0;
	double InitialBackoffSeconds = 0.5;
	double MaxBackoffSeconds = 5.0;

	/** Frame payloads from the worker to Poll (the generic bounded MPSC queue; items are received buffers). */
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> ReceiveQueue;
	FO3DTransportWorker Worker;
	/** Holds the consumer and sinks strongly; Stop() releases them (TRF-38, ADR 0011). Poll thread only. */
	FO3DUnifiedReceiveDemux Demux;

	std::atomic<bool> bConnected{ false };
	std::atomic<int32> ConnectCount{ 0 };
	std::atomic<int32> FailedConnectAttempts{ 0 };
	std::atomic<int64> FramesReceived{ 0 };
	std::atomic<int64> BytesReceived{ 0 };
	std::atomic<int64> DroppedFrames{ 0 };
	std::atomic<int64> ReceiveErrors{ 0 };

	/** ADR 0007 item 3. */
	FO3DConnectionStateTracker ConnectionState;
};
