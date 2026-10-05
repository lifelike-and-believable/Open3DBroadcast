// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// ADR 0012 item 3 (SHR-38): a transport records its metrics into the runtime context its config
// names, and into the default context only when the config names none. NNG on 127.0.0.1 with a
// real socket and an ephemeral port; the sender's worker is paused, so every accepted frame
// stays queued and the counts are exact. MoQ and WebRTC follow the same pattern; the CI check
// Build/Scripts/check-transport-metrics.py keeps every transport off FO3DPerformanceMetrics::Get().

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG

#include "Testing/NngTesting.h"

#include "Misc/AutomationTest.h"
#include "O3DRuntimeContext.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"

namespace O3DNngRuntimeContextTests
{
	constexpr int32 FrameBytes = 100;
	constexpr int32 NumFrames = 3;

	FO3DTransportConfig MakeConfig(int32 Port, const FO3DRuntimeContextPtr& Context)
	{
		FO3DTransportConfig Config(TEXT("NNG"), EO3DTransportRole::Sender);
		Config.AdvancedParams.Add(TEXT("nng.mode"), TEXT("pair"));
		Config.AdvancedParams.Add(TEXT("nng.role"), TEXT("server"));
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		Config.Context = Context;
		return Config;
	}

	uint64 TransportFramesSent(FO3DRuntimeContext& Context)
	{
		const TSharedPtr<FO3DTransportMetrics, ESPMode::ThreadSafe> Metrics = Context.GetMetrics().FindTransportMetrics(TEXT("NNG"));
		return Metrics.IsValid() ? Metrics->FramesSent.load() : 0;
	}

	/** Starts a listening sender with Config, sends NumFrames frames with its worker paused, and stops it. */
	bool SendFrames(FAutomationTestBase& Test, const FO3DTransportConfig& Config)
	{
		const TSharedRef<IOpen3DSender> Sender = O3DNngTesting::CreateSender();
		if (!Test.TestTrue(TEXT("Sender initializes"), Sender->Initialize(Config).IsOk())
			|| !Test.TestTrue(TEXT("Sender starts"), Sender->Start().IsOk()))
		{
			Sender->Stop();
			return false;
		}
		O3DNngTesting::SenderSetWorkerPaused(*Sender, true);
		TArray<uint8> Frame;
		Frame.SetNumZeroed(FrameBytes);
		int32 Queued = 0;
		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			Queued += Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Ctx"), 0.0)) == EO3DSendResult::Queued ? 1 : 0;
		}
		O3DNngTesting::SenderSetWorkerPaused(*Sender, false);
		Sender->Stop();
		return Test.TestEqual(TEXT("Every frame queued"), Queued, NumFrames);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngRuntimeContextTest, "Open3DBroadcast.Transport.NNG.RecordsIntoConfigContext", O3DB_TEST_FLAGS)
