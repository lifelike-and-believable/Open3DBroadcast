// Copyright (c) Open3DStream Contributors

#include "O3DRemoteAudioComponent.h"
#include "O3DReceiverSource.h"

#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS

#include "O3DAudioBus.h"

#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "Sound/SoundWaveProcedural.h"

struct FO3DRemoteAudioComponentTestAccessor
{
    static void CallEnsureSoundWave(UO3DRemoteAudioComponent* Component, int32 NumChannels, int32 SampleRate)
    {
        Component->EnsureSoundWave(NumChannels, SampleRate);
    }

    static void CallOnAudioPcm16(UO3DRemoteAudioComponent* Component, const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& PCM16Bytes)
    {
        Component->OnAudioPcm16(Meta, PCM16Bytes);
    }

    static bool CallMatchesFilter(const UO3DRemoteAudioComponent* Component, const FString& Subject, const FString& Stream)
    {
        return Component->MatchesFilter(Subject, Stream);
    }

    static USoundWaveProcedural* GetSoundWave(const UO3DRemoteAudioComponent* Component)
    {
        return Component->SoundWave;
    }

    static int32 GetCurrentChannels(const UO3DRemoteAudioComponent* Component)
    {
        return Component->CurrentChannels;
    }

    static int32 GetCurrentSampleRate(const UO3DRemoteAudioComponent* Component)
    {
        return Component->CurrentSampleRate;
    }
};

struct FO3DReceiverSourceTestAccessor
{
    static void SetActiveConfig(FO3DReceiverSource& Source, const FO3DTransportConfig& Config)
    {
        Source.ActiveConfig = Config;
    }

    static void SetLastObservedSubjectName(FO3DReceiverSource& Source, const FName& SubjectName)
    {
        Source.LastObservedSubjectName = SubjectName;
    }

    static void CallFinalizeAudioMeta(const FO3DReceiverSource& Source, O3DS::FAudioFrameMeta& Meta)
    {
        Source.FinalizeAudioMeta(Meta);
    }

    static void SetSourceGuid(FO3DReceiverSource& Source, const FGuid& Guid)
    {
        Source.SourceGuid = Guid;
    }

    static TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> MakeAudioSink(const FO3DReceiverSource& Source)
    {
        return Source.MakeAudioSink();
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioComponentFilterTest, "Open3DBroadcast.Open3DReceiver.RemoteAudioComponent.Filtering", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioComponentAudioQueueTest, "Open3DBroadcast.O3DReceiver.RemoteAudioComponent.AudioQueue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
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



IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverSourceFinalizeAudioMetaTest, "Open3DBroadcast.Open3DReceiver.Source.FinalizeAudioMeta", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DReceiverSourceFinalizeAudioMetaTest::RunTest(const FString& Parameters)
{
    FO3DTransportConfig Config;
    Config.StreamId = TEXT("testanimchannel");
    Config.Audio.bEnableAudio = true;
    Config.Audio.SampleRate = 44100;
    Config.Audio.NumChannels = 2;

    FO3DReceiverSource Source;
    FO3DReceiverSourceTestAccessor::SetActiveConfig(Source, Config);
    FO3DReceiverSourceTestAccessor::SetLastObservedSubjectName(Source, FName(TEXT("Quinn")));

    O3DS::FAudioFrameMeta Meta;
    Meta.SampleRate = 0;
    Meta.NumChannels = 0;
    FO3DReceiverSourceTestAccessor::CallFinalizeAudioMeta(Source, Meta);

    TestEqual(TEXT("Observed subject applied"), Meta.SubjectName, FString(TEXT("Quinn")));
    TestEqual(TEXT("Sample rate propagated"), Meta.SampleRate, Config.Audio.SampleRate);
    TestEqual(TEXT("Channel count propagated"), Meta.NumChannels, Config.Audio.NumChannels);

    O3DS::FAudioFrameMeta ChannelSubjectMeta;
    ChannelSubjectMeta.SubjectName = Config.StreamId;
    FO3DReceiverSourceTestAccessor::CallFinalizeAudioMeta(Source, ChannelSubjectMeta);
    TestEqual(TEXT("Channel fallback replaced by subject"), ChannelSubjectMeta.SubjectName, FString(TEXT("Quinn")));

    O3DS::FAudioFrameMeta ExplicitSubjectMeta;
    ExplicitSubjectMeta.SubjectName = TEXT("AlreadySet");
    FO3DReceiverSourceTestAccessor::CallFinalizeAudioMeta(Source, ExplicitSubjectMeta);
    TestEqual(TEXT("Explicit subject preserved"), ExplicitSubjectMeta.SubjectName, FString(TEXT("AlreadySet")));

    FO3DReceiverSource SourceWithoutSubject;
    FO3DReceiverSourceTestAccessor::SetActiveConfig(SourceWithoutSubject, Config);

    O3DS::FAudioFrameMeta FallbackMeta;
    FO3DReceiverSourceTestAccessor::CallFinalizeAudioMeta(SourceWithoutSubject, FallbackMeta);
    TestEqual(TEXT("Stream id used when no subject observed"), FallbackMeta.SubjectName, Config.StreamId);

    return true;
}

// WP-S5 (RCV-1): the receiver-side audio sink holds an immutable metadata snapshot, not the
// source. It must keep working (and must not touch the source) after the source is destroyed,
// when called from a non-game thread, and it must publish on the game thread only (SHR-10).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverAudioSinkLifetimeTest, "Open3DBroadcast.Receiver.Lifetime.AudioSinkOutlivesSource", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
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
    FDelegateHandle Handle = FO3DAudioBus::OnPcm16().AddLambda([&Received, &LastMeta](const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>&)
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
        O3DS::FAudioFrameMeta Meta; // empty: every field comes from the snapshot
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

#endif // WITH_AUTOMATION_TESTS
