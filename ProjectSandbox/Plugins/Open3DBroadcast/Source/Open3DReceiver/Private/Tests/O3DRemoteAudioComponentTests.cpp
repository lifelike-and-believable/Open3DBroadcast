// Copyright (c) Open3DStream Contributors

#include "O3DRemoteAudioComponent.h"

#if WITH_AUTOMATION_TESTS

#include "O3DAudioBus.h"

#include "Misc/AutomationTest.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioComponentFilterTest, "Open3DStream.Receiver.RemoteAudioComponent.Filtering", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DRemoteAudioComponentFilterTest::RunTest(const FString& Parameters)
{
    UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>();

    // Default mode is Mix; should accept o3ds:mix streams regardless of subject name.
    TestTrue(TEXT("Mix mode accepts o3ds:mix"), FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, TEXT("Unused"), TEXT("o3ds:mix")));

    // Subject mode with specific name filtering
    Component->ReceiveMode = EO3DRemoteAudioMode::Subject;
    Component->LiveLinkSubjectName = FLiveLinkSubjectName(TEXT("Hero"));

    TestTrue(TEXT("Subject match case-insensitive"), FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, TEXT("hero"), TEXT("streamA")));
    TestFalse(TEXT("Subject mismatch rejected"), FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, TEXT("Villain"), TEXT("streamA")));

    // Stream label filter
    Component->StreamLabelFilter = TEXT("vocals");
    TestTrue(TEXT("Stream label substring accepted"), FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, TEXT("hero"), TEXT("subject:vocals")));
    TestFalse(TEXT("Stream label mismatch rejected"), FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, TEXT("hero"), TEXT("subject:fx")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioComponentAudioQueueTest, "Open3DStream.Receiver.RemoteAudioComponent.AudioQueue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioBusBroadcastTest, "Open3DStream.Shared.AudioBus.BroadcastCopy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DAudioBusBroadcastTest::RunTest(const FString& Parameters)
{
    bool bDelegateInvoked = false;
    O3DS::FAudioFrameMeta ReceivedMeta;
    TArray<uint8> ReceivedData;

    FDelegateHandle Handle = FO3DAudioBus::OnPcm16().AddLambda([
        &bDelegateInvoked,
        &ReceivedMeta,
        &ReceivedData
    ](const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& PCM16Bytes)
    {
        bDelegateInvoked = true;
        ReceivedMeta = Meta;
        ReceivedData = PCM16Bytes;
    });

    O3DS::FAudioFrameMeta Meta;
    Meta.StreamLabel = TEXT("o3ds:mix");
    Meta.SubjectName = TEXT("Checker");
    Meta.NumChannels = 2;
    Meta.SampleRate = 48000;

    TArray<uint8> PCM16;
    PCM16.AddUninitialized(6);
    for (int32 Index = 0; Index < PCM16.Num(); ++Index)
    {
        PCM16[Index] = static_cast<uint8>(Index * 3);
    }

    FO3DAudioBus::PublishPcm16(Meta, PCM16.GetData(), PCM16.Num());

    TestTrue(TEXT("Delegate invoked"), bDelegateInvoked);
    TestEqual(TEXT("Metadata copied"), ReceivedMeta.SubjectName, Meta.SubjectName);
    TestEqual(TEXT("Payload size preserved"), ReceivedData.Num(), PCM16.Num());
    TestTrue(TEXT("Payload content copied"), ReceivedData == PCM16);

    FO3DAudioBus::OnPcm16().Remove(Handle);

    return true;
}

#endif // WITH_AUTOMATION_TESTS
