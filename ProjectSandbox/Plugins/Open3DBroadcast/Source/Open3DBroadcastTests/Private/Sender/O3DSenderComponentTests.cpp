// Copyright (c) Open3DStream Contributors
//
// Sender component helpers (SND-5 rate limiter, bone-space conversion). White-box access through
// Open3DSender/Public/Testing/O3DSenderTesting.h (WP-T2).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Testing/O3DSenderTesting.h"

#include "Misc/AutomationTest.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderConsumeCaptureBudgetTest, "Open3DBroadcast.Sender.Component.ConsumeCaptureBudget", O3DB_TEST_FLAGS)
bool FO3DSenderConsumeCaptureBudgetTest::RunTest(const FString& Parameters)
{
	double LastCaptureTime = 0.0;
	TestTrue(TEXT("Initial capture is permitted"), FO3DSenderComponentTestAccess::ConsumeCaptureBudget(10.0, LastCaptureTime, 60.0f));
	const double FirstStamp = LastCaptureTime;
	TestEqual(TEXT("Timestamp updated on first capture"), LastCaptureTime, 10.0);
	TestFalse(TEXT("Subsequent capture within rate budget is blocked"), FO3DSenderComponentTestAccess::ConsumeCaptureBudget(FirstStamp + 0.001, LastCaptureTime, 60.0f));
	TestTrue(TEXT("Capture permitted after interval"), FO3DSenderComponentTestAccess::ConsumeCaptureBudget(FirstStamp + (1.0 / 60.0) + 0.001, LastCaptureTime, 60.0f));

	double NoRateLastTime = -1.0;
	TestTrue(TEXT("Disabled rate limiter always allows capture"), FO3DSenderComponentTestAccess::ConsumeCaptureBudget(5.0, NoRateLastTime, 0.0f));
	const double ZeroRateStamp = NoRateLastTime;
	TestTrue(TEXT("Disabled limiter continues to allow immediate captures"), FO3DSenderComponentTestAccess::ConsumeCaptureBudget(ZeroRateStamp + 0.0001, NoRateLastTime, 0.0f));
	TestTrue(TEXT("Negative rate behaves as disabled"), FO3DSenderComponentTestAccess::ConsumeCaptureBudget(NoRateLastTime + 0.0001, NoRateLastTime, -5.0f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderBuildLocalBoneTransformsTest, "Open3DBroadcast.Sender.Component.BuildLocalBoneTransforms", O3DB_TEST_FLAGS)
bool FO3DSenderBuildLocalBoneTransformsTest::RunTest(const FString& Parameters)
{
	TArray<FTransform> ComponentSpace;
	ComponentSpace.Add(FTransform(FQuat::Identity, FVector::ZeroVector, FVector::OneVector));

	FQuat UnnormalisedQuat(0.0f, 0.0f, 0.7071067f * 2.0f, 0.7071067f * 2.0f);
	ComponentSpace.Add(FTransform(UnnormalisedQuat, FVector(10.0f, 0.0f, 0.0f), FVector::OneVector));
	ComponentSpace.Add(FTransform(FQuat::Identity, FVector(20.0f, 5.0f, 0.0f), FVector(2.0f, 2.0f, 2.0f)));

	TArray<int32> CachedParents;
	CachedParents.Add(INDEX_NONE);
	CachedParents.Add(0);
	CachedParents.Add(1);

	TArray<FTransform> LocalTransforms;
	TArray<int32> ResolvedParents;

	auto NoFallback = [](int32)
	{
		return INDEX_NONE;
	};

	FO3DSenderComponentTestAccess::BuildLocalBoneTransforms(ComponentSpace, CachedParents, ComponentSpace.Num(), NoFallback, LocalTransforms, &ResolvedParents);
	TestEqual(TEXT("Local transform count matches"), LocalTransforms.Num(), 3);
	TestEqual(TEXT("Resolved parent count matches"), ResolvedParents.Num(), 3);
	TestEqual(TEXT("Root parent remains none"), ResolvedParents[0], INDEX_NONE);
	TestEqual(TEXT("First child parent is root"), ResolvedParents[1], 0);
	TestTrue(TEXT("Rotation was normalised"), LocalTransforms[1].GetRotation().IsNormalized());
	TestTrue(TEXT("Child translation derived from parent"), LocalTransforms[1].GetTranslation().Equals(FVector(10.0f, 0.0f, 0.0f), 0.001f));

	// Ensure fallback is consulted when cached parent is invalid.
	TArray<int32> CachedParentsWithGap = { INDEX_NONE, 99 };
	TArray<FTransform> LocalWithFallback;
	TArray<int32> ParentsWithFallback;
	auto ProvideFallback = [](int32 BoneIndex)
	{
		return (BoneIndex == 1) ? 0 : INDEX_NONE;
	};

	FO3DSenderComponentTestAccess::BuildLocalBoneTransforms(ComponentSpace, CachedParentsWithGap, 2, ProvideFallback, LocalWithFallback, &ParentsWithFallback);
	TestEqual(TEXT("Fallback resolved parent"), ParentsWithFallback[1], 0);
	TestTrue(TEXT("Fallback relative transform computed"), LocalWithFallback[1].GetTranslation().Equals(FVector(10.0f, 0.0f, 0.0f), 0.001f));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
