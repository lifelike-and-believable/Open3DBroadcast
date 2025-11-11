#pragma once

#include "O3DSenderInterface.h"
#include "LoopbackChannel.h"

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
    FString ChannelKey;
    int32 QueueCapacity = 64;
    int32 AudioQueueCapacity = 32;
    TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> Channel;
    bool bInitialized = false;
    FO3DTransportStats Stats;
};
