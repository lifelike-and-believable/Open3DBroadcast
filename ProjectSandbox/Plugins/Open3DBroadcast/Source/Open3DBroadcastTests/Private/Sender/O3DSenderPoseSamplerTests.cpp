// Copyright Lifelike & Believable. All Rights Reserved.

// FO3DSenderPoseSampler (WP-A3 step 6, SND-22): the sender's skeleton descriptor cache, subject
// name and frame shell, reached through FO3DSenderPoseSamplerProbe. The mesh is built in code
// (O3DTestSkeletalMesh) on a component that is never registered: the descriptor and the name need
// only the mesh asset, and an unregistered component has no component-space pose.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "O3DHelpers.h"
#include "O3DSenderComponent.h"
#include "O3DTestSkeletalMesh.h"
#include "Testing/O3DSenderTesting.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPoseSamplerDescriptorTest, "Open3DBroadcast.Sender.PoseSampler.CachesDescriptorPerMesh", O3DB_TEST_FLAGS)
bool FO3DSenderPoseSamplerDescriptorTest::RunTest(const FString& Parameters)
{
	USkeletalMesh* ThreeBones = O3DTestSkeletalMesh::CreateChainMesh(GetTransientPackage(), 3, 0);
	USkeletalMesh* FiveBones = O3DTestSkeletalMesh::CreateChainMesh(GetTransientPackage(), 5, 0);
	USkeletalMeshComponent* Mesh = NewObject<USkeletalMeshComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("Meshes built in code"), ThreeBones) || !TestNotNull(TEXT("And another"), FiveBones))
	{
		return false;
	}
	Mesh->SetSkeletalMeshAsset(ThreeBones);

	FO3DSenderPoseSamplerProbe Sampler;
	TestFalse(TEXT("No mesh component: nothing to build"), Sampler.EnsureSkeleton(nullptr, TEXT("")));

	TestTrue(TEXT("The first mesh builds the descriptor"), Sampler.EnsureSkeleton(Mesh, TEXT("Hero")));
	TestEqual(TEXT("Its bones"), Sampler.GetDescriptor().BoneNames.Num(), 3);
	TestTrue(TEXT("Its parents"), Sampler.GetDescriptor().ParentIndices == TArray<int32>({ INDEX_NONE, 0, 1 }));
	TestEqual(TEXT("Its hash"), Sampler.GetDescriptor().Hash, O3DHelpers::HashNamesAndParents(Sampler.GetDescriptor().BoneNames, Sampler.GetDescriptor().ParentIndices));
	TestTrue(TEXT("A snapshot for the frames"), Sampler.HasDescriptorSnapshot());
	TestTrue(TEXT("Reported once, under the name built for the mesh"), Sampler.DescriptorChanges == TArray<FString>({ TEXT("Hero") }));

	TestFalse(TEXT("Same mesh: not rebuilt"), Sampler.EnsureSkeleton(Mesh, TEXT("Hero")));
	TestEqual(TEXT("Nor reported"), Sampler.DescriptorChanges.Num(), 1);

	Mesh->SetSkeletalMeshAsset(FiveBones);
	TestTrue(TEXT("A new mesh asset rebuilds"), Sampler.EnsureSkeleton(Mesh, TEXT("Hero")));
	TestEqual(TEXT("With its bones"), Sampler.GetDescriptor().BoneNames.Num(), 5);
	TestEqual(TEXT("And reports the change"), Sampler.DescriptorChanges.Num(), 2);

	// SND-1: after a reset the same mesh is rebuilt and reported again.
	Sampler.ResetSkeleton();
	TestFalse(TEXT("Reset drops the descriptor"), Sampler.GetDescriptor().IsValid());
	TestFalse(TEXT("And its snapshot"), Sampler.HasDescriptorSnapshot());
	TestTrue(TEXT("The same mesh rebuilds after a reset"), Sampler.EnsureSkeleton(Mesh, TEXT("Hero")));
	TestEqual(TEXT("And is reported again"), Sampler.DescriptorChanges.Num(), 3);

	// An unregistered component has no pose: no bones are sampled.
	FO3DSPoseFrame Frame;
	Frame.BoneLocalTransforms.SetNum(2);
	Sampler.SampleBones(Mesh, Frame);
	TestEqual(TEXT("No component-space pose, no bones"), Frame.BoneLocalTransforms.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderPoseSamplerNamingTest, "Open3DBroadcast.Sender.PoseSampler.NamesSubjectsAndFillsFrames", O3DB_TEST_FLAGS)
