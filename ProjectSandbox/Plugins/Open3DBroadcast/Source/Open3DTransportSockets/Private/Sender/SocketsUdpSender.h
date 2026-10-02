// Copyright Lifelike & Believable. All Rights Reserved.

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
class FInternetAddr;

/**
 * UDP sender on the shared transport blocks (ADR 0007 item 7, WP-A1 PR 4c).
 *
 * SendSerialized, SendControl and the audio sinks (FO3DQueuedSenderAudioSink) only enqueue on one
 * FO3DSendQueue; an FO3DTransportWorker owns the socket and sends every item: frames and audio as
 * one datagram, or fragmented with the core's udp_fragment above udp.maxdatagram (unchanged wire
 * format), and control as exactly one datagram. No caller's thread ever calls SendTo (TRB-20).
 *
 * Queue policy: UDP is unreliable and live mocap is worth more fresh than complete (ADR 0008
 * decision driver 4), so frames use EO3DMocapOverflow::DropOldest: with the worker behind, the
 * oldest frames are discarded and the newest sent; callers are refused only at the hard cap.
 * Audio and control have budgets of their own and are never dropped for frames.
 *
 * Destination: an IP literal is resolved in Initialize (no DNS); a host name is resolved on the
 * worker with O3DTransportOptions::ResolveHostPort, retried with FO3DReconnectPolicy until it
 * resolves (the connection state is Connecting until then).
 *
 * Threading: Initialize/Start/Stop/Tick/CreateAudioSink: game thread. Send, SendSerialized,
 * SendControl, GetStats: any thread, never block.
 */
class FO3DSocketsUdpSender : public IOpen3DSender
{
public:
	/** Frames that may wait for the worker before the oldest are dropped (the soft cap; callers are refused at twice it). */
	static constexpr int32 FrameQueueSoftCap = 4;
	/** Payload bytes of waiting frames before the oldest are dropped (soft cap). */
	static constexpr int64 FrameQueueSoftBytes = 16 * 1024 * 1024;
	/** Payload bytes of waiting audio (refused beyond it), as the WP-S5 audio queue had. */
	static constexpr int64 AudioQueueBytes = 1024 * 1024;

	FO3DSocketsUdpSender();
	virtual ~FO3DSocketsUdpSender() override;

	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual bool Send(const O3DS::SubjectList& List) override;
	virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DSockets::GetUdpCapabilities(FO3DTransportConfig()); }
	/** Connected while the socket exists (UDP needs no peer); Connecting while a host name resolves. */
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;
	virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len) override;

	/** Test hook: while paused the worker sends nothing, so the queue's drop policy can be observed. */
	void SetWorkerPausedForTesting(bool bPaused) { bWorkerPausedForTesting.store(bPaused); Queue->Wake(); }

private:
	/** Creates the socket for Addr. Game thread before the worker runs, or the worker. */
	FO3DTransportResult OpenSocket(const TSharedPtr<FInternetAddr>& Addr);
	void DestroySocket();
	/** Empties the queue (worker not running) and records how many frames that discarded. */
	void DrainQueue();
	EO3DSendResult EnqueueFrame(FO3DSendItem&& Item);

	// Worker thread only.
	uint32 RunWorkerIteration();
	bool SendPayload(const uint8* Data, int32 Size, const TCHAR* Context);
	bool SendDatagram(const uint8* Data, int32 Size, const TCHAR* Context);
	bool SendFragmented(const uint8* Data, int32 Size, const TCHAR* Context);

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	/** Owned by the worker while it runs; created in Start (literal destination) or by the worker. */
	FSocket* Socket = nullptr;
	TSharedPtr<FInternetAddr> RemoteAddr;

	FO3DHostPort Endpoint;
	FString StreamId;

	bool bAllowBroadcast = false;
	int32 MaxDatagramBytes = 64000;
	int32 MtuBytes = 1200;
	FGuid AudioSourceGuid;

	/** Fragment message ids (worker thread). */
	uint32 MessageCounter = 0;
	std::vector<char> SerializationScratch; // Send(SubjectList), game thread
	std::vector<char> FragmentScratch; // worker thread
	FO3DReconnectPolicy ResolveBackoff; // worker thread

	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue;
	/** Shared with the audio sinks; its peer flag mirrors "the socket exists". */
	const TSharedRef<FO3DAudioPublishState, ESPMode::ThreadSafe> PublishState;
	FO3DTransportWorker Worker;

	std::atomic<bool> bRunning{ false };
	std::atomic<bool> bWorkerPausedForTesting{ false };
	std::atomic<int64> FramesSent{ 0 };
	std::atomic<int64> BytesSent{ 0 };
	/** Frames refused at the hard cap and frames whose send failed. */
	std::atomic<int64> DroppedFrames{ 0 };
	std::atomic<int64> SendErrors{ 0 };
	int64 MocapDroppedBaseline = 0;
	std::atomic<int64> MocapDrained{ 0 };

	/** ADR 0007 item 3. */
	FO3DConnectionStateTracker ConnectionState;
};
