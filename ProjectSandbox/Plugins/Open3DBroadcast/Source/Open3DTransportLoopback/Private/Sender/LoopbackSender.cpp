// Copyright Lifelike & Believable. All Rights Reserved.

#include "LoopbackSender.h"

#include "Logging/LogMacros.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "O3DSenderAudioSinkBase.h"

#include <atomic>

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

/**
 * Loopback audio sink (WP-S5, TRB-30). Holds the sender's lifetime gate, a weak channel
 * reference and its own encoders; it never references the sender.
 */
class FLoopbackSenderAudioSink final : public FO3DGatedSenderAudioSink
{
public:
    FLoopbackSenderAudioSink(TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> InGate,
                             TWeakPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> InChannel,
                             FString InChannelKey,
                             FO3DTransportAudioConfig InConfig,
                             FO3DSinkAudioEncoder::FSettings InEncoderSettings)
        : FO3DGatedSenderAudioSink(MoveTemp(InConfig), MoveTemp(InGate), MoveTemp(InEncoderSettings))
        , Channel(MoveTemp(InChannel))
        , ChannelKey(MoveTemp(InChannelKey))
    {
    }

    virtual bool OnSubmitGated(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
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
            if (Now - LastDropLogTime.load() > 1.0)
            {
                #if !WITH_DEV_AUTOMATION_TESTS
                UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("Loopback audio queue full for '%s'; dropping frame."), *ChannelKey);
                #endif
                LastDropLogTime.store(Now);
            }
            return false;
        }

        const FString SubjectForAudio = PinnedChannel->GetLastSubjectName();

        const FString LabelForPacket = StreamLabel.IsEmpty() ? ChannelKey : StreamLabel;

        // Opus may return zero or several packets per buffer (SHR-2).
        TArray<O3DAudio::FEncodedFrame> EncodedFrames;
        if (!GetEncoder().Encode(LabelForPacket, SubjectForAudio, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, EncodedFrames))
        {
            return false;
        }

        for (O3DAudio::FEncodedFrame& EncodedFrame : EncodedFrames)
        {
            FO3DLoopbackAudioPacket Packet;
            Packet.Payload = MoveTemp(EncodedFrame.Encoded);
            Packet.TimestampSeconds = EncodedFrame.Meta.TimestampSec;
            Packet.Codec = EncodedFrame.Codec;
            Packet.Meta = MoveTemp(EncodedFrame.Meta);

            PinnedChannel->AudioQueue.Enqueue(MoveTemp(Packet));
            PinnedChannel->AudioPendingCount.fetch_add(1);
        }

        const int32 DebugLevel = O3DLoopback::GetAudioDebugLevel();
        if (DebugLevel > 0)
        {
            const double Now = FPlatformTime::Seconds();
            if (DebugLevel > 1 || Now - LastEnqueueLogTime.load() > 0.25)
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
                LastEnqueueLogTime.store(Now);
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
    const FString ChannelKey;
    std::atomic<double> LastDropLogTime{0.0};
    std::atomic<double> LastEnqueueLogTime{0.0};
};

FO3DLoopbackSender::~FO3DLoopbackSender()
{
    AudioGate->Close();
}

bool FO3DLoopbackSender::Initialize(const FO3DTransportConfig& Config)
{
    QueueCapacity = O3DLoopback::ResolveQueueCapacity(Config);
    AudioQueueCapacity = O3DLoopback::ResolveAudioQueueCapacity(Config);
    ChannelKey = O3DLoopback::ResolveChannelKey(Config);

    Channel = O3DLoopback::AcquireChannel(ChannelKey, QueueCapacity, AudioQueueCapacity);
    bInitialized = Channel.IsValid();
    {
        FScopeLock StatsLock(&StatsMutex);
        Stats.Reset();
    }
    ActiveAudioConfig = Config.Audio;
    AudioSourceGuid = FGuid::NewGuid();
    if (bInitialized)
    {
        AudioGate->Open();
    }

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
    if (bInitialized)
    {
        AudioGate->Open();
    }
    bRunning.store(bInitialized);
    return bInitialized;
}

