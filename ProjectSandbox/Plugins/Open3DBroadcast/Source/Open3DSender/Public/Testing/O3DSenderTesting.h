// Copyright (c) Open3DStream Contributors

#pragma once

// Test-only white-box access to the sender module (ADR 0006, WP-T2). The Open3DBroadcastTests
// module is the only intended caller. Compiled out without dev automation tests.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DSenderAudioCaptureComponent.h"
#include "O3DSenderComponent.h"
#include "O3DSenderCurveConfig.h"
#include "O3DSenderPipelineStats.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "Transport/O3DSenderInterface.h"
#include "UObject/UnrealType.h"

class FO3DSenderCurveFilter;
class FO3DSenderPipeline;

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

	// The pose sampler is a private class (WP-A3 step 6): these are defined in the module.
	static OPEN3DSENDER_API void BuildLocalBoneTransforms(const TArray<FTransform>& ComponentSpaceTransforms,
		const TArray<int32>& CachedParentIndices,
		int32 NumBones,
		TFunctionRef<int32(int32)> ResolveFallbackParent,
		TArray<FTransform>& OutLocalTransforms,
		TArray<int32>* OutResolvedParents);
	static OPEN3DSENDER_API void SetDescriptor(UO3DSenderComponent& Component, const FO3DSSkeletonDescriptor& Descriptor);
	static OPEN3DSENDER_API const FO3DSSkeletonDescriptor& GetDescriptorCache(const UO3DSenderComponent& Component);
	static OPEN3DSENDER_API bool HasDescriptorSnapshot(const UO3DSenderComponent& Component);
	static void SetCapturing(UO3DSenderComponent& Component, bool bCapturing) { Component.bIsCapturing = bCapturing; }
	/** The serializer lives in the pose pipeline since WP-A2c; created by the first successful StartCapture. */
	static bool HasSerializer(const UO3DSenderComponent& Component) { return Component.Pipeline.IsValid(); }
	static void EnsureSubjectNameCached(UO3DSenderComponent& Component) { Component.EnsureSubjectNameCached(nullptr); }
	static FO3DSPoseFrame CreateFrameShell(UO3DSenderComponent& Component, double CaptureTimeSec)
	{
		FO3DSPoseFrame Frame;
		Component.FillFrameShell(nullptr, CaptureTimeSec, Frame);
		return Frame;
	}
	static void SetAudioCaptureComponent(UO3DSenderComponent& Component, UO3DSenderAudioCaptureComponent* Capture) { Component.AudioCaptureComponent = Capture; }
	/** The capture config StartCapture hands the audio capture component (device index resolved from the cache). */
	static FO3DSenderAudioCaptureConfig BuildAudioCaptureConfig(const UO3DSenderComponent& Component) { return Component.BuildAudioCaptureConfig(); }
	static OPEN3DSENDER_API FString GetCachedSubjectName(const UO3DSenderComponent& Component);

	// Tick order (WP-A2b).
	static void UnbindFromTarget(UO3DSenderComponent& Component) { Component.UnbindFromTarget(); }
	/** The per-tick capture check, which also moves the tick prerequisite to the current TargetMesh. */
	static bool CanCaptureThisFrame(UO3DSenderComponent& Component, double NowSeconds)
	{
		USkeletalMeshComponent* Mesh = nullptr;
		return Component.CanCaptureThisFrame(NowSeconds, Mesh);
	}

	// Pose pipeline (WP-A2c).
	/**
	 * What a sampled tick does after sampling, without a mesh: a pooled frame with this component's
	 * subject, frame index, descriptor snapshot (SetDescriptor), settings snapshot and Bones, handed
	 * to the pipeline. False when there is no pipeline or no free frame.
	 */
	static bool SubmitSampledFrame(UO3DSenderComponent& Component, const TArray<FTransform>& Bones, double CaptureTimeSec)
	{
		TUniquePtr<FO3DSPoseFrame> Frame = Component.AcquirePoseFrame();
		if (!Frame.IsValid())
		{
			return false;
		}
		Component.FillFrameShell(nullptr, CaptureTimeSec, *Frame);
		Frame->BoneLocalTransforms = Bones;
		Component.DispatchSampledFrame(MoveTemp(Frame));
		return true;
	}
	/** Waits (event with a timeout, never a bare sleep) until the pipeline's worker has nothing left. */
	static bool WaitForPipelineIdle(const UO3DSenderComponent& Component, double TimeoutSeconds) { return Component.WaitForPipelineIdle(TimeoutSeconds); }

	// Typed config and transport switching (WP-A1 PR 5a).
	static FO3DTransportConfig BuildTransportConfig(const UO3DSenderComponent& Component) { return Component.BuildTransportConfig(); }
