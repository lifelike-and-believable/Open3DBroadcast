#include "LoopbackSender.h"

#include "Logging/LogMacros.h"
#include "HAL/PlatformTime.h"

#include "o3ds/model.h"

#include <vector>

bool FO3DLoopbackSender::Initialize(const FO3DTransportConfig& Config)
{
    QueueCapacity = O3DLoopback::ResolveQueueCapacity(Config);
    ChannelKey = O3DLoopback::ResolveChannelKey(Config);

    Channel = O3DLoopback::AcquireChannel(ChannelKey, QueueCapacity);
    bInitialized = Channel.IsValid();
    Stats.Reset();

    if (!bInitialized)
    {
        UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("Loopback sender failed to acquire channel '%s'."), *ChannelKey);
    }

    return bInitialized;
}

bool FO3DLoopbackSender::Start()
{
    return bInitialized;
}

void FO3DLoopbackSender::Stop()
{
    // No persistent state required; channel remains available for new instances.
}

bool FO3DLoopbackSender::Send(const O3DS::SubjectList& List)
{
    if (!bInitialized || !Channel.IsValid())
    {
        return false;
    }

    if (Channel->PendingCount.load() >= Channel->Capacity)
    {
        Stats.DroppedFrames++;
        UE_LOG(LogO3DLoopbackTransport, Verbose, TEXT("Loopback queue full for '%s'; dropping frame."), *ChannelKey);
        return false;
    }

    FString SubjectName = ChannelKey;
    if (!List.mItems.empty() && List.mItems[0])
    {
        SubjectName = UTF8_TO_TCHAR(List.mItems[0]->mName.c_str());
    }

    std::vector<char> Buffer;
    const double TimestampSeconds = FPlatformTime::Seconds();

    int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);
    if (BytesWritten <= 0)
    {
        UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("Loopback sender failed to serialize subject '%s'."), *SubjectName);
        return false;
    }

    FO3DLoopbackPacket Packet;
    Packet.Subject = SubjectName;
    Packet.TimestampSeconds = TimestampSeconds;
    Packet.Payload.SetNumUninitialized(BytesWritten);
    FMemory::Memcpy(Packet.Payload.GetData(), Buffer.data(), BytesWritten);

    Channel->Queue.Enqueue(MoveTemp(Packet));
    Channel->PendingCount.fetch_add(1);

    Stats.FramesSent++;
    Stats.BytesSent += BytesWritten;

    return true;
}

void FO3DLoopbackSender::Tick(float DeltaSeconds)
{
    // Loopback sender has no background work.
}

FO3DTransportStats FO3DLoopbackSender::GetStats() const
{
    return Stats;
}