bool FO3DSenderPoseSamplerNamingTest::RunTest(const FString& Parameters)
{
	USkeletalMesh* Asset = O3DTestSkeletalMesh::CreateChainMesh(GetTransientPackage(), 2, 0);
	USkeletalMeshComponent* Mesh = NewObject<USkeletalMeshComponent>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), USkeletalMeshComponent::StaticClass(), TEXT("BodyMesh")));
	if (!TestNotNull(TEXT("A mesh built in code"), Asset))
	{
		return false;
	}
	Mesh->SetSkeletalMeshAsset(Asset);

	FO3DSenderPoseSamplerProbe Sampler;

	// Without an override: World/Actor/Component, with placeholders for what is missing.
	TestEqual(TEXT("No mesh: placeholders"), Sampler.ResolveSubjectName(nullptr, TEXT("")), O3DHelpers::SanitizeSubjectName(TEXT("World/Actor/SkeletalMeshComponent")));
	TestTrue(TEXT("The first name is a change from none"), Sampler.NameChanges.Num() == 1 && Sampler.NameChanges[0].StartsWith(TEXT(" -> ")));
	TestEqual(TEXT("An unowned mesh outside a world"), Sampler.ResolveSubjectName(Mesh, TEXT("")), O3DHelpers::SanitizeSubjectName(TEXT("World/Actor/") + Mesh->GetName()));
	TestEqual(TEXT("A different name is a change"), Sampler.NameChanges.Num(), 2);

	// An override wins, sanitized; the same inputs reuse the cached name.
	const FString Override(TEXT("Hero One"));
	const FString Expected = O3DHelpers::SanitizeSubjectName(Override);
	TestEqual(TEXT("The override, sanitized"), Sampler.ResolveSubjectName(Mesh, Override), Expected);
	TestEqual(TEXT("Cached"), Sampler.ResolveSubjectName(Mesh, Override), Expected);
	TestEqual(TEXT("One change per new name"), Sampler.NameChanges.Num(), 3);

	// Invalidation returns the old name for the caller to forget; the next resolve is a change from none.
	TestEqual(TEXT("Invalidate returns the cached name"), Sampler.InvalidateSubjectName(Override), Expected);
	TestEqual(TEXT("Rebuilt after invalidation"), Sampler.ResolveSubjectName(Mesh, Override), Expected);
	TestTrue(TEXT("Reported from no name"), Sampler.NameChanges.Num() == 4 && Sampler.NameChanges[3] == TEXT(" -> ") + Expected);

	// The frame shell: subject, a frame index counting from 1, the shared descriptor snapshot, the time.
	Sampler.EnsureSkeleton(Mesh, Override);
	FO3DSPoseFrame First;
	FO3DSPoseFrame Second;
	Sampler.FillShell(Mesh, Override, 1.5, First);
	Sampler.FillShell(Mesh, Override, 2.5, Second);
	TestEqual(TEXT("Subject"), First.Subject, Expected);
	TestEqual(TEXT("Frame index starts at 1"), First.FrameIndex, 1ull);
	TestEqual(TEXT("And counts up"), Second.FrameIndex, 2ull);
	TestEqual(TEXT("Sampling time"), Second.CaptureTimeSec, 2.5);
	TestTrue(TEXT("The descriptor snapshot is shared, not copied, per frame"), First.Descriptor.IsValid() && First.Descriptor == Second.Descriptor);
	Sampler.ResetFrameCounter();
	Sampler.FillShell(Mesh, Override, 3.0, First);
	TestEqual(TEXT("ResetFrameCounter starts over"), First.FrameIndex, 1ull);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
