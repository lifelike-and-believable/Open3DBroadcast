// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// ADR 0008 Verification (WP-A2b, item 9): "a test that moves the root bone each frame shows the
// captured transform equals the same frame's evaluated pose". The mesh is built in code
// (O3DTestSkeletalMesh.h) because the automation host project has no mesh asset; its anim instance
// moves the root bone to a position derived from GFrameCounter, so every frame's evaluated pose is
// known. The sender samples in TG_PostUpdateWork after the mesh's tick, so what it captures in a
// frame must be that frame's pose, never the previous one.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DSenderComponent.h"
#include "O3DTestSkeletalMesh.h"

#include "Components/SkeletalMeshComponent.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace O3DSenderRootBoneTests
{
	/** A standalone game world with its own world context, destroyed when the scope ends (pitfall 26). */
	class FRootBoneTestWorld
	{
	public:
		FRootBoneTestWorld()
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

		~FRootBoneTestWorld()
		{
			if (World != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
				World->RemoveFromRoot();
			}
		}

		FRootBoneTestWorld(const FRootBoneTestWorld&) = delete;
		FRootBoneTestWorld& operator=(const FRootBoneTestWorld&) = delete;

		UWorld* Get() const { return World; }

	private:
		UWorld* World = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderRootBoneTest, "Open3DBroadcast.Sender.TickOrder.CapturedRootEqualsEvaluatedPose", O3DB_TEST_FLAGS)
bool FO3DSenderRootBoneTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderRootBoneTests;

	FRootBoneTestWorld TestWorld;
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

	const int32 NumBones = 4;
	USkeletalMesh* MeshAsset = O3DTestSkeletalMesh::CreateChainMesh(GetTransientPackage(), NumBones, 0);
	if (!TestNotNull(TEXT("A skeletal mesh built in code"), MeshAsset))
	{
		return false;
	}
	TestEqual(TEXT("It has the requested bones"), MeshAsset->GetRefSkeleton().GetNum(), NumBones);

	USkeletalMeshComponent* Mesh = NewObject<USkeletalMeshComponent>(Actor);
	Mesh->SetSkeletalMeshAsset(MeshAsset);
	Mesh->SetAnimInstanceClass(UO3DRootMoverAnimInstance::StaticClass());
	// Evaluate every frame even though nothing renders the mesh.
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	// Default evaluation path otherwise (parallel evaluation where the engine uses it): whether it
	// has finished when the sender samples is what ADR 0008 open question 2 asks.

	UO3DSenderComponent* Sender = NewObject<UO3DSenderComponent>(Actor);
	Sender->bAutoStartCapture = false;
	Sender->CaptureRateHz = 0.0f; // no rate limit: every tick samples
	Sender->TargetMesh = Mesh;

	Mesh->RegisterComponent();
	Sender->RegisterComponent();
	Actor->DispatchBeginPlay();

	Sender->StartCapture();
	if (!TestTrue(TEXT("Capture started"), Sender->IsCapturing()))
	{
		return false;
	}

	int32 Sampled = 0;
	int32 Mismatches = 0;
	FString FirstMismatch;
	const FDelegateHandle Handle = Sender->OnPoseFrameReady.AddLambda([&](const FString& /*Subject*/, const FO3DSPoseFrame& Frame)
	{
		if (Frame.BoneLocalTransforms.Num() != NumBones)
		{
			++Mismatches;
			return;
		}
		++Sampled;
		const double Expected = O3DTestSkeletalMesh::RootXForFrame(GFrameCounter);
		const double Captured = Frame.BoneLocalTransforms[0].GetTranslation().X;
		if (!FMath::IsNearlyEqual(Captured, Expected, 1e-3))
		{
			if (Mismatches == 0)
			{
				FirstMismatch = FString::Printf(TEXT("frame %llu: captured root X %.3f, this frame's pose %.3f"), (unsigned long long)GFrameCounter, Captured, Expected);
			}
			++Mismatches;
		}
	});

	const int32 NumFrames = 30;
	int32 PoseMismatches = 0;
	for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
	{
		++GFrameCounter; // pitfall 28: the engine loop does this once per frame
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);

		// The component's own evaluated pose this frame (sanity check of the test mesh).
		const TArray<FTransform>& ComponentSpace = Mesh->GetComponentSpaceTransforms();
		if (ComponentSpace.Num() != NumBones || !FMath::IsNearlyEqual(ComponentSpace[0].GetTranslation().X, O3DTestSkeletalMesh::RootXForFrame(GFrameCounter), 1e-3))
		{
			++PoseMismatches;
		}
	}

	Sender->OnPoseFrameReady.Remove(Handle);
	Sender->StopCapture();

	TestEqual(TEXT("The mesh evaluates the moving root every frame"), PoseMismatches, 0);
	TestTrue(FString::Printf(TEXT("The sender sampled most frames (%d of %d)"), Sampled, NumFrames), Sampled >= NumFrames - 1);
	TestEqual(FString::Printf(TEXT("Every captured root equals that frame's evaluated pose %s"), *FirstMismatch), Mismatches, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
