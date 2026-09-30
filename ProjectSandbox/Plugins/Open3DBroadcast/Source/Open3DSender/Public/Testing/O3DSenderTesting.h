// Copyright (c) Open3DStream Contributors

#pragma once

// Test-only white-box access to the sender module (ADR 0006, WP-T2). The Open3DBroadcastTests
// module is the only intended caller. Compiled out without dev automation tests.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DSenderComponent.h"
#include "O3DSenderCurveConfig.h"
#include "Templates/UniquePtr.h"

class FO3DSenderCurveProcessor;

/**
 * Befriended by UO3DSenderComponent. Header-only: the component class is exported, so these
 * inline accessors link from any module that depends on Open3DSender.
 */
struct FO3DSenderComponentTestAccess
{
	static bool ConsumeCaptureBudget(double NowSeconds, double& InOutLastCaptureTime, float CaptureRateHz)
	{
		return UO3DSenderComponent::ConsumeCaptureBudget(NowSeconds, InOutLastCaptureTime, CaptureRateHz);
	}

	static void BuildLocalBoneTransforms(const TArray<FTransform>& ComponentSpaceTransforms,
		const TArray<int32>& CachedParentIndices,
		int32 NumBones,
		TFunctionRef<int32(int32)> ResolveFallbackParent,
		TArray<FTransform>& OutLocalTransforms,
		TArray<int32>* OutResolvedParents)
	{
		UO3DSenderComponent::BuildLocalBoneTransforms(ComponentSpaceTransforms, CachedParentIndices, NumBones, ResolveFallbackParent, OutLocalTransforms, OutResolvedParents);
	}

	static void SetDescriptor(UO3DSenderComponent& Component, const FO3DSSkeletonDescriptor& Descriptor)
	{
		Component.DescriptorCache = Descriptor;
		Component.DescriptorSnapshot = MakeShared<FO3DSSkeletonDescriptor>(Descriptor);
	}

	static const FO3DSSkeletonDescriptor& GetDescriptorCache(const UO3DSenderComponent& Component) { return Component.DescriptorCache; }
	static bool HasDescriptorSnapshot(const UO3DSenderComponent& Component) { return Component.DescriptorSnapshot.IsValid(); }
	static void SetCapturing(UO3DSenderComponent& Component, bool bCapturing) { Component.bIsCapturing = bCapturing; }
	static bool HasSerializer(const UO3DSenderComponent& Component) { return Component.Serializer.IsValid(); }
	static void EnsureSubjectNameCached(UO3DSenderComponent& Component) { Component.EnsureSubjectNameCached(nullptr); }
	static FO3DSPoseFrame CreateFrameShell(UO3DSenderComponent& Component, double CaptureTimeSec) { return Component.CreateFrameShell(nullptr, CaptureTimeSec); }
	static void SetAudioCaptureComponent(UO3DSenderComponent& Component, UO3DSenderAudioCaptureComponent* Capture) { Component.AudioCaptureComponent = Capture; }
	static FString GetCachedSubjectName(const UO3DSenderComponent& Component) { return Component.CachedSubjectName; }
};

/**
 * Owns one FO3DSenderCurveProcessor (a private class of this module) and exposes what the
 * curve filter tests need (SND-4, SND-20). Befriended by the processor.
 */
class OPEN3DSENDER_API FO3DSenderCurveProcessorProbe
{
public:
	FO3DSenderCurveProcessorProbe();
	~FO3DSenderCurveProcessorProbe();

	FO3DSenderCurveProcessorProbe(const FO3DSenderCurveProcessorProbe&) = delete;
	FO3DSenderCurveProcessorProbe& operator=(const FO3DSenderCurveProcessorProbe&) = delete;
	FO3DSenderCurveProcessorProbe(FO3DSenderCurveProcessorProbe&&) = delete;
	FO3DSenderCurveProcessorProbe& operator=(FO3DSenderCurveProcessorProbe&&) = delete;

	/** Replaces the cached curve list as if a mesh with these curves had been captured. */
	void SetCurves(const TArray<FName>& Names, const TArray<float>& Values);
	/** Replaces this frame's values, keeping the cached names. */
	void SetCurveValues(const TArray<float>& Values);
	void BuildFilteredCurves(const FO3DSenderCurveConfig& Config, TArray<FName>& OutNames, TArray<float>& OutValues);

private:
	TUniquePtr<FO3DSenderCurveProcessor> Processor;
};

#endif // WITH_DEV_AUTOMATION_TESTS
