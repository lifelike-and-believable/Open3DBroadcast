#pragma once

#include "O3DSenderInterface.h"
#include "LoopbackChannel.h"
#include "O3DAudioFrameCodec.h"

class FO3DLoopbackSender : public IOpen3DSender
{
public:
    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual bool Send(const O3DS::SubjectList& List) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override;
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
    friend class FLoopbackSenderAudioSink;

    FString ChannelKey;
    int32 QueueCapacity = 64;
    int32 AudioQueueCapacity = 32;
    TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> Channel;
    bool bInitialized = false;
    bool bAudioEncoderInitialized = false;
    FO3DTransportAudioConfig ActiveAudioConfig;
    O3DAudio::FFrameEncoder AudioEncoder;
    FO3DTransportStats Stats;

    bool EncodeAudioFrame(const FString& StreamLabelOverride,
        const FString& SubjectOverride,
        const float* Interleaved,
        int32 NumFrames,
        int32 NumChannels,
        int32 SampleRate,
        double TimestampSec,
        O3DAudio::FEncodedFrame& OutFrame);
};
