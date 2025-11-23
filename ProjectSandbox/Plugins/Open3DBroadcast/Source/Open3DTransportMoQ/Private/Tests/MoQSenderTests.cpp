#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "O3DTransportTypes.h"
#include "Sender/MoQSender.h"

#if O3D_WITH_TRANSPORT_MOQ

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderRequiresUriTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.RequiresUri", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderRequiresUriTest::RunTest(const FString& Parameters)
{
    FO3DMoQSender Sender;
    FO3DTransportConfig Config;
    Config.Transport = TEXT("MoQ");

    TestFalse(TEXT("Initialize should fail when relay URI is missing"), Sender.Initialize(Config));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderInitializeSuccessTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.Initialize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderInitializeSuccessTest::RunTest(const FString& Parameters)
{
    FO3DMoQSender Sender;
    FO3DTransportConfig Config;
    Config.Transport = TEXT("MoQ");
    Config.Uri = TEXT("https://localhost:4443");
    Config.StreamId = TEXT("session/testTrack");

    TestTrue(TEXT("Initialize should succeed with valid relay URI"), Sender.Initialize(Config));

    // Ensure Stop is safe to call immediately after initialization.
    Sender.Stop();
    return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
