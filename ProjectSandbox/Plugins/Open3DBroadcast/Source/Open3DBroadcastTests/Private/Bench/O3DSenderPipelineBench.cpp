// Copyright Lifelike & Believable. All Rights Reserved.

// The ADR 0008 measurement scene (Decision item 11, WP-A2c addendum "Insights numbers"): one, then
// ten, 250-bone, 250-curve senders at 60 Hz, on Loopback and on UDP, with o3d.Sender.AsyncPipeline
// 0 (before) and 1 (after). Each case runs inside an Unreal Insights region named
// "O3D.Bench.<Transport>_<Senders>_Async<0|1>", so a trace of one run splits into the eight cases;
// Build/Scripts/Run-SenderBenchmark.py records the trace and summarizes it.
//
// Not a test: it checks nothing and registers no instances unless O3DB_BENCH=1, so it is absent
// from every default run and from CI (like the O3DB_NETWORK_TESTS tests, ADR 0006 §6). It paces
// the world at 60 Hz of wall time (a short sleep each frame), because the queue behaviour it
// measures depends on the frame rate.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DSenderComponent.h"
#include "O3DSenderPipelineStats.h"
#include "Sender/O3DTestSkeletalMesh.h"

#include "Components/SkeletalMeshComponent.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "UObject/Package.h"

namespace O3DSenderPipelineBench
{
	constexpr int32 NumBones = 250;
	constexpr int32 NumCurves = 250;
	constexpr int32 WarmupFrames = 60;
	constexpr int32 MeasuredFrames = 600; // 10 s at 60 Hz
	constexpr double FrameSeconds = 1.0 / 60.0;

	bool IsEnabled()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("O3DB_BENCH")) == TEXT("1");
	}

	/** A standalone game world with its own world context, destroyed when the scope ends (pitfall 26). */
	class FBenchWorld
	{
	public:
		FBenchWorld()
		{
			if (GEngine == nullptr)
			{
				return;
			}
			World = UWorld::CreateWorld(EWorldType::Game, false);
			if (World == nullptr)
			{
				return;
			}
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
		}

		~FBenchWorld()
		{
			if (World != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
				World->RemoveFromRoot();
			}
		}

		FBenchWorld(const FBenchWorld&) = delete;
		FBenchWorld& operator=(const FBenchWorld&) = delete;

		UWorld* Get() const { return World; }

	private:
		UWorld* World = nullptr;
	};

	/** Sets o3d.Sender.AsyncPipeline for the scope (read at StartCapture). */
	class FScopedAsyncPipeline
	{
	public:
		explicit FScopedAsyncPipeline(bool bAsync)
			: Variable(IConsoleManager::Get().FindConsoleVariable(TEXT("o3d.Sender.AsyncPipeline")))
		{
			if (Variable)
			{
				Previous = Variable->GetInt();
				Variable->Set(bAsync ? 1 : 0, ECVF_SetByCode);
			}
		}

		~FScopedAsyncPipeline()
		{
			if (Variable)
			{
				Variable->Set(Previous, ECVF_SetByCode);
			}
		}

		FScopedAsyncPipeline(const FScopedAsyncPipeline&) = delete;
		FScopedAsyncPipeline& operator=(const FScopedAsyncPipeline&) = delete;

	private:
		IConsoleVariable* Variable = nullptr;
		int32 Previous = 1;
	};

	double Percentile(TArray<double> Values, double Fraction)
	{
		if (Values.Num() == 0)
		{
			return 0.0;
		}
		Values.Sort();
		const int32 Index = FMath::Clamp(FMath::CeilToInt(Fraction * Values.Num()) - 1, 0, Values.Num() - 1);
		return Values[Index];
	}

	/** One tick of the world, then a wait until FrameSeconds of wall time have passed since FrameStart. */
	void TickPaced(UWorld* World, double FrameStart)
	{
		++GFrameCounter; // pitfall 28
		World->Tick(LEVELTICK_All, static_cast<float>(FrameSeconds));
		for (;;)
		{
			const double Remaining = FrameSeconds - (FPlatformTime::Seconds() - FrameStart);
			if (Remaining <= 0.0)
			{
				break;
			}
			FPlatformProcess::Sleep(static_cast<float>(FMath::Min(Remaining, 0.001)));
		}
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FO3DSenderPipelineBench, "Open3DBroadcast.Bench.SenderPipeline", O3DB_TEST_FLAGS)

void FO3DSenderPipelineBench::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	if (!O3DSenderPipelineBench::IsEnabled())
	{
		return;
	}
	for (const TCHAR* Transport : { TEXT("Loopback"), TEXT("UDP") })
	{
		for (const int32 Senders : { 1, 10 })
		{
			for (const int32 Async : { 0, 1 })
			{
				const FString Case = FString::Printf(TEXT("%s_%d_Async%d"), Transport, Senders, Async);
				OutBeautifiedNames.Add(Case);
				OutTestCommands.Add(Case);
			}
		}
	}
}

