#include "LoopbackSender.h"

#include "Logging/LogMacros.h"
#include "HAL/PlatformTime.h"
#include "O3DAudioSenderSink.h"

#include "o3ds/model.h"

#include <vector>

class FLoopbackSenderAudioSink final : public FO3DSenderAudioSinkBase
{
public:
    FLoopbackSenderAudioSink(FO3DLoopbackSender& InOwner,
                             TWeakPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> InChannel,
                             FString InChannelKey,
                             FO3DTransportAudioConfig InConfig)
        : FO3DSenderAudioSinkBase(MoveTemp(InConfig))
        , Owner(InOwner)
        , Channel(MoveTemp(InChannel))
        , ChannelKey(MoveTemp(InChannelKey))
    {
    }

    virtual bool OnSubmitPcmInternal(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
    {
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

        const FString SubjectForAudio = PinnedChannel->GetLastSubjectName();

        const FString LabelForPacket = StreamLabel.IsEmpty() ? ChannelKey : StreamLabel;

        O3DAudio::FEncodedFrame EncodedFrame;
        if (!Owner.EncodeAudioFrame(LabelForPacket, SubjectForAudio, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, EncodedFrame))
        {
            return false;
        }

        FO3DLoopbackAudioPacket Packet;
        Packet.Payload = MoveTemp(EncodedFrame.Encoded);
        Packet.TimestampSeconds = TimestampSec;
        Packet.Codec = EncodedFrame.Codec;
        Packet.Meta = MoveTemp(EncodedFrame.Meta);

        PinnedChannel->AudioQueue.Enqueue(MoveTemp(Packet));
        PinnedChannel->AudioPendingCount.fetch_add(1);

        const int32 DebugLevel = O3DLoopback::GetAudioDebugLevel();
        if (DebugLevel > 0)
        {
            const double Now = FPlatformTime::Seconds();
            if (DebugLevel > 1 || Now - LastEnqueueLogTime > 0.25)
            {
                const int32 PendingNow = PinnedChannel->AudioPendingCount.load();
                UE_LOG(LogO3DLoopbackTransport, Log, TEXT("Loopback audio enqueued channel='%s' label='%s' frames=%d channels=%d sr=%d pending=%d timestamp=%.3f"),
                    *ChannelKey,
                    *LabelForPacket,
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
    FO3DLoopbackSender& Owner;
    TWeakPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> Channel;
    FString ChannelKey;
    double LastDropLogTime = 0.0;
    double LastEnqueueLogTime = 0.0;
};

bool FO3DLoopbackSender::Initialize(const FO3DTransportConfig& Config)
{
    QueueCapacity = O3DLoopback::ResolveQueueCapacity(Config);
    AudioQueueCapacity = O3DLoopback::ResolveAudioQueueCapacity(Config);
    ChannelKey = O3DLoopback::ResolveChannelKey(Config);

    Channel = O3DLoopback::AcquireChannel(ChannelKey, QueueCapacity, AudioQueueCapacity);
    bInitialized = Channel.IsValid();
    Stats.Reset();
    ActiveAudioConfig = Config.Audio;
    const FString DefaultLabel = ActiveAudioConfig.StreamLabel.IsEmpty() ? ChannelKey : ActiveAudioConfig.StreamLabel;
    ActiveAudioConfig.StreamLabel = DefaultLabel.IsEmpty() ? ChannelKey : DefaultLabel;
    bAudioEncoderInitialized = AudioEncoder.Initialize(ActiveAudioConfig, ActiveAudioConfig.StreamLabel, ChannelKey);

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

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DLoopbackSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
    if (!bInitialized || !Channel.IsValid())
    {
        return nullptr;
    }

    ActiveAudioConfig = AudioConfig;
    const FString DefaultLabel = ActiveAudioConfig.StreamLabel.IsEmpty() ? ChannelKey : ActiveAudioConfig.StreamLabel;
    ActiveAudioConfig.StreamLabel = DefaultLabel.IsEmpty() ? ChannelKey : DefaultLabel;
    bAudioEncoderInitialized = AudioEncoder.Initialize(ActiveAudioConfig, ActiveAudioConfig.StreamLabel, ChannelKey);

    return MakeShared<FLoopbackSenderAudioSink, ESPMode::ThreadSafe>(*this, Channel, ChannelKey, ActiveAudioConfig);
}

bool FO3DLoopbackSender::EncodeAudioFrame(const FString& StreamLabelOverride,
    const FString& SubjectOverride,
    const float* Interleaved,
    int32 NumFrames,
    int32 NumChannels,
    int32 SampleRate,
    double TimestampSec,
    O3DAudio::FEncodedFrame& OutFrame)
{
    if (!bAudioEncoderInitialized)
    {
        return false;
    }

    const FString EffectiveSubject = SubjectOverride.IsEmpty() ? ChannelKey : SubjectOverride;
    return AudioEncoder.BuildEncodedFrame(StreamLabelOverride, EffectiveSubject, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, OutFrame);
}
