#include "LoopbackReceiver.h"

#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

bool FO3DLoopbackReceiver::Initialize(const FO3DTransportConfig& Config)
{
    QueueCapacity = O3DLoopback::ResolveQueueCapacity(Config);
    ChannelKey = O3DLoopback::ResolveChannelKey(Config);

    Channel = O3DLoopback::AcquireChannel(ChannelKey, QueueCapacity);
    bInitialized = Channel.IsValid();
    Stats.Reset();
    LatencySamples = 0;

    if (!bInitialized)
    {
        UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("Loopback receiver failed to acquire channel '%s'."), *ChannelKey);
    }

    return bInitialized;
}

bool FO3DLoopbackReceiver::Start(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
    Consumer = InConsumer;

    if (!Consumer.IsValid())
    {
        Consumer = FSerializedFrameConsumerRegistry::Create();
        if (!Consumer.IsValid())
        {
            UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("No serialized frame consumer registered; loopback frames will be dropped."));
        }
    }
    return bInitialized;
}

void FO3DLoopbackReceiver::Stop()
{
    Consumer.Reset();
}

int32 FO3DLoopbackReceiver::Poll()
{
    if (!bInitialized || !Channel.IsValid())
    {
        return 0;
    }

    int32 Processed = 0;
    FO3DLoopbackPacket Packet;

    while (Channel->Queue.Dequeue(Packet))
    {
        Channel->PendingCount.fetch_sub(1);
        ++Processed;

        Stats.FramesReceived++;
        Stats.BytesReceived += Packet.Payload.Num();

        const double NowSeconds = FPlatformTime::Seconds();
        const double LatencyMs = (NowSeconds - Packet.TimestampSeconds) * 1000.0;
        AccumulateLatency(LatencyMs);

        if (Consumer.IsValid())
        {
            Consumer->SubmitFrame(Packet.Subject, Packet.Payload, Packet.TimestampSeconds);
        }
    }

    return Processed;
}

FO3DTransportStats FO3DLoopbackReceiver::GetStats() const
{
    return Stats;
}

void FO3DLoopbackReceiver::AccumulateLatency(double LatencyMs)
{
    Stats.MaxLatencyMs = FMath::Max(Stats.MaxLatencyMs, LatencyMs);

    const int64 NewSampleCount = LatencySamples + 1;
    const double PreviousTotal = Stats.AverageLatencyMs * LatencySamples;
    Stats.AverageLatencyMs = (PreviousTotal + LatencyMs) / FMath::Max<int64>(1, NewSampleCount);
    LatencySamples = NewSampleCount;
}