#if WITH_EDITOR
	/** What a Details-panel edit of TransportName does: PreEditChange, the change, PostEditChangeProperty. */
	static void EditTransportName(UO3DSenderComponent& Component, FName NewName)
	{
		FProperty* Property = UO3DSenderComponent::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName));
		Component.PreEditChange(Property);
		Component.TransportName = NewName;
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Component.PostEditChangeProperty(Event);
	}
#endif
};

/** Befriended by UO3DSenderAudioCaptureComponent (WP-A2d). Header-only, like the accessor above. */
struct FO3DSenderAudioCaptureTestAccess
{
	/** Times the component tried to open its capture device (ADR 0008 item 8: once per start). */
	static int32 GetNumMicOpenAttempts(const UO3DSenderAudioCaptureComponent& Component) { return Component.NumMicOpenAttempts; }
};

/**
 * Owns one FO3DSenderCurveFilter (a private class of this module) and exposes what the curve filter
 * tests need (SND-4, SND-20). Since WP-A2a the filter works on the sampled frame: SetCurves builds
 * the shared curve list a sampled frame carries, and BuildFilteredCurves filters it.
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

	/** Filters a sampled frame in place, as the component does after sampling (WP-A2a). Same state as BuildFilteredCurves. */
	void FilterFrame(FO3DSPoseFrame& Frame);

private:
	TUniquePtr<FO3DSenderCurveFilter> Filter;
	TSharedPtr<const FO3DSCurveList> CurveList;
	TArray<float> CurveValues;
};

/**
 * Owns one FO3DSenderPipeline (a private class of this module) for the pipeline tests (WP-A2c,
 * ADR 0008 Verification). Same calls as the component makes; SubmitFrame filters on the calling
 * thread first in synchronous mode, as the component does.
 */
class OPEN3DSENDER_API FO3DSenderPipelineProbe
{
public:
	FO3DSenderPipelineProbe();
	~FO3DSenderPipelineProbe();

	FO3DSenderPipelineProbe(const FO3DSenderPipelineProbe&) = delete;
	FO3DSenderPipelineProbe& operator=(const FO3DSenderPipelineProbe&) = delete;
	FO3DSenderPipelineProbe(FO3DSenderPipelineProbe&&) = delete;
	FO3DSenderPipelineProbe& operator=(FO3DSenderPipelineProbe&&) = delete;

	void Start(bool bAsync);
	void Stop();
	void RemoveSubject(const FString& Subject);
	/** A fixed queue depth instead of o3d.Sender.PipelineDepth (0 restores the console variable). */
	void SetDepth(int32 Depth);
	int32 GetDepth() const;

	TUniquePtr<FO3DSPoseFrame> AcquireFrame();
	void SubmitFrame(TUniquePtr<FO3DSPoseFrame>&& Frame);

	void AttachSender(const TSharedPtr<IOpen3DSender>& Sender);
	/** Waits for a frame being processed, as the transport controller's Stop does. */
	void DetachSender();
	/** The component's OnSerializedFrame role: broadcast after each serialized frame. Null removes it (waits). */
	void SetSerializedFrameListener(FOnO3DSerializedFrame* Listener);

