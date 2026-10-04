// Copyright Lifelike & Believable. All Rights Reserved.

//
// ADR 0012 (SHR-38): FO3DRuntimeContext. Two contexts never see each other's audio, control or
// metrics, and the static accessors reach the default context.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DRuntimeContext.h"

namespace O3DRuntimeContextTests
{
	FO3DControlChange Value(const FString& Source, const FString& Key, double V)
	{
		FO3DControlChange C;
		C.Kind = FO3DControlChange::EKind::ValueChanged;
		C.Name = Key;
		C.Value = FO3DControlValue::MakeFloat(V);
		C.Meta.SourceId = Source;
		C.Meta.Epoch = 1;
		C.Meta.Version = 1;
		return C;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRuntimeContextIsolationTest, "Open3DBroadcast.Shared.RuntimeContext.ContextsAreIsolated", O3DB_TEST_FLAGS)
bool FO3DRuntimeContextIsolationTest::RunTest(const FString& Parameters)
{
	using namespace O3DRuntimeContextTests;
	const FO3DRuntimeContextRef A = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.A"));
	const FO3DRuntimeContextRef B = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.B"));
	TestEqual(TEXT("Name kept"), A->GetName(), FName(TEXT("O3DTest.A")));

	// Audio
	int32 HeardA = 0;
	int32 HeardB = 0;
	A->GetAudioBus().OnPcm16().AddLambda([&HeardA](const O3DS::FAudioFrameMeta&, TConstArrayView<uint8>) { ++HeardA; });
	B->GetAudioBus().OnPcm16().AddLambda([&HeardB](const O3DS::FAudioFrameMeta&, TConstArrayView<uint8>) { ++HeardB; });
	const uint8 Pcm[4] = { 1, 2, 3, 4 };
	A->GetAudioBus().PublishPcm16(O3DS::FAudioFrameMeta(), Pcm, UE_ARRAY_COUNT(Pcm));
	TestEqual(TEXT("Audio reaches its own context"), HeardA, 1);
	TestEqual(TEXT("Audio does not reach another context"), HeardB, 0);

	// Control
	int32 ChangesB = 0;
	B->GetControlBus().OnChange().AddLambda([&ChangesB](const FO3DControlChange&) { ++ChangesB; });
	TestTrue(TEXT("Control published in A"), A->GetControlBus().Publish(Value(TEXT("Src"), TEXT("env.fog"), 0.5)));
	TestNotNull(TEXT("A holds the value"), A->GetControlBus().FindValue(TEXT("Src"), TEXT("env.fog"), FString()));
	TestNull(TEXT("B does not"), B->GetControlBus().FindValue(TEXT("Src"), TEXT("env.fog"), FString()));
	TestEqual(TEXT("B's listeners not called"), ChangesB, 0);
	TestEqual(TEXT("B has no sources"), B->GetControlBus().GetSources().Num(), 0);
	// The same change in B is new there, not a duplicate of A's.
	TestTrue(TEXT("Duplicate checks are per context"), B->GetControlBus().Publish(Value(TEXT("Src"), TEXT("env.fog"), 0.5)));
	TestEqual(TEXT("B's listener called once"), ChangesB, 1);

	// Metrics
	A->GetMetrics().RecordFrameReceived();
	A->GetMetrics().RecordFrameCaptured();
	TestEqual(TEXT("A counts its frame"), A->GetMetrics().GetReceiverMetrics().FramesReceived.load(), (uint64)1);
	TestEqual(TEXT("B counts nothing (receiver)"), B->GetMetrics().GetReceiverMetrics().FramesReceived.load(), (uint64)0);
	TestEqual(TEXT("B counts nothing (sender)"), B->GetMetrics().GetSenderMetrics().FramesCaptured.load(), (uint64)0);
	A->GetMetrics().AcquireTransportMetrics(TEXT("O3DTest.Transport"))->RecordFrameSent(10);
	TestFalse(TEXT("Transport metrics are per context"), B->GetMetrics().FindTransportMetrics(TEXT("O3DTest.Transport")).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRuntimeContextDefaultTest, "Open3DBroadcast.Shared.RuntimeContext.StaticsUseDefault", O3DB_TEST_FLAGS)
bool FO3DRuntimeContextDefaultTest::RunTest(const FString& Parameters)
{
	using namespace O3DRuntimeContextTests;
	FO3DRuntimeContext& Default = *FO3DRuntimeContext::Default();
	TestTrue(TEXT("Default() returns one context"), &*FO3DRuntimeContext::Default() == &Default);
	TestEqual(TEXT("The default context has no name"), Default.GetName(), FName());

	TestTrue(TEXT("FO3DPerformanceMetrics::Get() is the default context's"), &FO3DPerformanceMetrics::Get() == &Default.GetMetrics());
	TestTrue(TEXT("FO3DAudioBus::OnPcm16() is the default context's"), &FO3DAudioBus::OnPcm16() == &Default.GetAudioBus().OnPcm16());
	TestTrue(TEXT("FO3DControlBus::OnChange() is the default context's"), &FO3DControlBus::OnChange() == &Default.GetControlBus().OnChange());

	// Audio published through the static reaches a listener on the default instance.
	int32 Heard = 0;
	const FDelegateHandle AudioHandle = Default.GetAudioBus().OnPcm16().AddLambda([&Heard](const O3DS::FAudioFrameMeta&, TConstArrayView<uint8>) { ++Heard; });
	const uint8 Pcm[2] = { 0, 0 };
	FO3DAudioBus::PublishPcm16(O3DS::FAudioFrameMeta(), Pcm, UE_ARRAY_COUNT(Pcm));
	Default.GetAudioBus().OnPcm16().Remove(AudioHandle);
	TestEqual(TEXT("Static audio publish reaches the default bus"), Heard, 1);

	// Control published through the static is held by the default instance, and forgotten through it.
	const FString Source = TEXT("O3DTest.RuntimeContext.Default");
	TestTrue(TEXT("Static control publish"), FO3DControlBus::Publish(Value(Source, TEXT("k"), 1.0)));
	TestNotNull(TEXT("Default instance holds it"), Default.GetControlBus().FindValue(Source, TEXT("k"), FString()));
	Default.GetControlBus().ForgetSource(Source);
	TestNull(TEXT("Static lookup sees the instance's forget"), FO3DControlBus::FindValue(Source, TEXT("k"), FString()));

	// A fresh context starts empty, whatever the default context holds.
	const FO3DRuntimeContextRef Fresh = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.Fresh"));
	TestEqual(TEXT("Fresh context starts at zero"), Fresh->GetMetrics().GetReceiverMetrics().FramesReceived.load(), (uint64)0);
	TestEqual(TEXT("Fresh context has no transports"), Fresh->GetMetrics().GetTransportMetricsSnapshot().Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
