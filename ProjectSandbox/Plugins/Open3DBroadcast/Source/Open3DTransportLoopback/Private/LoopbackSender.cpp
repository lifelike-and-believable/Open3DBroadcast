#include "LoopbackSender.h"

#include "Logging/LogMacros.h"
#include "HAL/PlatformTime.h"

#include "o3ds/model.h"

#include <vector>

namespace
{
    class FLoopbackSenderAudioSink final : public IO3DSenderAudioSink
    {
    public:
        FLoopbackSenderAudioSink(TWeakPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> InChannel,
                                 FString InChannelKey)
            : Channel(MoveTemp(InChannel))
            , ChannelKey(MoveTemp(InChannelKey))
        {
        }

        virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
        {
            if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
            {
                return false;
            }

            TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> PinnedChannel = Channel.Pin();
            if (!PinnedChannel.IsValid())
            {
                return false;
            }

            const int32 PendingAudio = PinnedChannel->AudioPendingCount.load();
            if (PendingAudio >= PinnedChannel->AudioCapacity)
            {
                const double Now = FPlatformTime::Seconds();
                if (Now - LastDropLogTime > 1.0)
                {
                    UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("Loopback audio queue full for '%s'; dropping frame."), *ChannelKey);
                    LastDropLogTime = Now;
                }
                return false;
            }

            const int32 NumSamples = NumFrames * NumChannels;
            if (NumSamples <= 0)
            {
                return false;
            }

            TArray<uint8> Encoded;
            Encoded.SetNumUninitialized(NumSamples * sizeof(int16));
            int16* Dest = reinterpret_cast<int16*>(Encoded.GetData());

            for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
            {
                const float Clamped = FMath::Clamp(Interleaved[SampleIndex], -1.0f, 1.0f);
                const int32 Scaled = FMath::RoundToInt(Clamped * 32767.0f);
                Dest[SampleIndex] = static_cast<int16>(FMath::Clamp(Scaled, -32768, 32767));
            }

            const FString SubjectForAudio = PinnedChannel->GetLastSubjectName();

            FO3DLoopbackAudioPacket Packet;
            Packet.Payload = MoveTemp(Encoded);
            Packet.TimestampSeconds = TimestampSec;
            Packet.Codec = O3DS::EUnifiedCodec::PCM16;
            Packet.Meta.StreamLabel = StreamLabel.IsEmpty() ? ChannelKey : StreamLabel;
            Packet.Meta.SubjectName = SubjectForAudio.IsEmpty() ? ChannelKey : SubjectForAudio;
            Packet.Meta.SampleRate = SampleRate;
            Packet.Meta.NumChannels = NumChannels;
            Packet.Meta.TimestampSec = TimestampSec;

            PinnedChannel->AudioQueue.Enqueue(MoveTemp(Packet));
            PinnedChannel->AudioPendingCount.fetch_add(1);

            const int32 DebugLevel = O3DLoopback::GetAudioDebugLevel();
            if (DebugLevel > 0)
            {
                const double Now = FPlatformTime::Seconds();
                if (DebugLevel > 1 || Now - LastEnqueueLogTime > 0.25)
                {
                    const int32 PendingNow = PinnedChannel->AudioPendingCount.load();
                    const FString EffectiveLabel = StreamLabel.IsEmpty() ? ChannelKey : StreamLabel;
                    UE_LOG(LogO3DLoopbackTransport, Log, TEXT("Loopback audio enqueued channel='%s' label='%s' frames=%d channels=%d sr=%d pending=%d timestamp=%.3f"),
                        *ChannelKey,
                        *EffectiveLabel,
                        NumFrames,
                        NumChannels,
                        SampleRate,
                        PendingNow,
                        TimestampSec);
                    LastEnqueueLogTime = Now;
                }
            }

            return true;
        }

        virtual void OnCaptureStopped() override
        {
            // No persistent state to release.
        }

    private:
        TWeakPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> Channel;
        FString ChannelKey;
        double LastDropLogTime = 0.0;
        double LastEnqueueLogTime = 0.0;
    };
}

bool FO3DLoopbackSender::Initialize(const FO3DTransportConfig& Config)
{
    QueueCapacity = O3DLoopback::ResolveQueueCapacity(Config);
    AudioQueueCapacity = O3DLoopback::ResolveAudioQueueCapacity(Config);
    ChannelKey = O3DLoopback::ResolveChannelKey(Config);

    Channel = O3DLoopback::AcquireChannel(ChannelKey, QueueCapacity, AudioQueueCapacity);
    bInitialized = Channel.IsValid();
    Stats.Reset();

    if (!bInitialized)
    {
        UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("Loopback sender failed to acquire channel '%s'."), *ChannelKey);
    }
    else
    {
        const int32 DebugLevel = O3DLoopback::GetAudioDebugLevel();
        if (DebugLevel > 0)
        {
            UE_LOG(LogO3DLoopbackTransport, Log, TEXT("Loopback sender initialized channel='%s' queueCapacity=%d audioCapacity=%d"),
                *ChannelKey,
                QueueCapacity,
                AudioQueueCapacity);
        }
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

    Channel->SetLastSubjectName(SubjectName);

    Channel->Queue.Enqueue(MoveTemp(Packet));
    Channel->PendingCount.fetch_add(1);

    Stats.FramesSent++;
    Stats.BytesSent += BytesWritten;

    if (O3DLoopback::GetAudioDebugLevel() > 1)
    {
        UE_LOG(LogO3DLoopbackTransport, Verbose, TEXT("Loopback subject enqueued channel='%s' subject='%s' bytes=%d pending=%d"),
            *ChannelKey,
            *SubjectName,
            BytesWritten,
            Channel->PendingCount.load());
    }

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

bool FO3DLoopbackSender::SupportsAudio() const
{
    return true;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DLoopbackSender::CreateAudioSink(const FO3DTransportAudioConfig& /*AudioConfig*/)
{
    if (!bInitialized || !Channel.IsValid())
    {
        return nullptr;
    }

    return MakeShared<FLoopbackSenderAudioSink, ESPMode::ThreadSafe>(Channel, ChannelKey);
}