	/** The worker owns it: call anything but the stats getters only after WaitForIdle. */
	FO3DSenderSerializer& GetSerializer() const;
	FO3DSenderPipelineStats GetStats() const;
	bool IsIdle() const;
	bool WaitForIdle(double TimeoutSeconds) const;

	/**
	 * Drops the probe's reference without detaching anything, as an owner destroyed with a task
	 * in flight does. The pipeline lives on while a task holds it. Every other call is a no-op
	 * afterwards.
	 */
	void Release();
	/** True while the pipeline object exists (the probe or a task still holds it). */
	bool IsPipelineAlive() const;

	/** No drain task of any pipeline scheduled or running. */
	static bool WaitForAllIdle(double TimeoutSeconds);
	static int32 GetNumActiveDrainTasks();
	static int32 GetMaxDepth();

private:
	TSharedPtr<FO3DSenderPipeline> Pipeline;
	TWeakPtr<FO3DSenderPipeline> WeakPipeline;
};

class FO3DSenderPoseSampler;

/** Owns one FO3DSenderPoseSampler (a private class of this module, WP-A3 step 6) and records its callbacks. */
class OPEN3DSENDER_API FO3DSenderPoseSamplerProbe
{
public:
	FO3DSenderPoseSamplerProbe();
	~FO3DSenderPoseSamplerProbe();

	FO3DSenderPoseSamplerProbe(const FO3DSenderPoseSamplerProbe&) = delete;
	FO3DSenderPoseSamplerProbe& operator=(const FO3DSenderPoseSamplerProbe&) = delete;

	bool EnsureSkeleton(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride);
	void ResetSkeleton();
	FString ResolveSubjectName(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride);
	FString InvalidateSubjectName(const FString& SubjectOverride);
	void ResetFrameCounter();
	void FillShell(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride, double CaptureTimeSec, FO3DSPoseFrame& Frame);
	void SampleBones(const USkeletalMeshComponent* Mesh, FO3DSPoseFrame& Frame);
	const FO3DSSkeletonDescriptor& GetDescriptor() const;
	bool HasDescriptorSnapshot() const;

	/** OnDescriptorChanged calls: the subject name each was given. */
	TArray<FString> DescriptorChanges;
	/** OnSubjectNameChanged calls: "Previous -> New". */
	TArray<FString> NameChanges;

private:
	TUniquePtr<FO3DSenderPoseSampler> Sampler;
};

class FO3DSenderAudioBinding;

/** Owns one FO3DSenderAudioBinding (a private class of this module, WP-A3 step 7). */
class OPEN3DSENDER_API FO3DSenderAudioBindingProbe
{
public:
	FO3DSenderAudioBindingProbe();
	~FO3DSenderAudioBindingProbe();

	FO3DSenderAudioBindingProbe(const FO3DSenderAudioBindingProbe&) = delete;
	FO3DSenderAudioBindingProbe& operator=(const FO3DSenderAudioBindingProbe&) = delete;

	// The audio properties the binding reads (FO3DSenderAudioSettings).
	bool bEnableAudio = true;
	EO3DSenderCaptureMode Mode = EO3DSenderCaptureMode::Mix;
	FName InputDevice;
	FName Codec;
	FO3DSenderAudioCaptureConfig CaptureConfig;

	FO3DSenderAudioCaptureConfig BuildCaptureConfig() const;
	/** Built from BuildCaptureConfig(), as the component builds the transport config. */
	FO3DTransportAudioConfig BuildTransportConfig() const;
	/** SyncSource applied to CaptureConfig. */
	void SyncSource();
	static UO3DSenderAudioCaptureComponent* FindOrCreateCaptureComponent(AActor* Owner, UO3DSenderAudioCaptureComponent* Current);
	void AttachSink(UO3DSenderAudioCaptureComponent& Capture, const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& Sink, const FString& Label, double NowSeconds);
	void Detach(UO3DSenderAudioCaptureComponent* Capture);
	double GetLastSinkWarningTime() const;

private:
	TUniquePtr<FO3DSenderAudioBinding> Binding;
};

#endif // WITH_DEV_AUTOMATION_TESTS