void FO3DLoopbackSender::Stop()
{
    // WP-S5: after this returns no audio sink created so far can enqueue again.
    // The channel itself remains available for new instances.
    AudioGate->Close();
    bRunning.store(false);
}

bool FO3DLoopbackSender::Send(const O3DS::SubjectList& List)
{
    if (!bInitialized || !bRunning.load() || !Channel.IsValid())
    {
        return false;
    }

    if (Channel->PendingCount.load() >= Channel->Capacity)
    {
        {
            FScopeLock StatsLock(&StatsMutex);
            Stats.DroppedFrames++;
        }
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

    return SendBytes(reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten, SubjectName, TimestampSeconds);
}

bool FO3DLoopbackSender::SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec)
{
    if (!bInitialized || !bRunning.load() || !Channel.IsValid() || Len <= 0)
    {
        return false;
    }

    if (Channel->PendingCount.load() >= Channel->Capacity)
    {
        {
            FScopeLock StatsLock(&StatsMutex);
            Stats.DroppedFrames++;
        }
        UE_LOG(LogO3DLoopbackTransport, Verbose, TEXT("Loopback queue full for '%s'; dropping frame."), *ChannelKey);
        return false;
    }

    return SendBytes(Data, Len, SubjectName.IsEmpty() ? ChannelKey : SubjectName, CaptureTimestampSec);
}

/** Control (ADR 0011): onto the channel's control queue, independent of frames and audio. Not counted as a frame. */
bool FO3DLoopbackSender::SendControl(const uint8* Envelope, int32 Len)
{
    TConstArrayView<uint8> Payload;
    if (!bInitialized || !bRunning.load() || !Channel.IsValid() || !O3DS::TryGetControlPayload(Envelope, Len, Payload))
    {
        return false;
    }
    if (Channel->ControlPendingCount.load() >= FO3DLoopbackChannel::ControlCapacity)
    {
        return false;
    }
    Channel->ControlQueue.Enqueue(TArray<uint8>(Envelope, Len));
    Channel->ControlPendingCount.fetch_add(1);
    return true;
}

/** Enqueue an already-serialized payload onto the loopback channel and record stats/subject bookkeeping.
 *  CaptureTimestampSec is the same value the caller already embedded in Data (Send()'s own
 *  FPlatformTime::Seconds() call, or FO3DSenderSerializer's `Now` via SendSerialized()) - reused here
 *  rather than sampling a fresh clock read, so the queued packet's local timestamp never drifts from
 *  what's actually encoded in the payload. */
bool FO3DLoopbackSender::SendBytes(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec)
{
    FO3DLoopbackPacket Packet;
    Packet.Subject = SubjectName;
    Packet.TimestampSeconds = CaptureTimestampSec;
    Packet.Payload.SetNumUninitialized(Len);
    FMemory::Memcpy(Packet.Payload.GetData(), Data, Len);

    Channel->SetLastSubjectName(SubjectName);

    Channel->Queue.Enqueue(MoveTemp(Packet));
    Channel->PendingCount.fetch_add(1);

    {
        FScopeLock StatsLock(&StatsMutex);
        Stats.FramesSent++;
        Stats.BytesSent += Len;
    }

    if (O3DLoopback::GetAudioDebugLevel() > 1)
    {
        UE_LOG(LogO3DLoopbackTransport, Verbose, TEXT("Loopback subject enqueued channel='%s' subject='%s' bytes=%d pending=%d"),
            *ChannelKey,
            *SubjectName,
            Len,
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
    FScopeLock StatsLock(&StatsMutex);
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

    // Immutable snapshot for this sink's own encoders (TRB-11): nothing reconfigures them later.
    FO3DSinkAudioEncoder::FSettings EncoderSettings;
    EncoderSettings.Config = ActiveAudioConfig;
    EncoderSettings.DefaultStreamLabel = ChannelKey;
    EncoderSettings.DefaultSubject = ChannelKey;
    EncoderSettings.SourceGuid = AudioSourceGuid;

    return MakeShared<FLoopbackSenderAudioSink, ESPMode::ThreadSafe>(AudioGate, Channel, ChannelKey, ActiveAudioConfig, MoveTemp(EncoderSettings));
}
