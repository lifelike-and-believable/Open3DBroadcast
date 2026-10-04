// Copyright Lifelike & Believable. All Rights Reserved.

// ADR 0012 item 4 (SHR-38): the sender component acquires one sender metrics handle from its
// runtime context (the default until ADR 0012 PR 4), passes it to its transport in
// FO3DTransportConfig::SenderMetrics, and keeps it across transport restarts. A fake transport
// records the config it was given.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DRuntimeContext.h"
#include "O3DRuntimeSubsystem.h"
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

// ADR 0012 item 5: the sender component's ContextName. Its transport gets that context, and a
// handle from it; changing the name between starts gives a new handle in the new context, because
// a transport refuses a handle from another context.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderComponentContextNameTest, "Open3DBroadcast.Sender.Metrics.ContextNameSelectsContext", O3DB_TEST_FLAGS)
bool FO3DSenderComponentContextNameTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("No TargetMesh set"), EAutomationExpectedMessageFlags::Contains, 0); // once per start
	const FName NameA(TEXT("O3DTest.SenderCtxA"));
	const FName NameB(TEXT("O3DTest.SenderCtxB"));
	const FO3DRuntimeContextRef ContextA = UO3DRuntimeSubsystem::Resolve(NameA);
	const FO3DRuntimeContextRef ContextB = UO3DRuntimeSubsystem::Resolve(NameB);

	FO3DFakeTransportScope Scope;
	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->bAutoCreateTransport = true;
	Component->bAllowControlOnly = true;
	Component->SubjectName = TEXT("ContextHero");
	Component->SetTransportName(Scope.GetName());
	Component->ContextName = NameA;

	Component->StartCapture();
	const TSharedPtr<FO3DSenderMetricsHandle, ESPMode::ThreadSafe> HandleA = Component->GetSenderMetricsHandle();
	const TSharedPtr<FO3DFakeSender> First = Scope.GetLastSender();
	if (!TestTrue(TEXT("A handle and a sender"), HandleA.IsValid() && First.IsValid()))
	{
		Component->StopCapture();
		return false;
	}
	TestTrue(TEXT("The transport was given context A"), First->GetLastConfig().Context == ContextA);
	TestTrue(TEXT("The handle is context A's"), &HandleA->GetAggregate() == &ContextA->GetMetrics());
	TestTrue(TEXT("Context A lists it"), ContextA->GetMetrics().GetSenderHandles().Contains(HandleA.ToSharedRef()));
	TestFalse(TEXT("The default context does not"), FO3DRuntimeContext::Default()->GetMetrics().GetSenderHandles().Contains(HandleA.ToSharedRef()));

	// Same name again: same handle.
	Component->StopCapture();
	Component->StartCapture();
	TestTrue(TEXT("Same context, same handle"), Component->GetSenderMetricsHandle() == HandleA);

	// A new name: a new handle in the new context, given to the transport with that context.
	Component->StopCapture();
	Component->ContextName = NameB;
	Component->StartCapture();
	const TSharedPtr<FO3DSenderMetricsHandle, ESPMode::ThreadSafe> HandleB = Component->GetSenderMetricsHandle();
	const TSharedPtr<FO3DFakeSender> Third = Scope.GetLastSender();
	TestTrue(TEXT("A new handle"), HandleB.IsValid() && HandleB != HandleA);
	TestTrue(TEXT("The new handle is context B's"), HandleB.IsValid() && &HandleB->GetAggregate() == &ContextB->GetMetrics());
	TestTrue(TEXT("The transport was given context B and the new handle"), Third.IsValid() && Third->GetLastConfig().Context == ContextB && Third->GetLastConfig().SenderMetrics == HandleB);
	Component->StopCapture();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
