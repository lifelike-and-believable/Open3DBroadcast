// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors
//
// Loopback audio path. The transport is created through the registry by its registered name,
// so these tests need no access to the module's private classes (WP-T2).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportRegistry.h"

#include "Misc/AutomationTest.h"

namespace
{
    const FName LoopbackTransportName(TEXT("Loopback"));

    class FLoopbackTestAudioSink final : public IO3DReceiverAudioSink
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackAudioRoundTripTest, "Open3DBroadcast.Transport.Loopback.Audio.RoundTrip", O3DB_TEST_FLAGS)
bool FO3DLoopbackAudioRoundTripTest::RunTest(const FString& Parameters)
{
    FO3DTransportConfig Config;
    Config.Transport = TEXT("Loopback");
    Config.StreamId = TEXT("audio_roundtrip_test");
    Config.Audio.bEnableAudio = true;
    Config.Audio.SampleRate = 48000;
    Config.Audio.NumChannels = 2;

    const TSharedPtr<IOpen3DSender> SenderPtr = FO3DTransportRegistry::Get().CreateSender(LoopbackTransportName);
    const TSharedPtr<IOpen3DReceiver> ReceiverPtr = FO3DTransportRegistry::Get().CreateReceiver(LoopbackTransportName);
    if (!TestTrue(TEXT("Loopback sender and receiver registered"), SenderPtr.IsValid() && ReceiverPtr.IsValid()))
    {
        return false;
    }
    IOpen3DSender& Sender = *SenderPtr;
    IOpen3DReceiver& Receiver = *ReceiverPtr;

    TestTrue(TEXT("Sender initializes"), Sender.Initialize(Config).IsOk());
    TestTrue(TEXT("Receiver initializes"), Receiver.Initialize(Config).IsOk());

    // A receiver needs a frame consumer to start (ADR 0007 item 3), even for audio only.
    Receiver.SetConsumer(MakeShared<FO3DRecordingFrameConsumer>());
    TestTrue(TEXT("Sender starts"), Sender.Start().IsOk());
    TestTrue(TEXT("Receiver starts"), Receiver.Start().IsOk());

    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> SenderAudioSink = Sender.CreateAudioSink(Config.Audio);
    TestTrue(TEXT("Audio sink created"), SenderAudioSink.IsValid());

    TSharedPtr<FLoopbackTestAudioSink, ESPMode::ThreadSafe> ReceiverAudioSink = MakeShared<FLoopbackTestAudioSink, ESPMode::ThreadSafe>();
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackAudioQueueOverflowTest, "Open3DBroadcast.Transport.Loopback.Audio.QueueOverflow", O3DB_TEST_FLAGS)
bool FO3DLoopbackAudioQueueOverflowTest::RunTest(const FString& Parameters)
{
    FO3DTransportConfig Config;
    Config.Transport = TEXT("Loopback");
    Config.StreamId = TEXT("audio_overflow_test");
    Config.Audio.bEnableAudio = true;
    Config.Audio.SampleRate = 44100;
    Config.Audio.NumChannels = 1;
    Config.AdvancedParams.Add(TEXT("loopback.maxaudioqueue"), TEXT("1"));

    const TSharedPtr<IOpen3DSender> SenderPtr = FO3DTransportRegistry::Get().CreateSender(LoopbackTransportName);
    if (!TestTrue(TEXT("Loopback sender registered"), SenderPtr.IsValid()))
    {
        return false;
    }
    IOpen3DSender& Sender = *SenderPtr;
    TestTrue(TEXT("Sender initializes"), Sender.Initialize(Config).IsOk());
    TestTrue(TEXT("Sender starts"), Sender.Start().IsOk());

    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink = Sender.CreateAudioSink(Config.Audio);
    TestTrue(TEXT("Audio sink created"), AudioSink.IsValid());

    const float SampleValue = 0.25f;
    const bool bFirstAccepted = AudioSink->SubmitPcm(TEXT("audio_overflow"), &SampleValue, 1, 1, Config.Audio.SampleRate, 0.0);
    TestTrue(TEXT("First audio frame accepted"), bFirstAccepted);

    const bool bSecondAccepted = AudioSink->SubmitPcm(TEXT("audio_overflow"), &SampleValue, 1, 1, Config.Audio.SampleRate, 0.1);
    TestFalse(TEXT("Second audio frame dropped when queue full"), bSecondAccepted);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackAudioIndependentTest, "Open3DBroadcast.Transport.Loopback.Audio.IndependentOfFrameQueue", O3DB_TEST_FLAGS)
bool FO3DLoopbackAudioIndependentTest::RunTest(const FString& Parameters)
{
    // ADR 0011 / ADR 0007 item 7: the channel's kinds have their own limits, so a full frame
    // queue never refuses audio, and audio is delivered with the frame in the order it was sent.
    FO3DTransportConfig Config;
    Config.Transport = TEXT("Loopback");
    Config.StreamId = O3DTests::MakeUniqueName(TEXT("audio_independent"));
    Config.Audio.bEnableAudio = true;
    Config.Audio.SampleRate = 48000;
    Config.Audio.NumChannels = 1;
    Config.AdvancedParams.Add(TEXT("loopback.maxqueue"), TEXT("1"));

    const TSharedPtr<IOpen3DSender> Sender = FO3DTransportRegistry::Get().CreateSender(LoopbackTransportName);
    const TSharedPtr<IOpen3DReceiver> Receiver = FO3DTransportRegistry::Get().CreateReceiver(LoopbackTransportName);
    if (!TestTrue(TEXT("Loopback registered"), Sender.IsValid() && Receiver.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("Sender starts"), Sender->Initialize(Config).IsOk() && Sender->Start().IsOk());
    TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(Config).IsOk());
    const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
    const TSharedRef<FLoopbackTestAudioSink, ESPMode::ThreadSafe> ReceiverSink = MakeShared<FLoopbackTestAudioSink, ESPMode::ThreadSafe>();
    Receiver->SetConsumer(Consumer);
    Receiver->SetAudioSink(ReceiverSink, Config.Audio);
    TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk());

    const uint8 Frame[4] = { 1, 2, 3, 4 };
    TestTrue(TEXT("Frame fills the one-frame queue"), Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame, 4, TEXT("Hero"), 0.0)) == EO3DSendResult::Queued);
    TestTrue(TEXT("Next frame refused"), Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame, 4, TEXT("Hero"), 0.0)) == EO3DSendResult::DroppedBackpressure);

    const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink = Sender->CreateAudioSink(Config.Audio);
    const float Samples[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    TestTrue(TEXT("Audio accepted while the frame queue is full"), AudioSink.IsValid() && AudioSink->SubmitPcm(TEXT("voice"), Samples, 4, 1, 48000, 1.0));

    TestEqual(TEXT("Poll delivers the frame and the audio"), Receiver->Poll(), 2);
    TestEqual(TEXT("One frame"), Consumer->Num(), 1);
    TestTrue(TEXT("Audio delivered"), ReceiverSink->WasInvoked());
    TestEqual(TEXT("Audio carries the subject last sent"), ReceiverSink->GetMeta().SubjectName, FString(TEXT("Hero")));
    TestEqual(TEXT("Sender counted the refused frame"), Sender->GetStats().DroppedFrames, static_cast<int64>(1));

    Receiver->Stop();
    Sender->Stop();
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
