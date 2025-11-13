#pragma once

#include "O3DReceiverInterface.h"
#include "LoopbackChannel.h"
#include "SerializedFrameConsumerRegistry.h"
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

private:
    FString ChannelKey;
    int32 QueueCapacity = 64;
    int32 AudioQueueCapacity = 32;
    TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> Channel;
    TSharedPtr<ISerializedFrameConsumer> Consumer;
    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
    FO3DTransportAudioConfig ActiveAudioConfig;
    bool bInitialized = false;
    FO3DTransportStats Stats;
    int64 LatencySamples = 0;
    double LastAudioDropLogTime = 0.0;
    O3DAudio::FFrameDecoder AudioDecoder;
    TArray<int16> DecodedPcmScratch;

    void AccumulateLatency(double LatencyMs);
};
