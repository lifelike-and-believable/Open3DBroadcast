// Copyright (c) Open3DStream Contributors

#if WITH_DEV_AUTOMATION_TESTS

#include "../Sender/LoopbackSender.h"
#include "../Receiver/LoopbackReceiver.h"

#include "Misc/AutomationTest.h"

namespace
{
    class FTestReceiverAudioSink final : public IO3DReceiverAudioSink
    {
    public:
        virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& InMeta, const uint8* Data, int32 NumBytes) override
        {
            Meta = InMeta;
            Payload.Reset();
            if (Data && NumBytes > 0)
            {
                Payload.AddUninitialized(NumBytes);
                FMemory::Memcpy(Payload.GetData(), Data, NumBytes);
            }
            bInvoked = true;
        }

        bool WasInvoked() const { return bInvoked; }
        const O3DS::FAudioFrameMeta& GetMeta() const { return Meta; }
        const TArray<uint8>& GetPayload() const { return Payload; }

    private:
        bool bInvoked = false;
        O3DS::FAudioFrameMeta Meta;
        TArray<uint8> Payload;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackAudioRoundTripTest, "Open3DBroadcast.Open3DTransportLoopback.Audio.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DLoopbackAudioRoundTripTest::RunTest(const FString& Parameters)
{
    FO3DTransportConfig Config;
    Config.Transport = TEXT("loopback");
    Config.StreamId = TEXT("audio_roundtrip_test");
    Config.Audio.bEnableAudio = true;
    Config.Audio.SampleRate = 48000;
    Config.Audio.NumChannels = 2;

    FO3DLoopbackSender Sender;
    FO3DLoopbackReceiver Receiver;

    TestTrue(TEXT("Sender initializes"), Sender.Initialize(Config));
    TestTrue(TEXT("Receiver initializes"), Receiver.Initialize(Config));

    TestTrue(TEXT("Sender starts"), Sender.Start());
    TestTrue(TEXT("Receiver starts"), Receiver.Start());

    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> SenderAudioSink = Sender.CreateAudioSink(Config.Audio);
    TestTrue(TEXT("Audio sink created"), SenderAudioSink.IsValid());

    TSharedPtr<FTestReceiverAudioSink, ESPMode::ThreadSafe> ReceiverAudioSink = MakeShared<FTestReceiverAudioSink, ESPMode::ThreadSafe>();
    Receiver.SetAudioSink(ReceiverAudioSink, Config.Audio);

    const int32 NumFrames = 4;
    const int32 NumChannels = Config.Audio.NumChannels;
    TArray<float> Samples;
    Samples.SetNumUninitialized(NumFrames * NumChannels);
    for (int32 Index = 0; Index < NumFrames * NumChannels; ++Index)
    {
        Samples[Index] = (static_cast<float>(Index) / 4.0f) - 0.5f;
    }

    const double TimestampSec = 123.45;
    const bool bSubmitted = SenderAudioSink->SubmitPcm(TEXT("audio_test"), Samples.GetData(), NumFrames, NumChannels, Config.Audio.SampleRate, TimestampSec);
    TestTrue(TEXT("Audio frame submitted"), bSubmitted);

    const int32 Processed = Receiver.Poll();
    TestTrue(TEXT("Receiver processed frames"), Processed > 0);
    TestTrue(TEXT("Receiver sink invoked"), ReceiverAudioSink->WasInvoked());

    const TArray<uint8>& Payload = ReceiverAudioSink->GetPayload();
    TestEqual(TEXT("PCM16 payload size"), Payload.Num(), NumFrames * NumChannels * static_cast<int32>(sizeof(int16)));

    const int16* PcmData = reinterpret_cast<const int16*>(Payload.GetData());
    const int32 ExpectedFirst = FMath::Clamp(FMath::RoundToInt(Samples[0] * 32767.0f), -32768, 32767);
    TestEqual(TEXT("PCM16 conversion first sample"), PcmData[0], static_cast<int16>(ExpectedFirst));
    TestEqual(TEXT("Meta stream label propagated"), ReceiverAudioSink->GetMeta().StreamLabel, TEXT("audio_test"));
    TestEqual(TEXT("Meta channel count propagated"), ReceiverAudioSink->GetMeta().NumChannels, NumChannels);
    TestEqual(TEXT("Meta sample rate propagated"), ReceiverAudioSink->GetMeta().SampleRate, Config.Audio.SampleRate);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackAudioQueueOverflowTest, "Open3DBroadcast.Open3DTransportLoopback.Audio.QueueOverflow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DLoopbackAudioQueueOverflowTest::RunTest(const FString& Parameters)
{
    FO3DTransportConfig Config;
    Config.Transport = TEXT("loopback");
    Config.StreamId = TEXT("audio_overflow_test");
    Config.Audio.bEnableAudio = true;
    Config.Audio.SampleRate = 44100;
    Config.Audio.NumChannels = 1;
    Config.AdvancedParams.Add(TEXT("loopback.maxaudioqueue"), TEXT("1"));

    FO3DLoopbackSender Sender;
    TestTrue(TEXT("Sender initializes"), Sender.Initialize(Config));
    TestTrue(TEXT("Sender starts"), Sender.Start());

    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink = Sender.CreateAudioSink(Config.Audio);
    TestTrue(TEXT("Audio sink created"), AudioSink.IsValid());

    const float SampleValue = 0.25f;
    const bool bFirstAccepted = AudioSink->SubmitPcm(TEXT("audio_overflow"), &SampleValue, 1, 1, Config.Audio.SampleRate, 0.0);
    TestTrue(TEXT("First audio frame accepted"), bFirstAccepted);

    const bool bSecondAccepted = AudioSink->SubmitPcm(TEXT("audio_overflow"), &SampleValue, 1, 1, Config.Audio.SampleRate, 0.1);
    TestFalse(TEXT("Second audio frame dropped when queue full"), bSecondAccepted);

    return true;
}

#endif // WITH_AUTOMATION_TESTS
