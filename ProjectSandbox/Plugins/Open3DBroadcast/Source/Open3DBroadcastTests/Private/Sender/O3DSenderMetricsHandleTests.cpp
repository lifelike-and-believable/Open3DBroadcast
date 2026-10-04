// Copyright Lifelike & Believable. All Rights Reserved.

// ADR 0012 item 4 (SHR-38): the sender component acquires one sender metrics handle from its
// runtime context (the default until ADR 0012 PR 4), passes it to its transport in
// FO3DTransportConfig::SenderMetrics, and keeps it across transport restarts. A fake transport
// records the config it was given.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DRuntimeContext.h"
#include "O3DSenderComponent.h"
#include "O3DTestFakes.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderComponentMetricsHandleTest, "Open3DBroadcast.Sender.Metrics.ComponentPassesItsHandle", O3DB_TEST_FLAGS)
bool FO3DSenderComponentMetricsHandleTest::RunTest(const FString& Parameters)
{
	// Expected by design: the component has no mesh (control-only).
	AddExpectedError(TEXT("No TargetMesh set"), EAutomationExpectedMessageFlags::Contains, 0); // once per start

	FO3DFakeTransportScope Scope;
	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->bAutoCreateTransport = true;
	Component->bAllowControlOnly = true;
	Component->SubjectName = TEXT("MetricsHero");
	Component->SetTransportName(Scope.GetName());
	TestFalse(TEXT("No handle before the first start"), Component->GetSenderMetricsHandle().IsValid());

	Component->StartCapture();
	const TSharedPtr<FO3DSenderMetricsHandle, ESPMode::ThreadSafe> Handle = Component->GetSenderMetricsHandle();
	const TSharedPtr<FO3DFakeSender> First = Scope.GetLastSender();
	if (!TestTrue(TEXT("The component has a handle"), Handle.IsValid()) || !TestTrue(TEXT("The component started a fake sender"), First.IsValid()))
	{
		Component->StopCapture();
		return false;
	}
	TestTrue(TEXT("The transport was given the component's handle"), First->GetLastConfig().SenderMetrics == Handle);
	TestTrue(TEXT("The handle is the default context's"), &Handle->GetAggregate() == &FO3DRuntimeContext::Default()->GetMetrics());
	TestTrue(TEXT("The handle is named after the subject"), Handle->GetOwnerName().Contains(TEXT("MetricsHero")));
	TestTrue(TEXT("The default context lists it"), FO3DRuntimeContext::Default()->GetMetrics().GetSenderHandles().Contains(Handle.ToSharedRef()));

	// A restart keeps the handle, so its counts span the component's life.
	Component->StopCapture();
	Component->StartCapture();
	const TSharedPtr<FO3DFakeSender> Second = Scope.GetLastSender();
	TestTrue(TEXT("A new sender after the restart"), Second.IsValid() && Second != First);
	TestTrue(TEXT("The component kept its handle"), Component->GetSenderMetricsHandle() == Handle);
	TestTrue(TEXT("The new transport was given the same handle"), Second.IsValid() && Second->GetLastConfig().SenderMetrics == Handle);
	Component->StopCapture();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
