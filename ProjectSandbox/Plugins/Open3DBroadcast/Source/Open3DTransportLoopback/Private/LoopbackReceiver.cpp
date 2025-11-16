#include "LoopbackReceiver.h"

#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

bool FO3DLoopbackReceiver::Initialize(const FO3DTransportConfig& Config)
{
    QueueCapacity = O3DLoopback::ResolveQueueCapacity(Config);
    AudioQueueCapacity = O3DLoopback::ResolveAudioQueueCapacity(Config);
    ChannelKey = O3DLoopback::ResolveChannelKey(Config);

    Channel = O3DLoopback::AcquireChannel(ChannelKey, QueueCapacity, AudioQueueCapacity);
    bInitialized = Channel.IsValid();
    Stats.Reset();
    LatencySamples = 0;
    LastAudioDropLogTime = 0.0;

    if (!bInitialized)
    {
        UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("Loopback receiver failed to acquire channel '%s'."), *ChannelKey);
    }

    return bInitialized;
}

void FO3DLoopbackReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
    Consumer = InConsumer;
}

bool FO3DLoopbackReceiver::Start()
{
    if (!Consumer.IsValid())
    {
        Consumer = FSerializedFrameConsumerRegistry::Create();
        if (!Consumer.IsValid())
        {
            #if !WITH_DEV_AUTOMATION_TESTS
            UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("No serialized frame consumer registered; loopback frames will be dropped."));
            #endif  
        }
    }
    return bInitialized;
}

void FO3DLoopbackReceiver::Stop()
{
    Consumer.Reset();
    AudioSink.Reset();
}

int32 FO3DLoopbackReceiver::Poll()
{
    if (!bInitialized || !Channel.IsValid())
    {
        return 0;
    }

    const int32 DebugLevel = O3DLoopback::GetAudioDebugLevel();

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

    int32 AudioProcessed = 0;
    FO3DLoopbackAudioPacket AudioPacket;
    while (Channel->AudioQueue.Dequeue(AudioPacket))
    {
        Channel->AudioPendingCount.fetch_sub(1);
        ++AudioProcessed;
        Stats.BytesReceived += AudioPacket.Payload.Num();

        if (AudioSink.IsValid())
        {
            if (AudioPacket.Codec == O3DS::EUnifiedCodec::PCM16)
            {
                AudioSink->SubmitPcm16(AudioPacket.Meta, AudioPacket.Payload.GetData(), AudioPacket.Payload.Num());

                if (DebugLevel > 0)
                {
                    static double LastAudioLogTime = 0.0;
                    const double Now = FPlatformTime::Seconds();
                    if (DebugLevel > 1 || Now - LastAudioLogTime > 0.25)
                    {
                        UE_LOG(LogO3DLoopbackTransport, Log, TEXT("Loopback audio dequeued channel='%s' label='%s' bytes=%d sr=%d ch=%d pending=%d"),
                            *ChannelKey,
                            *AudioPacket.Meta.StreamLabel,
                            AudioPacket.Payload.Num(),
                            AudioPacket.Meta.SampleRate,
                            AudioPacket.Meta.NumChannels,
                            Channel->AudioPendingCount.load());
                        LastAudioLogTime = Now;
                    }
                }
            }
			else
			{
				if (AudioDecoder.Decode(AudioPacket.Codec, AudioPacket.Meta, AudioPacket.Payload.GetData(), AudioPacket.Payload.Num(), DecodedPcmScratch) && DecodedPcmScratch.Num() > 0)
				{
					AudioSink->SubmitPcm16(AudioPacket.Meta, reinterpret_cast<const uint8*>(DecodedPcmScratch.GetData()), DecodedPcmScratch.Num() * sizeof(int16));
				}
				else
				{
					UE_LOG(LogO3DLoopbackTransport, Verbose, TEXT("Loopback audio decode failed for codec '%d'."), static_cast<int32>(AudioPacket.Codec));
				}
			}
        }
        else
        {
            const double Now = FPlatformTime::Seconds();
            if (Now - LastAudioDropLogTime > 1.0)
            {
                UE_LOG(LogO3DLoopbackTransport, Verbose, TEXT("Loopback audio frame discarded (no sink) for '%s'; label='%s' bytes=%d"),
                    *ChannelKey,
                    *AudioPacket.Meta.StreamLabel,
                    AudioPacket.Payload.Num());
                LastAudioDropLogTime = Now;
            }
        }
    }

    return Processed + AudioProcessed;
}

FO3DTransportStats FO3DLoopbackReceiver::GetStats() const
{
    return Stats;
}

bool FO3DLoopbackReceiver::SupportsAudio() const
{
    return true;
}

void FO3DLoopbackReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
    AudioSink = Sink;
    ActiveAudioConfig = AudioConfig;
}

void FO3DLoopbackReceiver::AccumulateLatency(double LatencyMs)
{
    Stats.MaxLatencyMs = FMath::Max(Stats.MaxLatencyMs, LatencyMs);

    const int64 NewSampleCount = LatencySamples + 1;
    const double PreviousTotal = Stats.AverageLatencyMs * LatencySamples;
    Stats.AverageLatencyMs = (PreviousTotal + LatencyMs) / FMath::Max<int64>(1, NewSampleCount);
    LatencySamples = NewSampleCount;
}
