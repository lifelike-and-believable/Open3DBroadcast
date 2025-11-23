#include "Misc/AutomationTest.h"

#include "HAL/PlatformProcess.h"
#include "Shared/MoQAsyncDispatcher.h"
#include "Shared/MoQSessionWrapper.h"
#include "Shared/MoQTypes.h"

#include <atomic>

#if O3D_WITH_TRANSPORT_MOQ

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSessionInvalidUrlTest, "Open3DBroadcast.Open3DTransportMoQ.Session.InvalidUrl", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSessionInvalidUrlTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<FMoQSessionWrapper> Session = MakeShared<FMoQSessionWrapper>();
    const FMoQResult Result = Session->Initialize(TEXT("   \t"));
    TestEqual(TEXT("Empty URL should be rejected"), LexToString(Result.Code), LexToString(EMoQErrorCode::InvalidArgument));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSessionCreatePublisherWithoutConnectionTest, "Open3DBroadcast.Open3DTransportMoQ.Session.CreatePublisherWithoutConnection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSessionCreatePublisherWithoutConnectionTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<FMoQSessionWrapper> Session = MakeShared<FMoQSessionWrapper>();
    TestTrue(TEXT("Initialize should succeed"), Session->Initialize(TEXT("https://localhost:4443")).IsOk());

    FMoQPublisherConfig Config;
    Config.Namespace = TEXT("mocap/test");
    Config.TrackName = TEXT("character");
    Config.DeliveryMode = MOQ_DELIVERY_STREAM;

    TSharedPtr<FMoQPublisherHandle> Publisher;
    const FMoQResult Result = Session->CreatePublisher(Config, Publisher);
    TestEqual(TEXT("Expected not-connected error"), LexToString(Result.Code), LexToString(EMoQErrorCode::NotConnected));
    TestFalse(TEXT("Publisher should not be created"), Publisher.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSubscriptionRequiresCallbackTest, "Open3DBroadcast.Open3DTransportMoQ.Session.SubscribeRequiresCallback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSubscriptionRequiresCallbackTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<FMoQSessionWrapper> Session = MakeShared<FMoQSessionWrapper>();
    TestTrue(TEXT("Initialize should succeed"), Session->Initialize(TEXT("https://localhost:4443")).IsOk());

    FMoQSubscriptionConfig Config;
    Config.Namespace = TEXT("mocap/test");
    Config.TrackName = TEXT("character");

    TSharedPtr<FMoQSubscriberHandle> Subscriber;
    const FMoQResult Result = Session->Subscribe(Config, Subscriber);
    TestEqual(TEXT("Missing callback should return invalid argument"), LexToString(Result.Code), LexToString(EMoQErrorCode::InvalidArgument));
    TestFalse(TEXT("Subscriber handle should be invalid"), Subscriber.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQDispatcherRunsOnGameThreadTest, "Open3DBroadcast.Open3DTransportMoQ.Dispatcher.GameThread", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQDispatcherRunsOnGameThreadTest::RunTest(const FString& Parameters)
{
    std::atomic<bool> bCompleted{false};
    std::atomic<bool> bOnGameThread{false};

    FMoQAsyncDispatcher::Get().Initialize();
    FMoQAsyncDispatcher::Get().EnqueueGameThreadTask([&bCompleted, &bOnGameThread]()
    {
        bOnGameThread = IsInGameThread();
        bCompleted = true;
    });

    const double StartTime = FPlatformTime::Seconds();
    while (!bCompleted.load() && (FPlatformTime::Seconds() - StartTime) < 2.0)
    {
        FPlatformProcess::Sleep(0.01f);
    }

    TestTrue(TEXT("Dispatcher should complete queued work"), bCompleted.load());
    TestTrue(TEXT("Work should execute on game thread"), bOnGameThread.load());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQResultFromCodeFallbackTest, "Open3DBroadcast.Open3DTransportMoQ.Result.Fallback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQResultFromCodeFallbackTest::RunTest(const FString& Parameters)
{
    const FMoQResult Result = FMoQResult::FromCode(EMoQErrorCode::Timeout, FString());
    TestTrue(TEXT("Fallback message should not be empty"), !Result.Message.IsEmpty());
    TestEqual(TEXT("Timeout should map correctly"), LexToString(Result.Code), LexToString(EMoQErrorCode::Timeout));
    return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ
