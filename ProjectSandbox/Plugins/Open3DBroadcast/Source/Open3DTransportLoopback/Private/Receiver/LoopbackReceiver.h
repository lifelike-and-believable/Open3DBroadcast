// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DConnectionState.h"
#include "../Shared/LoopbackChannel.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "O3DAudioFrameCodec.h"

class FO3DLoopbackReceiver : public IOpen3DReceiver
{
public:
    virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
    virtual FO3DTransportResult Start() override;
    virtual void Stop() override;
    virtual int32 Poll() override;
    virtual FO3DTransportStats GetStats() const override;
    virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DLoopback::GetCapabilities(FO3DTransportConfig()); }
    virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
    virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
    virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
    virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override { ControlSink = Sink; }

private:
    FString ChannelKey;
    int32 QueueCapacity = 64;
    int32 AudioQueueCapacity = 32;
    TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> Channel;
    TSharedPtr<ISerializedFrameConsumer> Consumer;
    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
    /** Control payloads (ADR 0011). Held strongly, released in Stop; called from Poll. */
    TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe> ControlSink;
    FO3DTransportAudioConfig ActiveAudioConfig;
    bool bInitialized = false;
    FO3DTransportStats Stats;
    int64 LatencySamples = 0;
    double LastAudioDropLogTime = 0.0;
    O3DAudio::FMultiStreamFrameDecoder AudioDecoder; // SHR-15: one decoder per (SourceGuid, StreamLabel)
    TArray<int16> DecodedPcmScratch;
    /** ADR 0007 item 3: Connected from Start to Stop (the channel needs no peer). */
    FO3DConnectionStateTracker ConnectionState;

    void AccumulateLatency(double LatencyMs);
};
