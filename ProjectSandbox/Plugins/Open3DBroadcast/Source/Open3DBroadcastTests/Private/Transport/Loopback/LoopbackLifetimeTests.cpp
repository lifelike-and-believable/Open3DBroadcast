// Copyright Lifelike & Believable. All Rights Reserved.

// WP-S5 acceptance (TRB-30): start/stop the loopback sender 1,000 times while a fake audio
// thread submits PCM, including cycles where the sender is destroyed while the audio thread
// still holds its sink. Run under ASan (MSVC /fsanitize=address) to catch use-after-free.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DReceiverRegistry.h"
#include "O3DSenderRegistry.h"
#include "Testing/O3DLifetimeTestUtils.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackLifetimeStressTest, "Open3DBroadcast.Transport.Loopback.Lifetime.StartStopWithAudio", O3DB_TEST_FLAGS)
bool FO3DLoopbackLifetimeStressTest::RunTest(const FString& Parameters)
{
    // A receiver keeps the channel alive across cycles, which is the case where the old
    // sink's weak channel pin still succeeds after its sender is gone.
    FO3DTransportConfig ReceiverConfig;
    ReceiverConfig.Transport = TEXT("loopback");
    ReceiverConfig.StreamId = TEXT("wp_s5_lifetime");
    ReceiverConfig.Audio.bEnableAudio = true;
    const TSharedPtr<IOpen3DReceiver> Receiver = O3DTransport::CreateReceiver(TEXT("Loopback"));
    if (!TestTrue(TEXT("Loopback receiver registered"), Receiver.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(ReceiverConfig));

    const O3DLifetimeTest::FStressResult Result = O3DLifetimeTest::RunSenderStressWith(
        []() { return O3DTransport::CreateSender(TEXT("Loopback")); },
        [](int32)
    {
        FO3DTransportConfig Config;
        Config.Transport = TEXT("loopback");
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

#endif // WITH_DEV_AUTOMATION_TESTS
