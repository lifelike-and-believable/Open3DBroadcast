// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U2 (UX-3): Create Open3DStream LiveLink Source adds a receiver source to the LiveLink client
// at runtime, with the settings LiveLink saves into a preset (connection string and factory), and
// returns a handle for LiveLink's own source nodes. A transport without a receiver creates nothing.
// The loopback transport on a unique channel: no network. The source is shut down and removed
// before the test ends.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "LiveLinkSourceSettings.h"
#include "Misc/AutomationTest.h"

#include "O3DReceiverBlueprintLibrary.h"
#include "O3DReceiverSourceFactory.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverBlueprintLibraryCreateTest, "Open3DBroadcast.Receiver.BlueprintLibrary.CreateLiveLinkSource", O3DB_TEST_FLAGS)
bool FO3DReceiverBlueprintLibraryCreateTest::RunTest(const FString& Parameters)
{
	IModularFeatures& ModularFeatures = IModularFeatures::Get();
	if (!TestTrue(TEXT("LiveLink client available"), ModularFeatures.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName)))
	{
		return false;
	}
	ILiveLinkClient& Client = ModularFeatures.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

	// A transport with no receiver: nothing is created.
	{
		FLiveLinkSourceHandle Handle;
		AddExpectedError(TEXT("no receiver is registered"), EAutomationExpectedMessageFlags::Contains, 1);
		const bool bCreated = UO3DReceiverBlueprintLibrary::CreateLiveLinkSource(FName(*O3DTests::MakeUniqueName(TEXT("O3DNoSuchReceiver"))), {}, NAME_None, false, Handle);
		TestFalse(TEXT("Not created"), bCreated);
		TestFalse(TEXT("Empty handle"), Handle.SourcePointer.IsValid());
	}

	const FString Channel = O3DTests::MakeUniqueName(TEXT("o3d-bp-library"));
	FLiveLinkSourceHandle Handle;
	const bool bCreated = UO3DReceiverBlueprintLibrary::CreateLiveLinkSource(TEXT("loopback"), { { TEXT("channel"), Channel } }, NAME_None, false, Handle);
	if (!TestTrue(TEXT("Created"), bCreated) || !TestTrue(TEXT("Handle holds the source"), Handle.SourcePointer.IsValid()))
	{
		return false;
	}

	// Find it in the client by the settings LiveLink keeps for presets.
	FGuid Found;
	for (const FGuid& Guid : Client.GetSources())
	{
		const ULiveLinkSourceSettings* Settings = Client.GetSourceSettings(Guid);
		if (Settings && Settings->ConnectionString.Contains(Channel))
		{
			Found = Guid;
			TestTrue(TEXT("The preset recreates it with the Open3DStream factory"), Settings->Factory == UO3DReceiverSourceFactory::StaticClass());
		}
	}
	TestTrue(TEXT("The source is in the LiveLink client, with its options in the connection string"), Found.IsValid());

	Handle.SourcePointer->RequestSourceShutdown();
	if (Found.IsValid())
	{
		Client.RemoveSource(Found);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
