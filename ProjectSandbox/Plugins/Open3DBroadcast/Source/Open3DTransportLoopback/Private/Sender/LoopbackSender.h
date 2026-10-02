// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DSenderAudioSinkBase.h"
#include "../Shared/LoopbackChannel.h"

#include <atomic>

/**
 * Loopback sender: enqueues mocap, audio (through FO3DQueuedSenderAudioSink) and control items on
 * its channel's shared FO3DSendQueue (ADR 0007 item 7, WP-A1 step 4). There is no worker: the
 * receiver's Poll consumes the queue in process.
 *
 * Threading: Initialize, Start, Stop, Tick, CreateAudioSink: game thread. Send, SendSerialized,
 * SendControl, GetStats: any thread; they never block.
 */
class FO3DLoopbackSender : public IOpen3DSender
{
public:
	virtual ~FO3DLoopbackSender() override;

	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override;
	virtual void Tick(float /*DeltaSeconds*/) override {}
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DLoopback::GetCapabilities(FO3DTransportConfig()); }
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;
	virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len) override;

private:
	/** Enqueues one mocap frame and counts it (sent or dropped). Any thread. */
	EO3DSendResult EnqueueFrame(TArray<uint8>&& Bytes, FString SubjectName, double CaptureTimeSec, bool bFullSync);

	FString ChannelKey;
	/** The channel; set by Initialize (game thread) and read by senders only while running. */
	TSharedPtr<FO3DSendQueue, ESPMode::ThreadSafe> Channel;
	/**
	 * WP-S5: audio sinks hold this (never the sender). A new one per Initialize, so a sink of an
	 * earlier session can never reach a later session's channel; Stop() closes it.
	 */
	TSharedPtr<FO3DAudioPublishState, ESPMode::ThreadSafe> PublishState;
	bool bInitialized = false;
	/** Set by Start(), cleared by Stop(). Sends outside a session are rejected (ADR 0007 contract; WP-T2 conformance). */
	std::atomic<bool> bRunning{ false };
	FO3DTransportAudioConfig ActiveAudioConfig;
	FGuid AudioSourceGuid;
	/** Lock-free counters; GetStats() reads them (WP-T2 conformance Stats.MonotonicUnderLoad). */
	std::atomic<int64> FramesSent{ 0 };
	std::atomic<int64> BytesSent{ 0 };
	std::atomic<int64> DroppedFrames{ 0 };
	/** ADR 0007 item 3: Connected from Start to Stop (the channel needs no peer). */
	FO3DConnectionStateTracker ConnectionState;
};
