// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U3 (SND-27, SND-30): the sender component's target mesh resolution and edit-time validation.
// - Target Mesh (the picker, FComponentReference) names the captured mesh; unset, a mesh already
//   set (Blueprint, or a level saved before the picker) is kept; with neither, the owner's
//   skeletal mesh that drives its own pose is preferred over a follower of a leader pose.
// - A picker naming something that is not a skeletal mesh falls back, with a warning.
// - Editing the quantization ranges keeps the 16-bit range at or above the 8-bit range.
// An actor in the transient package, no world.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "O3DSenderComponent.h"
#include "Testing/O3DSenderTesting.h"

namespace O3DSenderDetailsTest
{
	template <typename T>
	T* AddComponent(AActor& Actor, const TCHAR* Name)
	{
		T* Component = NewObject<T>(&Actor, Name);
		Actor.AddOwnedComponent(Component);
		return Component;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTargetMeshResolutionTest, "Open3DBroadcast.Sender.TargetMesh.Resolution", O3DB_TEST_FLAGS)
bool FO3DSenderTargetMeshResolutionTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderDetailsTest;

	AActor* Actor = NewObject<AActor>(GetTransientPackage(), *O3DTests::MakeUniqueName(TEXT("O3DTargetMeshActor")));
	// The follower comes first, so a plain "first skeletal mesh" would pick it.
	USkeletalMeshComponent* Follower = AddComponent<USkeletalMeshComponent>(*Actor, TEXT("Follower"));
	USkeletalMeshComponent* Leader = AddComponent<USkeletalMeshComponent>(*Actor, TEXT("Leader"));
	Follower->LeaderPoseComponent = Leader;
	USceneComponent* NotAMesh = AddComponent<USceneComponent>(*Actor, TEXT("NotAMesh"));
	UO3DSenderComponent* Sender = AddComponent<UO3DSenderComponent>(*Actor, TEXT("Sender"));

	// Nothing set: the mesh that drives its own pose.
	FO3DSenderComponentTestAccess::ResolveTargetMesh(*Sender);
	TestTrue(TEXT("Auto: the leader, not the follower"), Sender->TargetMesh.Get() == Leader);

	// The picker wins over a mesh set the old way.
	Sender->TargetMesh = Leader;
	Sender->TargetMeshComponent.PathToComponent = TEXT("Follower");
	FO3DSenderComponentTestAccess::ResolveTargetMesh(*Sender);
	TestTrue(TEXT("Picker: the follower it names"), Sender->TargetMesh.Get() == Follower);

	// An unset picker keeps a mesh set by Blueprint or a level saved before the picker existed.
	Sender->TargetMeshComponent = FComponentReference();
	Sender->TargetMesh = Follower;
	FO3DSenderComponentTestAccess::ResolveTargetMesh(*Sender);
	TestTrue(TEXT("Unset picker: the existing mesh is kept"), Sender->TargetMesh.Get() == Follower);

	// A picker naming a component that is not a skeletal mesh falls back to the automatic choice.
	Sender->TargetMesh.Reset();
	Sender->TargetMeshComponent.PathToComponent = NotAMesh->GetName();
	AddExpectedError(TEXT("does not name a skeletal mesh component"), EAutomationExpectedMessageFlags::Contains, 1);
	FO3DSenderComponentTestAccess::ResolveTargetMesh(*Sender);
	TestTrue(TEXT("Wrong picker: the automatic choice"), Sender->TargetMesh.Get() == Leader);
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderQuantizationRangeEditTest, "Open3DBroadcast.Sender.Details.QuantizationRangesStayOrdered", O3DB_TEST_FLAGS)
bool FO3DSenderQuantizationRangeEditTest::RunTest(const FString& Parameters)
{
	UO3DSenderComponent* Sender = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Sender->QuantizationByteRange = 0.5f;
	Sender->QuantizationHalfRange = 0.1f;

	FProperty* Property = UO3DSenderComponent::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, QuantizationHalfRange));
	if (!TestNotNull(TEXT("Property found"), Property))
	{
		return false;
	}
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	static_cast<UObject*>(Sender)->PostEditChangeProperty(Event); // private in the component, public on UObject
	TestEqual(TEXT("The 16-bit range is raised to the 8-bit range"), Sender->QuantizationHalfRange, 0.5f);

	Sender->QuantizationHalfRange = 2.0f;
	static_cast<UObject*>(Sender)->PostEditChangeProperty(Event); // private in the component, public on UObject
	TestEqual(TEXT("A larger 16-bit range is left alone"), Sender->QuantizationHalfRange, 2.0f);
	return true;
}
#endif

#endif // WITH_DEV_AUTOMATION_TESTS
