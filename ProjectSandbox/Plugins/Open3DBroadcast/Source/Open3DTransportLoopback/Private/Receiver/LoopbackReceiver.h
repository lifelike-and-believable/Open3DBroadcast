// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "Transport/O3DReceiverInterface.h"
#include "../Shared/LoopbackChannel.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "O3DAudioFrameCodec.h"

class FO3DLoopbackReceiver : public IOpen3DReceiver
{
public:
    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual int32 Poll() override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override;
    virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
    virtual bool SupportsControl() const override { return true; }
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

    void AccumulateLatency(double LatencyMs);
};
