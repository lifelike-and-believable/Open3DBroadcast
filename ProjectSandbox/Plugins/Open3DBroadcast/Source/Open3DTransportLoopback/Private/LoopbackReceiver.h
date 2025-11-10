#pragma once

#include "O3DReceiverInterface.h"
#include "LoopbackChannel.h"
#include "O3DLoopback.h"

class FO3DLoopbackReceiver : public IOpen3DReceiver
{
public:
    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual bool Start(const TSharedPtr<ISerializedFrameConsumer>& Consumer = nullptr) override;
    virtual void Stop() override;
    virtual int32 Poll() override;
    virtual FO3DTransportStats GetStats() const override;

private:
    FString ChannelKey;
    int32 QueueCapacity = 64;
    TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> Channel;
    TSharedPtr<ISerializedFrameConsumer> Consumer;
    bool bInitialized = false;
    FO3DTransportStats Stats;
    int64 LatencySamples = 0;

    void AccumulateLatency(double LatencyMs);
};
