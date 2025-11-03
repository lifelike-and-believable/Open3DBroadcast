// Copyright (c) Open3DStream Contributors

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "O3DSAudioBus.h"
#include "O3DSUnifiedMessage.h"
#include "Async/Async.h"

// Console: o3ds.AudioBus.TestTone [Seconds] [FrequencyHz] [Gain]
// Publishes a simple sine wave via FO3DSAudioBus with a Mix-mode stream label so
// UO3DSRemoteAudioComponent (Mix) will accept it. Useful to isolate AudioBus->Playback.
static void O3DSTestTone_Exec(const TArray<FString>& Args)
{
    const int32 SampleRate = 48000;
    int32 Seconds = 2;
    float Freq = 440.0f;
    float Gain = 0.5f; // 50% to avoid clipping

    if (Args.Num() > 0) { Seconds = FMath::Clamp(FCString::Atoi(*Args[0]), 1, 10); }
    if (Args.Num() > 1) { Freq = FMath::Clamp(FCString::Atof(*Args[1]), 50.0f, 4000.0f); }
    if (Args.Num() > 2) { Gain = FMath::Clamp(FCString::Atof(*Args[2]), 0.0f, 2.0f); }

    const int32 TotalFrames = Seconds * SampleRate;
    const int32 Channels = 1;
    TArray<int16> PCM16;
    PCM16.AddUninitialized(TotalFrames * Channels);

    const double TwoPi = 2.0 * PI;
    for (int32 n = 0; n < TotalFrames; ++n)
    {
        const double t = static_cast<double>(n) / static_cast<double>(SampleRate);
        const double s = FMath::Sin(TwoPi * Freq * t) * Gain;
        const int16 v = static_cast<int16>(FMath::Clamp(s, -1.0, 1.0) * 32767.0);
        PCM16[n] = v;
    }

    AsyncTask(ENamedThreads::GameThread, [PCM16 = MoveTemp(PCM16), Channels, SampleRate]()
    {
        O3DS::FAudioFrameMeta Meta;
        Meta.StreamLabel = TEXT("o3ds:mix:test");
        Meta.SubjectName = TEXT("TestTone");
        Meta.NumChannels = Channels;
        Meta.SampleRate = SampleRate;
        Meta.TimestampSec = 0.0;

        const uint8* Bytes = reinterpret_cast<const uint8*>(PCM16.GetData());
        const int32 NumBytes = PCM16.Num() * sizeof(int16);
        FO3DSAudioBus::PublishPcm16(Meta, Bytes, NumBytes);

        UE_LOG(LogTemp, Log, TEXT("O3DS AudioBus: Published TestTone %d frames @ %d Hz"), PCM16.Num() / Channels, SampleRate);
    });
}

static FAutoConsoleCommand O3DSCmd_TestTone(
    TEXT("o3ds.AudioBus.TestTone"),
    TEXT("Publish a sine test tone onto O3DS AudioBus. Usage: o3ds.AudioBus.TestTone [Seconds=2] [FreqHz=440] [Gain=0.5]"),
    FConsoleCommandWithArgsDelegate::CreateStatic(&O3DSTestTone_Exec));
