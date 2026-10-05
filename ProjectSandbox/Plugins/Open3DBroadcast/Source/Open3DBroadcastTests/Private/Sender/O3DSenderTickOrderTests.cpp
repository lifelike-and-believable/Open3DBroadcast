// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-A2b (ADR 0008 implementation outline item 3, SND-12): the sender's capture tick runs in
// TG_PostUpdateWork and waits for the target skeletal mesh's tick, so it samples the pose the mesh
// ends the frame with. White-box access goes through Open3DSender/Public/Testing/O3DSenderTesting.h
// (ADR 0006).
//
// ADR 0008's acceptance test ("move the root bone each frame; the captured transform equals that
// frame's evaluated pose") needs a skeletal mesh asset with a skeleton, and the plugin has none: the
// automation host project that Run-AutomationTests.ps1 builds holds only the packaged plugin.
// SamplesAfterTargetMeshEachFrame checks what that test rests on instead: in a ticking world, every
// frame, the sender samples after the mesh's tick has finished, with both in the same tick group.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DSenderComponent.h"
#include "O3DTickOrderProbeMeshComponent.h"
#include "Testing/O3DSenderTesting.h"

#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace O3DSenderTickOrderTests
{
	/** How many of Component's tick prerequisites are Target's primary tick. */
	int32 CountPrerequisitesOn(const UActorComponent& Component, const UActorComponent& Target)
	{
		int32 Count = 0;
		for (const FTickPrerequisite& Prerequisite : Component.PrimaryComponentTick.GetPrerequisites())
		{
			if (Prerequisite.PrerequisiteTickFunction == &Target.PrimaryComponentTick)
			{
				++Count;
			}
		}
		return Count;
	}

	/** A standalone game world with its own world context, destroyed when the scope ends. */
	class FTickOrderTestWorld
	{
	public:
		FTickOrderTestWorld()
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

		~FTickOrderTestWorld()
		{
			if (World != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
				// Harmless when CreateWorld did not root it; otherwise the world could never be collected.
				World->RemoveFromRoot();
			}
		}

		FTickOrderTestWorld(const FTickOrderTestWorld&) = delete;
		FTickOrderTestWorld& operator=(const FTickOrderTestWorld&) = delete;

		UWorld* Get() const { return World; }

	private:
		UWorld* World = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTickGroupTest, "Open3DBroadcast.Sender.TickOrder.TickGroupIsPostUpdateWork", O3DB_TEST_FLAGS)
bool FO3DSenderTickGroupTest::RunTest(const FString& Parameters)
{
	const UO3DSenderComponent* ClassDefault = GetDefault<UO3DSenderComponent>();
	TestTrue(TEXT("The class default ticks in TG_PostUpdateWork"), ClassDefault->PrimaryComponentTick.TickGroup == TG_PostUpdateWork);

	const UO3DSenderComponent* Sender = NewObject<UO3DSenderComponent>(GetTransientPackage());
	TestTrue(TEXT("A new component ticks in TG_PostUpdateWork"), Sender->PrimaryComponentTick.TickGroup == TG_PostUpdateWork);
	TestTrue(TEXT("It can still tick"), Sender->PrimaryComponentTick.bCanEverTick);
	TestEqual(TEXT("No prerequisite before a target is bound"), Sender->PrimaryComponentTick.GetPrerequisites().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTickPrerequisiteTest, "Open3DBroadcast.Sender.TickOrder.PrerequisiteFollowsTargetMesh", O3DB_TEST_FLAGS)
bool FO3DSenderTickPrerequisiteTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderTickOrderTests;

	// No world: StartCapture runs outside a game world check (GetWorld() is null), with no
	// transport (bAutoCreateTransport is false) and no audio. A mesh is set, so nothing warns.
	UO3DSenderComponent* Sender = NewObject<UO3DSenderComponent>(GetTransientPackage());
	USkeletalMeshComponent* MeshA = NewObject<USkeletalMeshComponent>(GetTransientPackage());
	USkeletalMeshComponent* MeshB = NewObject<USkeletalMeshComponent>(GetTransientPackage());
	if (!TestTrue(TEXT("A skeletal mesh component can tick (a prerequisite needs both sides ticking)"), MeshA->PrimaryComponentTick.bCanEverTick))
	{
		return false;
	}
	const TArray<FTickPrerequisite>& Prerequisites = Sender->PrimaryComponentTick.GetPrerequisites();

	// Bind.
	Sender->TargetMesh = MeshA;
	Sender->StartCapture();
	if (!TestTrue(TEXT("Capture started"), Sender->IsCapturing()))
	{
		return false;
	}
	TestEqual(TEXT("Binding adds the mesh's tick as a prerequisite"), CountPrerequisitesOn(*Sender, *MeshA), 1);
	TestEqual(TEXT("And nothing else"), Prerequisites.Num(), 1);

	// The per-tick check finds the mesh already bound and adds nothing.
	FO3DSenderComponentTestAccess::CanCaptureThisFrame(*Sender, 100.0);
	FO3DSenderComponentTestAccess::CanCaptureThisFrame(*Sender, 101.0);
	TestEqual(TEXT("Ticking does not add the prerequisite again"), CountPrerequisitesOn(*Sender, *MeshA), 1);
	TestEqual(TEXT("Still one prerequisite"), Prerequisites.Num(), 1);

	// Unbind.
	Sender->StopCapture();
	TestEqual(TEXT("Stopping removes the prerequisite"), CountPrerequisitesOn(*Sender, *MeshA), 0);
	TestEqual(TEXT("No prerequisite left after stopping"), Prerequisites.Num(), 0);

	// Unbinding when nothing is bound changes nothing.
	FO3DSenderComponentTestAccess::UnbindFromTarget(*Sender);
	Sender->StopCapture();
	TestEqual(TEXT("Unbinding twice leaves no prerequisite"), Prerequisites.Num(), 0);

	// Restart on another mesh, which is what a TargetMesh edit in the Details panel does.
	Sender->TargetMesh = MeshB;
	Sender->StartCapture();
	TestEqual(TEXT("Rebinding: the old mesh is not a prerequisite"), CountPrerequisitesOn(*Sender, *MeshA), 0);
	TestEqual(TEXT("Rebinding: the new mesh is"), CountPrerequisitesOn(*Sender, *MeshB), 1);
	TestEqual(TEXT("Rebinding leaves one prerequisite"), Prerequisites.Num(), 1);

	// TargetMesh written while capturing, as Blueprint can: the next capture check moves the prerequisite.
	Sender->TargetMesh = MeshA;
	FO3DSenderComponentTestAccess::CanCaptureThisFrame(*Sender, 200.0);
	TestEqual(TEXT("Switching while capturing: the new mesh is a prerequisite"), CountPrerequisitesOn(*Sender, *MeshA), 1);
	TestEqual(TEXT("Switching while capturing: the old mesh is not"), CountPrerequisitesOn(*Sender, *MeshB), 0);
	TestEqual(TEXT("Switching while capturing leaves one prerequisite"), Prerequisites.Num(), 1);

	// The bound mesh is destroyed while capturing: the next capture check removes its prerequisite.
	MeshA->DestroyComponent();
	TestFalse(TEXT("TargetMesh no longer resolves to the destroyed mesh"), Sender->TargetMesh.IsValid());
	TestFalse(TEXT("Nothing is sampled without a mesh"), FO3DSenderComponentTestAccess::CanCaptureThisFrame(*Sender, 300.0));
	TestEqual(TEXT("The destroyed mesh is no longer a prerequisite"), CountPrerequisitesOn(*Sender, *MeshA), 0);
	TestEqual(TEXT("No prerequisite left after the mesh was destroyed"), Prerequisites.Num(), 0);

	Sender->StopCapture();
	TestEqual(TEXT("Stopping after the mesh was destroyed leaves no prerequisite"), Prerequisites.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTickOrderInWorldTest, "Open3DBroadcast.Sender.TickOrder.SamplesAfterTargetMeshEachFrame", O3DB_TEST_FLAGS)
bool FO3DSenderTickOrderInWorldTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderTickOrderTests;

	// Expected by design: the probe mesh has no skeletal mesh asset, so frames carry no skeleton
	// descriptor and the serializer drops them with a warning, rate-limited to one per 5 s. The drop
	// happens on the pipeline's worker, and StopCapture discards frames still queued, so a run may
	// end before the worker has dropped (and warned about) any frame. Occurrences -1: any number of
	// times, none included (0 would demand at least one, AutomationTest.h).
	AddExpectedError(TEXT("no skeleton descriptor on the frame"), EAutomationExpectedMessageFlags::Contains, -1);

	FTickOrderTestWorld TestWorld;
	UWorld* World = TestWorld.Get();
	if (!TestNotNull(TEXT("A standalone game world"), World))
	{
		return false;
	}

	AActor* Actor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("An actor"), Actor))
	{
		return false;
	}

	UO3DSenderComponent* Sender = NewObject<UO3DSenderComponent>(Actor);
	Sender->bAutoStartCapture = false;
	Sender->CaptureRateHz = 0.0f; // no rate limit: every tick samples
	UO3DTickOrderProbeMeshComponent* Mesh = NewObject<UO3DTickOrderProbeMeshComponent>(Actor);
	Sender->TargetMesh = Mesh;

	// The sender is registered first, so within their shared tick group only the prerequisite puts
	// the mesh first. The world has no game mode, so the actor's BeginPlay (which registers the
	// component ticks) is dispatched here.
	Sender->RegisterComponent();
	Mesh->RegisterComponent();
	Actor->DispatchBeginPlay();

	Sender->StartCapture();
	if (!TestTrue(TEXT("Capture started"), Sender->IsCapturing()))
	{
		return false;
	}
	TestTrue(TEXT("The sender ticks in TG_PostUpdateWork"), Sender->PrimaryComponentTick.TickGroup == TG_PostUpdateWork);
	TestTrue(TEXT("The probe mesh ticks in the same group"), Mesh->PrimaryComponentTick.TickGroup == TG_PostUpdateWork);
	TestEqual(TEXT("The mesh is a prerequisite of the sender's tick"), CountPrerequisitesOn(*Sender, *Mesh), 1);

	int32 Sequence = 0;
	int32 MeshTickedAt = 0;
	int32 SampledAt = 0;
	Mesh->OnTicked = [&Sequence, &MeshTickedAt]()
	{
		MeshTickedAt = ++Sequence;
	};
	const FDelegateHandle SampleHandle = Sender->OnPoseFrameReady.AddLambda([&Sequence, &SampledAt](const FString& /*Subject*/, const FO3DSPoseFrame& /*SampledFrame*/)
	{
		SampledAt = ++Sequence;
	});

	constexpr int32 FrameCount = 8;
	for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
	{
		MeshTickedAt = 0;
		SampledAt = 0;
		// The engine loop advances GFrameCounter once per frame, and a tick function that already ran
		// in the current GFrameCounter frame is not queued again. Ticking the world directly does not
		// advance it, so without this only the first World->Tick runs the component ticks (#317).
		++GFrameCounter;
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);

		TestTrue(FString::Printf(TEXT("Frame %d: the mesh ticked"), FrameIndex), MeshTickedAt > 0);
		TestTrue(FString::Printf(TEXT("Frame %d: the sender sampled"), FrameIndex), SampledAt > 0);
		TestTrue(FString::Printf(TEXT("Frame %d: the sender sampled after the mesh's tick finished"), FrameIndex), SampledAt > MeshTickedAt);
	}

	Sender->OnPoseFrameReady.Remove(SampleHandle);
	Mesh->OnTicked = nullptr;

	Sender->StopCapture();
	TestEqual(TEXT("Stopping removes the prerequisite"), CountPrerequisitesOn(*Sender, *Mesh), 0);

	Actor->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
