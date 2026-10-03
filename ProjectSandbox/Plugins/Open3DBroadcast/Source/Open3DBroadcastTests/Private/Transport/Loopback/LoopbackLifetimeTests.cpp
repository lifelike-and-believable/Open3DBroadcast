// Copyright (c) Open3DStream Contributors
//
// WP-S5 acceptance (TRB-30): start/stop the loopback sender 1,000 times while a fake audio
// thread submits PCM, including cycles where the sender is destroyed while the audio thread
// still holds its sink. Run under ASan (MSVC /fsanitize=address) to catch use-after-free.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Transport/O3DTransportRegistry.h"
#include "Testing/O3DTransportLifetimeTestUtils.h"

#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeLock.h"

#include <atomic>

namespace O3DLoopbackLifetimeTests
{
    /** Calls SendSerialized in a loop on whichever sender is published, like the ADR 0008 pipeline worker. */
    class FLoopbackSendHammer final : public FRunnable
    {
    public:
        FLoopbackSendHammer()
        {
            Thread = FRunnableThread::Create(this, TEXT("O3D_LoopbackSendHammer"));
        }
        virtual ~FLoopbackSendHammer() override { StopAndJoin(); }

        void SetSender(const TSharedPtr<IOpen3DSender>& InSender)
        {
            FScopeLock Lock(&Mutex);
            Sender = InSender;
        }

        void StopAndJoin()
        {
            bStop.store(true);
            if (Thread)
            {
                Thread->WaitForCompletion();
                delete Thread;
                Thread = nullptr;
            }
        }