bool FO3DSenderPipelineBench::RunTest(const FString& Parameters)
{
	using namespace O3DSenderPipelineBench;

	TArray<FString> Parts;
	Parameters.ParseIntoArray(Parts, TEXT("_"));
	if (!TestEqual(TEXT("Case is <Transport>_<Senders>_Async<0|1>"), Parts.Num(), 3))
	{
		return false;
	}
	const FName Transport(*Parts[0]);
	const int32 NumSenders = FCString::Atoi(*Parts[1]);
	const bool bAsync = Parts[2] == TEXT("Async1");

	FScopedAsyncPipeline AsyncScope(bAsync);
	FBenchWorld BenchWorld;
	UWorld* World = BenchWorld.Get();
	if (!TestNotNull(TEXT("A standalone game world"), World))
	{
		return false;
	}

	USkeletalMesh* MeshAsset = O3DTestSkeletalMesh::CreateChainMesh(GetTransientPackage(), NumBones, NumCurves);
	TArray<UO3DSenderComponent*> Senders;
	for (int32 Index = 0; Index < NumSenders; ++Index)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		USkeletalMeshComponent* Mesh = NewObject<USkeletalMeshComponent>(Actor);
		Mesh->SetSkeletalMeshAsset(MeshAsset);
		Mesh->SetAnimInstanceClass(UO3DRootMoverAnimInstance::StaticClass());
		Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

		UO3DSenderComponent* Sender = NewObject<UO3DSenderComponent>(Actor);
		Sender->bAutoStartCapture = false;
		Sender->bAutoCreateTransport = true;
		Sender->SetTransportName(Transport);
		Sender->SubjectName = FString::Printf(TEXT("Bench%02d"), Index);
		Sender->CaptureRateHz = 0.0f; // the world is paced at 60 Hz; sample every frame
		Sender->TargetMesh = Mesh;

		Mesh->RegisterComponent();
		Sender->RegisterComponent();
		Actor->DispatchBeginPlay();
		Sender->StartCapture();
		Senders.Add(Sender);
	}

	for (int32 Frame = 0; Frame < WarmupFrames; ++Frame)
	{
		TickPaced(World, FPlatformTime::Seconds());
	}

	const FString Region = FString::Printf(TEXT("O3D.Bench.%s"), *Parameters);
	TArray<double> LatencyMs;
	LatencyMs.Reserve(MeasuredFrames * NumSenders);
	TArray<FO3DSenderPipelineStats> Before;
	for (UO3DSenderComponent* Sender : Senders)
	{
		Before.Add(Sender->GetPipelineStats());
	}

	TRACE_BEGIN_REGION(*Region);
	const double MeasureStart = FPlatformTime::Seconds();
	for (int32 Frame = 0; Frame < MeasuredFrames; ++Frame)
	{
		TickPaced(World, FPlatformTime::Seconds());
		for (UO3DSenderComponent* Sender : Senders)
		{
			LatencyMs.Add(Sender->GetPipelineStats().LastCaptureToSendSeconds * 1000.0);
		}
	}
	const double MeasureSeconds = FPlatformTime::Seconds() - MeasureStart;
	TRACE_END_REGION(*Region);

	uint64 Submitted = 0;
	uint64 Dropped = 0;
	uint64 Accepted = 0;
	uint64 Refused = 0;
	int32 MaxQueued = 0;
	double MaxWorkerMs = 0.0;
	double MaxLatencyMs = 0.0;
	for (int32 Index = 0; Index < Senders.Num(); ++Index)
	{
		const FO3DSenderPipelineStats After = Senders[Index]->GetPipelineStats();
		Submitted += After.FramesSubmitted - Before[Index].FramesSubmitted;
		Dropped += After.FramesDropped - Before[Index].FramesDropped;
		Accepted += After.PayloadsAccepted - Before[Index].PayloadsAccepted;
		Refused += After.PayloadsRefused - Before[Index].PayloadsRefused;
		MaxQueued = FMath::Max(MaxQueued, After.MaxQueuedFrames);
		MaxWorkerMs = FMath::Max(MaxWorkerMs, After.MaxWorkerSeconds * 1000.0);
		MaxLatencyMs = FMath::Max(MaxLatencyMs, After.MaxCaptureToSendSeconds * 1000.0);
		Senders[Index]->StopCapture();
	}

	// One line per case; Run-SenderBenchmark.py reads it from the log.
	const FString Result = FString::Printf(TEXT("O3D_BENCH case=%s senders=%d frames=%d seconds=%.3f submitted=%llu dropped=%llu accepted=%llu refused=%llu max_queued=%d max_worker_ms=%.4f latency_ms_p50=%.4f latency_ms_p99=%.4f latency_ms_max=%.4f"),
		*Parameters, NumSenders, MeasuredFrames, MeasureSeconds,
		(unsigned long long)Submitted, (unsigned long long)Dropped, (unsigned long long)Accepted, (unsigned long long)Refused,
		MaxQueued, MaxWorkerMs, Percentile(LatencyMs, 0.5), Percentile(LatencyMs, 0.99), MaxLatencyMs);
	UE_LOG(LogTemp, Display, TEXT("%s"), *Result);
	AddInfo(Result);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
