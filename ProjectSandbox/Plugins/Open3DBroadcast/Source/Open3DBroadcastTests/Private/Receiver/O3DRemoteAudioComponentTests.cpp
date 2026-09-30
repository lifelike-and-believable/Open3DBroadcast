// Copyright Lifelike & Believable. All Rights Reserved.

// Remote audio component and receiver-source audio glue. White-box access goes through
// Open3DReceiver/Public/Testing/O3DReceiverTesting.h (WP-T2).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DAudioBus.h"
#include "O3DReceiverSource.h"
#include "O3DRemoteAudioComponent.h"
#include "Testing/O3DReceiverTesting.h"

#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "Sound/SoundWaveProcedural.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioComponentFilterTest, "Open3DBroadcast.Receiver.RemoteAudioComponent.Filtering", O3DB_TEST_FLAGS)
bool FO3DRemoteAudioComponentFilterTest::RunTest(const FString& Parameters)
{
    UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>();

    // Default mode is Mix; should accept o3ds:mix streams regardless of subject name.
    TestTrue(TEXT("Mix mode accepts o3ds:mix"), FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, TEXT("Unused"), TEXT("o3ds:mix")));

    // Subject mode with specific name filtering
    Component->ReceiveMode = EO3DRemoteAudioMode::Subject;
    Component->LiveLinkSubjectName = FLiveLinkSubjectName();
    TestFalse(TEXT("Subject mode rejects when no subject selected"), FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, TEXT("hero"), TEXT("streamA")));

    Component->LiveLinkSubjectName = FLiveLinkSubjectName(TEXT("Hero"));

    TestTrue(TEXT("Subject match case-insensitive"), FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, TEXT("hero"), TEXT("streamA")));
    TestFalse(TEXT("Subject mismatch rejected"), FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, TEXT("Villain"), TEXT("streamA")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioComponentAudioQueueTest, "Open3DBroadcast.Receiver.RemoteAudioComponent.AudioQueue", O3DB_TEST_FLAGS)
bool FO3DRemoteAudioComponentAudioQueueTest::RunTest(const FString& Parameters)
{
    UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>();

    // EnsureSoundWave should allocate a procedural wave with the requested properties.
    FO3DRemoteAudioComponentTestAccessor::CallEnsureSoundWave(Component, 2, 48000);
    USoundWaveProcedural* Wave = FO3DRemoteAudioComponentTestAccessor::GetSoundWave(Component);
    TestNotNull(TEXT("SoundWave allocated"), Wave);

    if (Wave)
    {
        TestEqual(TEXT("NumChannels propagated"), Wave->NumChannels, 2);
    }
    TestEqual(TEXT("CurrentChannels updated"), FO3DRemoteAudioComponentTestAccessor::GetCurrentChannels(Component), 2);
    TestEqual(TEXT("CurrentSampleRate updated"), FO3DRemoteAudioComponentTestAccessor::GetCurrentSampleRate(Component), 48000);

    // Publish PCM16 samples through the component handler.
    O3DS::FAudioFrameMeta Meta;
    Meta.StreamLabel = TEXT("o3ds:mix/test");
    Meta.SubjectName = TEXT("Hero");
    Meta.NumChannels = 1;
    Meta.SampleRate = 44100;

    TArray<uint8> PCM16;
    PCM16.AddUninitialized(4);
    PCM16[0] = 0;
    PCM16[1] = 8;
    PCM16[2] = 16;
    PCM16[3] = 24;

    FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16(Component, Meta, PCM16);

    Wave = FO3DRemoteAudioComponentTestAccessor::GetSoundWave(Component);
    TestNotNull(TEXT("SoundWave still valid after PCM16"), Wave);
    if (Wave)
    {
        TestTrue(TEXT("Queued audio bytes available"), Wave->GetAvailableAudioByteCount() >= PCM16.Num());
        TestEqual(TEXT("Wave channel count reflects metadata"), Wave->NumChannels, 1);
    }
    TestEqual(TEXT("Runtime channels follow metadata"), FO3DRemoteAudioComponentTestAccessor::GetCurrentChannels(Component), 1);
    TestEqual(TEXT("Runtime sample rate follows metadata"), FO3DRemoteAudioComponentTestAccessor::GetCurrentSampleRate(Component), 44100);

    return true;
}



// RCV-2 (decided in ADR 0006 Q6): FinalizeAudioMeta falls back to the stream label, never to the
// last subject seen on the mocap stream. An empty label becomes the channel's StreamId (or
// "o3ds:mix" without one) and an empty subject becomes that label. The unused
// LastObservedSubjectName field is gone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverSourceFinalizeAudioMetaTest, "Open3DBroadcast.Receiver.Source.FinalizeAudioMetaStreamLabelFallback", O3DB_TEST_FLAGS)
bool FO3DReceiverSourceFinalizeAudioMetaTest::RunTest(const FString& Parameters)
{
    FO3DTransportConfig Config;
    Config.StreamId = TEXT("testanimchannel");
    Config.Audio.bEnableAudio = true;
    Config.Audio.SampleRate = 44100;
    Config.Audio.NumChannels = 2;

    FO3DReceiverSource Source;
    FO3DReceiverSourceTestAccessor::SetActiveConfig(Source, Config);

    // Nothing set: label and subject both fall back to the StreamId; format from the config.
    O3DS::FAudioFrameMeta Meta;
    Meta.SampleRate = 0;
    Meta.NumChannels = 0;
    FO3DReceiverSourceTestAccessor::CallFinalizeAudioMeta(Source, Meta);
    TestEqual(TEXT("Empty label falls back to the StreamId"), Meta.StreamLabel, Config.StreamId);
    TestEqual(TEXT("Empty subject falls back to the stream label"), Meta.SubjectName, Config.StreamId);
    TestEqual(TEXT("Sample rate from the config"), Meta.SampleRate, Config.Audio.SampleRate);
    TestEqual(TEXT("Channel count from the config"), Meta.NumChannels, Config.Audio.NumChannels);

    // A label from the transport (for example a per-subject LiveKit track) names the subject.
    O3DS::FAudioFrameMeta LabelledMeta;
    LabelledMeta.StreamLabel = TEXT("Quinn");
    FO3DReceiverSourceTestAccessor::CallFinalizeAudioMeta(Source, LabelledMeta);
    TestEqual(TEXT("Explicit label kept"), LabelledMeta.StreamLabel, FString(TEXT("Quinn")));
    TestEqual(TEXT("Subject follows the explicit label"), LabelledMeta.SubjectName, FString(TEXT("Quinn")));

    // An explicit subject is never overwritten.
    O3DS::FAudioFrameMeta ExplicitSubjectMeta;
    ExplicitSubjectMeta.SubjectName = TEXT("AlreadySet");
    FO3DReceiverSourceTestAccessor::CallFinalizeAudioMeta(Source, ExplicitSubjectMeta);
    TestEqual(TEXT("Explicit subject preserved"), ExplicitSubjectMeta.SubjectName, FString(TEXT("AlreadySet")));
    TestEqual(TEXT("Label still falls back to the StreamId"), ExplicitSubjectMeta.StreamLabel, Config.StreamId);

    // Without a StreamId the label is the mix label. (Before ADR 0006 Q6 this test expected the
    // last mocap subject, "Quinn", as the subject, which the code no longer did.)
    FO3DTransportConfig NoStream = Config;
    NoStream.StreamId.Reset();
    FO3DReceiverSource SourceWithoutStream;
    FO3DReceiverSourceTestAccessor::SetActiveConfig(SourceWithoutStream, NoStream);
    O3DS::FAudioFrameMeta MixMeta;
    FO3DReceiverSourceTestAccessor::CallFinalizeAudioMeta(SourceWithoutStream, MixMeta);
    TestEqual(TEXT("Without a StreamId the label is o3ds:mix"), MixMeta.StreamLabel, FString(TEXT("o3ds:mix")));
    TestEqual(TEXT("...and the subject follows it"), MixMeta.SubjectName, FString(TEXT("o3ds:mix")));

    // With audio disabled no label is invented; the subject still falls back to the StreamId.
    FO3DTransportConfig AudioOff = Config;
    AudioOff.Audio.bEnableAudio = false;
    FO3DReceiverSource SourceAudioOff;
    FO3DReceiverSourceTestAccessor::SetActiveConfig(SourceAudioOff, AudioOff);
    O3DS::FAudioFrameMeta OffMeta;
    FO3DReceiverSourceTestAccessor::CallFinalizeAudioMeta(SourceAudioOff, OffMeta);
    TestTrue(TEXT("No label invented with audio disabled"), OffMeta.StreamLabel.IsEmpty());
    TestEqual(TEXT("Subject falls back to the StreamId"), OffMeta.SubjectName, Config.StreamId);

    return true;
}

// WP-S5 (RCV-1): the receiver-side audio sink holds an immutable metadata snapshot, not the
// source. It must keep working (and must not touch the source) after the source is destroyed,
// when called from a non-game thread, and it must publish on the game thread only (SHR-10).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverAudioSinkLifetimeTest, "Open3DBroadcast.Receiver.Lifetime.AudioSinkOutlivesSource", O3DB_TEST_FLAGS)
bool FO3DReceiverAudioSinkLifetimeTest::RunTest(const FString& Parameters)
{
    FO3DTransportConfig Config;
    Config.StreamId = TEXT("wp_s5_stream");
    Config.Audio.bEnableAudio = true;
    Config.Audio.SampleRate = 44100;
    Config.Audio.NumChannels = 2;
    const FGuid ExpectedGuid = FGuid::NewGuid();

    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> Sink;
    {
        TSharedPtr<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>();
        FO3DReceiverSourceTestAccessor::SetActiveConfig(*Source, Config);
        FO3DReceiverSourceTestAccessor::SetSourceGuid(*Source, ExpectedGuid);
        Sink = FO3DReceiverSourceTestAccessor::MakeAudioSink(*Source);
        // Changing the source afterwards must not affect the snapshot the sink holds.
        FO3DTransportConfig Other;
        Other.StreamId = TEXT("changed_after_start");
        FO3DReceiverSourceTestAccessor::SetActiveConfig(*Source, Other);
    } // source destroyed here, on the game thread
    TestTrue(TEXT("Sink created"), Sink.IsValid());
    if (!Sink.IsValid())
    {
        return false;
    }

    int32 Received = 0;
    O3DS::FAudioFrameMeta LastMeta;
    FDelegateHandle Handle = FO3DAudioBus::OnPcm16().AddLambda([&Received, &LastMeta](const O3DS::FAudioFrameMeta& Meta, TConstArrayView<uint8>)
    {
        check(IsInGameThread());
        ++Received;
        LastMeta = Meta;
    });

    // Submit from a background task, as a LiveKit or socket thread would, and let that task
    // hold the last reference to the sink.
    const int16 Pcm[4] = {1, 2, 3, 4};
    FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady([SinkCopy = Sink, &Pcm]() mutable
    {
        // Unknown format: FAudioFrameMeta defaults to 1 channel at 48 kHz, and the snapshot
        // fills only fields that are unset (<= 0), because a frame's own format describes the
        // PCM it carries. Zero them so rate and channels come from the snapshot too.
        O3DS::FAudioFrameMeta Meta;
        Meta.SampleRate = 0;
        Meta.NumChannels = 0;
        SinkCopy->SubmitPcm16(Meta, reinterpret_cast<const uint8*>(Pcm), sizeof(Pcm));
        SinkCopy.Reset();
    }, TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask);
    Sink.Reset();
    FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task, ENamedThreads::GameThread);
    FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);

    FO3DAudioBus::OnPcm16().Remove(Handle);

    TestEqual(TEXT("Frame published once on the game thread"), Received, 1);
    TestTrue(TEXT("SourceGuid from snapshot"), LastMeta.SourceGuid == ExpectedGuid);
    TestEqual(TEXT("Stream label from snapshot"), LastMeta.StreamLabel, FString(TEXT("wp_s5_stream")));
    TestEqual(TEXT("Sample rate from snapshot"), LastMeta.SampleRate, 44100);
    TestEqual(TEXT("Channels from snapshot"), LastMeta.NumChannels, 2);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
