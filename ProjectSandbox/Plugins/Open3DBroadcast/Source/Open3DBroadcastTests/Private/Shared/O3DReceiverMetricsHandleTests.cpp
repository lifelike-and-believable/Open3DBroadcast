// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// ADR 0012 item 4 (SHR-38): receiver metrics handles. On a fresh runtime context, so no deltas:
// each handle counts its own records; the aggregate equals the sum of every handle, released ones
// included; gauges and averages reach the aggregate; Reset clears handles and aggregate alike.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DRuntimeContext.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverMetricsHandleSumTest, "Open3DBroadcast.Shared.Metrics.ReceiverHandles.AggregateIsTheSum", O3DB_TEST_FLAGS)
bool FO3DReceiverMetricsHandleSumTest::RunTest(const FString& Parameters)
{
	const FO3DRuntimeContextRef Context = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.Handles"));
	FO3DPerformanceMetrics& Metrics = Context->GetMetrics();
	const FO3DPerformanceMetrics::FReceiverMetrics& Aggregate = Metrics.GetReceiverMetrics();

	FO3DReceiverMetricsHandleRef A = Metrics.AcquireReceiverMetrics(TEXT("Receiver A"));
	TSharedPtr<FO3DReceiverMetricsHandle, ESPMode::ThreadSafe> B = Metrics.AcquireReceiverMetrics(TEXT("Receiver B"));
	TestEqual(TEXT("Two live handles"), Metrics.GetReceiverHandles().Num(), 2);
	TestEqual(TEXT("Owner name kept"), A->GetOwnerName(), FString(TEXT("Receiver A")));
	A->SetOwnerName(TEXT("Receiver A (NNG tcp://127.0.0.1:1)"));
	TestEqual(TEXT("Owner name changed"), Metrics.GetReceiverHandles()[0]->GetOwnerName(), FString(TEXT("Receiver A (NNG tcp://127.0.0.1:1)")));

	for (int32 Index = 0; Index < 3; ++Index)
	{
		A->RecordFrameReceived();
		A->RecordFrameApplied();
		A->RecordBytesDeserialized(100);
	}
	A->RecordGateLost(2);
	A->RecordConcealedFrames(4);
	B->RecordFrameReceived();
	B->RecordReceiverFrameDropped(5);
	B->RecordUpdatesAwaitingFullSync(7);
	B->RecordInvalidPoseDropped();
	B->RecordDeserializationError();

	TestEqual(TEXT("A: frames received"), A->GetCounters().FramesReceived.load(), (uint64)3);
	TestEqual(TEXT("A: bytes"), A->GetCounters().BytesDeserialized.load(), (uint64)300);
	TestEqual(TEXT("B: frames received"), B->GetCounters().FramesReceived.load(), (uint64)1);
	TestEqual(TEXT("B: dropped"), B->GetCounters().FramesDropped.load(), (uint64)5);
	TestEqual(TEXT("A did not count B's drops"), A->GetCounters().FramesDropped.load(), (uint64)0);

	TestEqual(TEXT("Aggregate frames received is the sum"), Aggregate.FramesReceived.load(), (uint64)4);
	TestEqual(TEXT("Aggregate frames applied"), Aggregate.FramesApplied.load(), (uint64)3);
	TestEqual(TEXT("Aggregate bytes"), Aggregate.BytesDeserialized.load(), (uint64)300);
	TestEqual(TEXT("Aggregate dropped"), Aggregate.FramesDropped.load(), (uint64)5);
	TestEqual(TEXT("Aggregate awaiting full sync"), Aggregate.UpdatesAwaitingFullSync.load(), (uint64)7);
	TestEqual(TEXT("Aggregate invalid poses"), Aggregate.InvalidPosesDropped.load(), (uint64)1);
	TestEqual(TEXT("Aggregate errors"), Aggregate.DeserializationErrors.load(), (uint64)1);
	TestEqual(TEXT("Aggregate gate lost"), Aggregate.GateLost.load(), (uint64)2);
	TestEqual(TEXT("Aggregate concealed"), Aggregate.ConcealedFrames.load(), (uint64)4);

	// Gauges go to the handle and the aggregate; averages to the aggregate only.
	A->SetReceiverActiveSubjectCount(2);
	B->SetReceiverActiveSubjectCount(5);
	TestEqual(TEXT("A's own subject count"), A->GetCounters().ActiveSubjectCount.load(), 2);
	TestEqual(TEXT("Aggregate subject count is the last written"), Aggregate.ActiveSubjectCount.load(), 5);
	A->RecordFrameLatency(10.0);
	TestTrue(TEXT("Latency reaches the aggregate"), Aggregate.MaxLatencyMs.load() == 10.0);

	// A released handle's counts stay in the aggregate, and it is no longer listed.
	B.Reset();
	TestEqual(TEXT("One live handle"), Metrics.GetReceiverHandles().Num(), 1);
	TestEqual(TEXT("Released counts kept"), Aggregate.FramesReceived.load(), (uint64)4);
	A->RecordFrameReceived();
	TestEqual(TEXT("Aggregate keeps counting"), Aggregate.FramesReceived.load(), (uint64)5);

	// Reset clears the handles too, so the aggregate is again their sum.
	Metrics.Reset();
	TestEqual(TEXT("Aggregate reset"), Aggregate.FramesReceived.load(), (uint64)0);
	TestEqual(TEXT("Handle reset"), A->GetCounters().FramesReceived.load(), (uint64)0);
	TestEqual(TEXT("Handle gauge reset"), A->GetCounters().ActiveSubjectCount.load(), 0);
	A->RecordFrameReceived();
	TestEqual(TEXT("Aggregate equals the handle after a reset"), Aggregate.FramesReceived.load(), A->GetCounters().FramesReceived.load());

	// Another context's handles are not listed here.
	const FO3DRuntimeContextRef Other = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.Other"));
	const FO3DReceiverMetricsHandleRef C = Other->GetMetrics().AcquireReceiverMetrics(TEXT("Receiver C"));
	C->RecordFrameReceived();
	TestEqual(TEXT("Still one handle here"), Metrics.GetReceiverHandles().Num(), 1);
	TestEqual(TEXT("C counts into its own context"), Other->GetMetrics().GetReceiverMetrics().FramesReceived.load(), (uint64)1);
	TestEqual(TEXT("Not into this one"), Aggregate.FramesReceived.load(), (uint64)1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderMetricsHandleSumTest, "Open3DBroadcast.Shared.Metrics.SenderHandles.AggregateIsTheSum", O3DB_TEST_FLAGS)
bool FO3DSenderMetricsHandleSumTest::RunTest(const FString& Parameters)
{
	const FO3DRuntimeContextRef Context = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.SenderHandles"));
	FO3DPerformanceMetrics& Metrics = Context->GetMetrics();
	const FO3DPerformanceMetrics::FSenderMetrics& Aggregate = Metrics.GetSenderMetrics();

	const FO3DSenderMetricsHandleRef A = Metrics.AcquireSenderMetrics(TEXT("Sender A"));
	TSharedPtr<FO3DSenderMetricsHandle, ESPMode::ThreadSafe> B = Metrics.AcquireSenderMetrics(TEXT("Sender B"));
	TestEqual(TEXT("Two live sender handles"), Metrics.GetSenderHandles().Num(), 2);
	TestEqual(TEXT("No receiver handles"), Metrics.GetReceiverHandles().Num(), 0);

	A->RecordFrameCaptured();
	A->RecordFrameCaptured();
	A->RecordBytesSerialized(40);
	A->RecordBytesSent(40);
	B->RecordFrameCaptured();
	B->RecordFrameDropped();
	B->RecordTransportFrameDropped();
	TestEqual(TEXT("A: captured"), A->GetCounters().FramesCaptured.load(), (uint64)2);
	TestEqual(TEXT("B: captured"), B->GetCounters().FramesCaptured.load(), (uint64)1);
	TestEqual(TEXT("A: no drops"), A->GetCounters().FramesDropped.load(), (uint64)0);
	TestEqual(TEXT("Aggregate captured is the sum"), Aggregate.FramesCaptured.load(), (uint64)3);
	TestEqual(TEXT("Aggregate serialized"), Aggregate.BytesSerialized.load(), (uint64)40);
	TestEqual(TEXT("Aggregate sent"), Aggregate.BytesSent.load(), (uint64)40);
	TestEqual(TEXT("Aggregate dropped"), Aggregate.FramesDropped.load(), (uint64)1);
	TestEqual(TEXT("Aggregate transport dropped"), Aggregate.TransportFramesDropped.load(), (uint64)1);

	B.Reset();
	TestEqual(TEXT("One live sender handle"), Metrics.GetSenderHandles().Num(), 1);
	TestEqual(TEXT("Released counts kept"), Aggregate.FramesCaptured.load(), (uint64)3);

	Metrics.Reset();
	TestEqual(TEXT("Aggregate reset"), Aggregate.FramesCaptured.load(), (uint64)0);
	TestEqual(TEXT("Handle reset"), A->GetCounters().FramesCaptured.load(), (uint64)0);

	// A transport given no handle gets its own; given one from another context, it is refused.
	FO3DSenderMetricsHandleRef Resolved = A;
	TestTrue(TEXT("Own handle resolves"), Context->ResolveSenderMetrics(A, TEXT("unused"), Resolved));
	TestTrue(TEXT("The provided handle is used"), Resolved == A);
	TestTrue(TEXT("No handle: a new one"), Context->ResolveSenderMetrics(nullptr, TEXT("NNG sender"), Resolved));
	TestTrue(TEXT("New handle is listed"), Resolved != A && Resolved->GetOwnerName() == TEXT("NNG sender") && Metrics.GetSenderHandles().Num() == 2);
	const FO3DRuntimeContextRef Other = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.OtherSender"));
	FO3DSenderMetricsHandleRef Untouched = A;
	TestFalse(TEXT("A handle from another context is refused"), Other->ResolveSenderMetrics(A, TEXT("unused"), Untouched));
	TestTrue(TEXT("The out handle is untouched"), Untouched == A);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
