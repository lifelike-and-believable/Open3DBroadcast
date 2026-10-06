// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U4: the remote audio component in a game world. RCV-22 (it re-parented a component the user
// placed, and tried to attach to itself as the root) and RCV-23 (the internal UAudioComponent and
// the procedural wave outlived EndPlay). The editor runs these with -NoSound, so there is no audio
// device and UAudioComponent::Play is a no-op here; playback itself is not observable.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DRemoteAudioComponent.h"
#include "Testing/O3DReceiverTesting.h"

#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Sound/SoundWaveProcedural.h"

namespace O3DRemoteAudioLifecycleTests
{
	/** A standalone game world with its own world context, destroyed when the scope ends (as in O3DSenderRootBoneTests.cpp). */
	class FTestGameWorld
	{
	public:
		FTestGameWorld()
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

		~FTestGameWorld()
		{
			if (World != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
				World->RemoveFromRoot();
			}
		}

		FTestGameWorld(const FTestGameWorld&) = delete;
		FTestGameWorld& operator=(const FTestGameWorld&) = delete;

		UWorld* Get() const { return World; }

	private:
		UWorld* World = nullptr;
	};

	void QueueOneFrame(UO3DRemoteAudioComponent* Component)
	{
		O3DS::FAudioFrameMeta Meta;
		Meta.StreamLabel = TEXT("o3ds:mix");
		Meta.NumChannels = 1;
		Meta.SampleRate = 48000;
		TArray<uint8> PCM16;
		PCM16.SetNumZeroed(960 * sizeof(int16));
		FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16(Component, Meta, PCM16);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioAttachmentTest, "Open3DBroadcast.Receiver.RemoteAudioComponent.KeepsTheParentTheUserChose", O3DB_TEST_FLAGS)
bool FO3DRemoteAudioAttachmentTest::RunTest(const FString& Parameters)
{
	using namespace O3DRemoteAudioLifecycleTests;

	FTestGameWorld TestWorld;
	UWorld* World = TestWorld.Get();
	if (!TestNotNull(TEXT("A standalone game world"), World))
	{
		return false;
	}

	// Placed under a child of the root, with no Attach Parent set: it stays there.
	AActor* Actor = World->SpawnActor<AActor>();
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
	Actor->SetRootComponent(Root);
	Root->RegisterComponent();
	USceneComponent* Child = NewObject<USceneComponent>(Actor, TEXT("Child"));
	Child->SetupAttachment(Root);
	Child->RegisterComponent();
	UO3DRemoteAudioComponent* Placed = NewObject<UO3DRemoteAudioComponent>(Actor, TEXT("Placed"));
	Placed->SetupAttachment(Child);
	Placed->RegisterComponent();
	TestTrue(TEXT("A component placed under a child keeps that parent"), Placed->GetAttachParent() == Child);

	// The root of its actor: nothing to attach to, and no attempt to attach to itself.
	AActor* RootActor = World->SpawnActor<AActor>();
	UO3DRemoteAudioComponent* AsRoot = NewObject<UO3DRemoteAudioComponent>(RootActor, TEXT("AsRoot"));
	RootActor->SetRootComponent(AsRoot);
	AsRoot->RegisterComponent();
	TestNull(TEXT("The root component has no parent"), AsRoot->GetAttachParent());

	// Attach Parent set explicitly: it is applied.
	AActor* ExplicitActor = World->SpawnActor<AActor>();
	USceneComponent* ExplicitRoot = NewObject<USceneComponent>(ExplicitActor, TEXT("Root"));
	ExplicitActor->SetRootComponent(ExplicitRoot);
	ExplicitRoot->RegisterComponent();
	USceneComponent* Target = NewObject<USceneComponent>(ExplicitActor, TEXT("Target"));
	Target->SetupAttachment(ExplicitRoot);
	Target->RegisterComponent();
	UO3DRemoteAudioComponent* Explicit = NewObject<UO3DRemoteAudioComponent>(ExplicitActor, TEXT("Explicit"));
	Explicit->AC_AttachParent.PathToComponent = TEXT("Target");
	Explicit->SetupAttachment(ExplicitRoot);
	Explicit->RegisterComponent();
	TestTrue(TEXT("An explicit Attach Parent is applied"), Explicit->GetAttachParent() == Target);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioEndPlayTest, "Open3DBroadcast.Receiver.RemoteAudioComponent.EndPlayReleasesAudio", O3DB_TEST_FLAGS)
bool FO3DRemoteAudioEndPlayTest::RunTest(const FString& Parameters)
{
	using namespace O3DRemoteAudioLifecycleTests;

	FTestGameWorld TestWorld;
	UWorld* World = TestWorld.Get();
	if (!TestNotNull(TEXT("A standalone game world"), World))
	{
		return false;
	}

	AActor* Actor = World->SpawnActor<AActor>();
	UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>(Actor);
	Actor->SetRootComponent(Component);
	Component->RegisterComponent();
	Actor->DispatchBeginPlay();

	UAudioComponent* Inner = FO3DRemoteAudioComponentTestAccessor::GetAudioComponent(Component);
	if (!TestNotNull(TEXT("BeginPlay creates the internal audio component"), Inner))
	{
		return false;
	}
	QueueOneFrame(Component);
	TestNotNull(TEXT("A frame creates the procedural wave"), FO3DRemoteAudioComponentTestAccessor::GetSoundWave(Component));

	Actor->RouteEndPlay(EEndPlayReason::RemovedFromWorld);

	TestNull(TEXT("EndPlay releases the internal audio component"), FO3DRemoteAudioComponentTestAccessor::GetAudioComponent(Component));
	TestTrue(TEXT("The internal audio component is destroyed"), !IsValid(Inner) || Inner->IsBeingDestroyed() || !Inner->IsRegistered());
	TestNull(TEXT("EndPlay releases the procedural wave"), FO3DRemoteAudioComponentTestAccessor::GetSoundWave(Component));

	return true;
}

// RCV-23: with Auto Activate off, received audio is queued but not played until Play. Whether the
// engine then plays is not observable under -NoSound, so this checks the component's intent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioAutoActivateTest, "Open3DBroadcast.Receiver.RemoteAudioComponent.AutoActivateOffWaitsForPlay", O3DB_TEST_FLAGS)
bool FO3DRemoteAudioAutoActivateTest::RunTest(const FString& Parameters)
{
	using namespace O3DRemoteAudioLifecycleTests;

	FTestGameWorld TestWorld;
	UWorld* World = TestWorld.Get();
	if (!TestNotNull(TEXT("A standalone game world"), World))
	{
		return false;
	}

	AActor* Actor = World->SpawnActor<AActor>();
	UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>(Actor);
	Component->bAC_AutoActivate = false;
	Actor->SetRootComponent(Component);
	Component->RegisterComponent();
	Actor->DispatchBeginPlay();

	UAudioComponent* Inner = FO3DRemoteAudioComponentTestAccessor::GetAudioComponent(Component);
	if (!TestNotNull(TEXT("BeginPlay creates the internal audio component"), Inner))
	{
		return false;
	}
	TestFalse(TEXT("The internal component never auto-activates"), Inner->bAutoActivate);
	TestTrue(TEXT("The internal component is attached to this one"), Inner->GetAttachParent() == Component);

	QueueOneFrame(Component);
	TestNotNull(TEXT("Audio is queued"), FO3DRemoteAudioComponentTestAccessor::GetSoundWave(Component));
	TestFalse(TEXT("Not played without Play"), FO3DRemoteAudioComponentTestAccessor::IsPlaybackWanted(Component));

	Component->Play();
	TestTrue(TEXT("Play starts playback"), FO3DRemoteAudioComponentTestAccessor::IsPlaybackWanted(Component));
	Component->Stop();
	TestFalse(TEXT("Stop stops it"), FO3DRemoteAudioComponentTestAccessor::IsPlaybackWanted(Component));

	Actor->RouteEndPlay(EEndPlayReason::RemovedFromWorld);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