        virtual uint32 Run() override
        {
            const uint8 Frame[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
            while (!bStop.load())
            {
                TSharedPtr<IOpen3DSender> Local;
                {
                    FScopeLock Lock(&Mutex);
                    Local = Sender;
                }
                if (!Local.IsValid())
                {
                    FPlatformProcess::YieldThread();
                    continue;
                }
                Local->SendSerialized(FO3DSendPayload::MakeCopy(Frame, 16, TEXT("Hammer"), 0.0));
                Calls.fetch_add(1);
            }
            return 0;
        }

        std::atomic<int64> Calls{0};

    private:
        FCriticalSection Mutex;
        TSharedPtr<IOpen3DSender> Sender;
        std::atomic<bool> bStop{false};
        FRunnableThread* Thread = nullptr;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackLifetimeStressTest, "Open3DBroadcast.Transport.Loopback.Lifetime.StartStopWithAudio", O3DB_TEST_FLAGS)
bool FO3DLoopbackLifetimeStressTest::RunTest(const FString& Parameters)
{
    // A receiver keeps the channel alive across cycles, which is the case where the old
    // sink's weak channel pin still succeeds after its sender is gone.
    FO3DTransportConfig ReceiverConfig;
    ReceiverConfig.Transport = TEXT("Loopback");
    ReceiverConfig.StreamId = TEXT("wp_s5_lifetime");
    ReceiverConfig.Audio.bEnableAudio = true;
    const TSharedPtr<IOpen3DReceiver> Receiver = FO3DTransportRegistry::Get().CreateReceiver(TEXT("Loopback"));
    if (!TestTrue(TEXT("Loopback receiver registered"), Receiver.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(ReceiverConfig).IsOk());

    const O3DLifetimeTest::FStressResult Result = O3DLifetimeTest::RunSenderStressWith(
        []() { return FO3DTransportRegistry::Get().CreateSender(TEXT("Loopback")); },
        [](int32)
    {
        FO3DTransportConfig Config;
        Config.Transport = TEXT("Loopback");
        Config.StreamId = TEXT("wp_s5_lifetime");
        Config.Audio.bEnableAudio = true;
        Config.Audio.SampleRate = 48000;
        Config.Audio.NumChannels = 1;
        Config.AdvancedParams.Add(TEXT("loopback.maxaudioqueue"), TEXT("4096"));
        return Config;
    }, /*bStart=*/true);

    Receiver->Poll(); // drain whatever the fake audio thread queued
    Receiver->Stop();

    TestEqual(TEXT("All cycles ran"), Result.CyclesRun, O3DLifetimeTest::StressCycles);
    TestEqual(TEXT("Every cycle started"), Result.StartFailures, 0);
    TestEqual(TEXT("Every cycle produced a sink"), Result.SinksCreated, O3DLifetimeTest::StressCycles);
    TestEqual(TEXT("No sink accepts PCM after its sender stopped"), Result.StaleSinkAccepted, 0);
    AddInfo(FString::Printf(TEXT("Fake audio thread submitted %lld buffers, %lld accepted"), Result.Submitted, Result.Accepted));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackStopUnderLoadTest, "Open3DBroadcast.Transport.Loopback.Lifetime.StopWhileSending", O3DB_TEST_FLAGS)
bool FO3DLoopbackStopUnderLoadTest::RunTest(const FString& Parameters)
{
    // ADR 0007 Verification: Stop() while four threads call SendSerialized and a fake audio thread
    // submits PCM, 1,000 cycles, on the shared send queue (WP-A1 step 4). Run under ASan to catch a
    // use after free; the checks are that every cycle starts and stops and nothing enqueues after Stop.
    using namespace O3DLoopbackLifetimeTests;
    const FString Channel = O3DTests::MakeUniqueName(TEXT("loopback_stop_under_load"));
    TArray<TUniquePtr<FLoopbackSendHammer>> Hammers;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        Hammers.Add(MakeUnique<FLoopbackSendHammer>());
    }
    O3DLifetimeTest::FFakeAudioThread AudioThread;

    int32 StartFailures = 0;
    int32 AcceptedAfterStop = 0;
    for (int32 Cycle = 0; Cycle < O3DLifetimeTest::StressCycles; ++Cycle)
    {
        FO3DTransportConfig Config;
        Config.Transport = TEXT("Loopback");
        Config.StreamId = Channel;
        Config.Audio.bEnableAudio = true;
        Config.AdvancedParams.Add(TEXT("loopback.maxqueue"), TEXT("256"));
        Config.AdvancedParams.Add(TEXT("loopback.maxaudioqueue"), TEXT("256"));

        TSharedPtr<IOpen3DSender> Sender = FO3DTransportRegistry::Get().CreateSender(TEXT("Loopback"));
        if (!Sender.IsValid() || !Sender->Initialize(Config).IsOk() || !Sender->Start().IsOk())
        {
            ++StartFailures;
            continue;
        }
        AudioThread.SetSink(Sender->CreateAudioSink(Config.Audio));
        for (const TUniquePtr<FLoopbackSendHammer>& Hammer : Hammers)
        {
            Hammer->SetSender(Sender);
        }
        FPlatformProcess::YieldThread();

        Sender->Stop();
        const uint8 Probe[4] = { 9, 9, 9, 9 };
        AcceptedAfterStop += Sender->SendSerialized(FO3DSendPayload::MakeCopy(Probe, 4, TEXT("Probe"), 0.0)) == EO3DSendResult::Queued ? 1 : 0;

        // Half the cycles drop the sender while the threads still hold it.
        if ((Cycle & 1) == 0)
        {
            for (const TUniquePtr<FLoopbackSendHammer>& Hammer : Hammers)
            {
                Hammer->SetSender(nullptr);
            }
            AudioThread.SetSink(nullptr);
        }
        Sender.Reset();
    }

    int64 Calls = 0;
    for (TUniquePtr<FLoopbackSendHammer>& Hammer : Hammers)
    {
        Hammer->SetSender(nullptr);
        Hammer->StopAndJoin();
        Calls += Hammer->Calls.load();
    }
    AudioThread.SetSink(nullptr);
    AudioThread.StopAndJoin();

    TestEqual(TEXT("Every cycle started"), StartFailures, 0);
    TestEqual(TEXT("No send is accepted after Stop returned"), AcceptedAfterStop, 0);
    AddInfo(FString::Printf(TEXT("Send threads made %lld calls; the audio thread submitted %lld buffers"), Calls, AudioThread.GetSubmitted()));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