bool FO3DNngRuntimeContextTest::RunTest(const FString& Parameters)
{
	using namespace O3DNngRuntimeContextTests;
	FO3DRuntimeContext& Default = *FO3DRuntimeContext::Default();
	const FO3DPerformanceMetrics::FSenderMetrics& DefaultSender = Default.GetMetrics().GetSenderMetrics();

	// A named context: its counters move, the default context's do not.
	{
		const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
		if (!TestTrue(TEXT("Loopback TCP port allocated"), Port > 0))
		{
			return false;
		}
		const FO3DRuntimeContextRef Context = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.Nng"));
		const uint64 DefaultCapturedBefore = DefaultSender.FramesCaptured.load();
		const uint64 DefaultBytesSentBefore = DefaultSender.BytesSent.load();
		const uint64 DefaultTransportBefore = TransportFramesSent(Default);

		if (!SendFrames(*this, MakeConfig(Port, Context)))
		{
			return false;
		}

		const FO3DPerformanceMetrics::FSenderMetrics& Sender = Context->GetMetrics().GetSenderMetrics();
		TestEqual(TEXT("Context: frames captured"), Sender.FramesCaptured.load(), static_cast<uint64>(NumFrames));
		TestEqual(TEXT("Context: bytes serialized"), Sender.BytesSerialized.load(), static_cast<uint64>(NumFrames * FrameBytes));
		TestEqual(TEXT("Context: bytes sent"), Sender.BytesSent.load(), static_cast<uint64>(NumFrames * FrameBytes));
		TestEqual(TEXT("Context: NNG transport frames"), TransportFramesSent(*Context), static_cast<uint64>(NumFrames));

		TestEqual(TEXT("Default: frames captured unchanged"), DefaultSender.FramesCaptured.load(), DefaultCapturedBefore);
		TestEqual(TEXT("Default: bytes sent unchanged"), DefaultSender.BytesSent.load(), DefaultBytesSentBefore);
		TestEqual(TEXT("Default: NNG transport frames unchanged"), TransportFramesSent(Default), DefaultTransportBefore);
	}

	// No context in the config: the default context, as before ADR 0012.
	{
		const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
		if (!TestTrue(TEXT("Loopback TCP port allocated"), Port > 0))
		{
			return false;
		}
		const uint64 DefaultCapturedBefore = DefaultSender.FramesCaptured.load();
		const uint64 DefaultTransportBefore = TransportFramesSent(Default);

		if (!SendFrames(*this, MakeConfig(Port, nullptr)))
		{
			return false;
		}

		TestEqual(TEXT("Default: frames captured"), DefaultSender.FramesCaptured.load() - DefaultCapturedBefore, static_cast<uint64>(NumFrames));
		TestEqual(TEXT("Default: NNG transport frames"), TransportFramesSent(Default) - DefaultTransportBefore, static_cast<uint64>(NumFrames));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngSenderMetricsHandleTest, "Open3DBroadcast.Transport.NNG.RecordsIntoConfigSenderMetrics", O3DB_TEST_FLAGS)
bool FO3DNngSenderMetricsHandleTest::RunTest(const FString& Parameters)
{
	using namespace O3DNngRuntimeContextTests;
	const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
	if (!TestTrue(TEXT("Loopback TCP port allocated"), Port > 0))
	{
		return false;
	}
	const FO3DRuntimeContextRef Context = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.NngSenderMetrics"));
	const FO3DSenderMetricsHandleRef Handle = Context->GetMetrics().AcquireSenderMetrics(TEXT("Test sender component"));

	// A handle from another context is refused before anything starts.
	{
		const FO3DRuntimeContextRef Other = MakeShared<FO3DRuntimeContext, ESPMode::ThreadSafe>(TEXT("O3DTest.NngOther"));
		FO3DTransportConfig Foreign = MakeConfig(Port, Other);
		Foreign.SenderMetrics = Handle;
		const TSharedRef<IOpen3DSender> Sender = O3DNngTesting::CreateSender();
		const FO3DTransportResult Result = Sender->Initialize(Foreign);
		TestTrue(TEXT("A foreign sender metrics handle is InvalidConfig"), !Result.IsOk() && Result.Code == EO3DTransportError::InvalidConfig);
	}

	FO3DTransportConfig Config = MakeConfig(Port, Context);
	Config.SenderMetrics = Handle;
	if (!SendFrames(*this, Config))
	{
		return false;
	}
	TestEqual(TEXT("The provided handle counts the frames"), Handle->GetCounters().FramesCaptured.load(), static_cast<uint64>(NumFrames));
	TestEqual(TEXT("And the bytes sent"), Handle->GetCounters().BytesSent.load(), static_cast<uint64>(NumFrames * FrameBytes));
	TestEqual(TEXT("The context's aggregate equals the handle"), Context->GetMetrics().GetSenderMetrics().FramesCaptured.load(), static_cast<uint64>(NumFrames));
	TestEqual(TEXT("The transport made no handle of its own"), Context->GetMetrics().GetSenderHandles().Num(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG
