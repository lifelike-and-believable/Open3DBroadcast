// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DUnifiedReceiveDemux.h"
#include "../Shared/LoopbackChannel.h"

/**
 * Loopback receiver: Poll() is the single consumer of its channel's FO3DSendQueue and hands each
 * item to the shared FO3DUnifiedReceiveDemux (ADR 0007 item 7, WP-A1 step 4). Mocap goes to the
 * consumer without a copy; audio and control envelopes are classified by the demux.
 *
 * Threading: everything on the game thread, except GetStats and GetConnectionState (any thread).
 */
class FO3DLoopbackReceiver : public IOpen3DReceiver
{
public:
	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override { Demux.SetConsumer(Consumer); }
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DLoopback::GetCapabilities(FO3DTransportConfig()); }
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& /*AudioConfig*/) override { Demux.SetAudioSink(Sink); }
	virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override { Demux.SetControlSink(Sink); }

private:
	void AccumulateLatency(double LatencyMs);

	FString ChannelKey;
	TSharedPtr<FO3DSendQueue, ESPMode::ThreadSafe> Channel;
	/** Holds the consumer and sinks strongly; Stop() releases them (TRF-38, ADR 0011). */
	FO3DUnifiedReceiveDemux Demux;
	bool bInitialized = false;
	/** Written by Poll (game thread); GetStats copies it on the same thread in practice. */
	FO3DTransportStats Stats;
	int64 LatencySamples = 0;
	double LastAudioLogTime = 0.0;
	/** ADR 0007 item 3: Connected from Start to Stop (the channel needs no peer). */
	FO3DConnectionStateTracker ConnectionState;
};
