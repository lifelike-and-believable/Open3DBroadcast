// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-R1 (mid-project review RR-1): a receiver source can outlive its LiveLink registration, for
// example through the handle Create Open3DStream LiveLink Source returns to Blueprint. LiveLink
// owns the source's settings object and frees it with the source's collection entry, so the
// source must stop ticking once shut down and must not hand out a settings object that is gone.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DReceiverSource.h"
#include "O3DReceiverSourceSettings.h"
#include "Testing/O3DReceiverTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverSourceShutdownTickTest, "Open3DBroadcast.Receiver.Source.StopsTickingAfterShutdown", O3DB_TEST_FLAGS)
bool FO3DReceiverSourceShutdownTickTest::RunTest(const FString& Parameters)
{
	FO3DReceiverSourceConfig Config;
	Config.TransportName = FName(TEXT("loopback"));
	const TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(Config);
	TestTrue(TEXT("A live source ticks"), Source->IsTickable());
	Source->RequestSourceShutdown();
	TestFalse(TEXT("A source LiveLink has shut down does not tick"), Source->IsTickable());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverSourceGarbageSettingsTest, "Open3DBroadcast.Receiver.Source.DoesNotKeepFreedSettings", O3DB_TEST_FLAGS)
bool FO3DReceiverSourceGarbageSettingsTest::RunTest(const FString& Parameters)
{
	FO3DReceiverSourceConfig Config;
	Config.TransportName = FName(TEXT("loopback"));
	const TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(Config);
	UO3DReceiverSourceSettings* Settings = NewObject<UO3DReceiverSourceSettings>();
	Source->InitializeSettings(Settings);
	TestTrue(TEXT("The settings LiveLink gave are used"), FO3DReceiverSourceTestAccessor::GetSettings(*Source) == Settings);

	// What LiveLink's removal leads to: nothing holds the settings any more and they are destroyed.
	Settings->MarkAsGarbage();
	TestNull(TEXT("Settings that are being destroyed are not used"), FO3DReceiverSourceTestAccessor::GetSettings(*Source));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
