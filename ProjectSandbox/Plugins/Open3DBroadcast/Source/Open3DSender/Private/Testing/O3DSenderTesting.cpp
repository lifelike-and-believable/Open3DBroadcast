// Copyright (c) Open3DStream Contributors

#include "Testing/O3DSenderTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DSenderCurveProcessor.h"
#include "O3DSenderPipeline.h"
#include "O3DSenderPoseSampler.h"

FO3DSenderCurveProcessorProbe::FO3DSenderCurveProcessorProbe()
	: Filter(MakeUnique<FO3DSenderCurveFilter>())
{
}

FO3DSenderCurveProcessorProbe::~FO3DSenderCurveProcessorProbe() = default;

void FO3DSenderCurveProcessorProbe::SetCurves(const TArray<FName>& Names, const TArray<float>& Values)
{
	// A new list, as a curve cache refresh makes; the filter resets its last-sent state for it.
	// No morph curves, as before (the probe never marked any).
	TSharedRef<FO3DSCurveList> List = MakeShared<FO3DSCurveList>();
	List->Names = Names;
	List->MorphMask.Init(false, Names.Num());
	CurveList = List;
	CurveValues = Values;
}

void FO3DSenderCurveProcessorProbe::SetCurveValues(const TArray<float>& Values)
{
	CurveValues = Values;
}

void FO3DSenderCurveProcessorProbe::BuildFilteredCurves(const FO3DSenderCurveConfig& Config, TArray<FName>& OutNames, TArray<float>& OutValues)
{
	Filter->Apply(Config, CurveList, CurveValues, OutNames, OutValues);
}

void FO3DSenderCurveProcessorProbe::FilterFrame(FO3DSPoseFrame& Frame)
{
	Filter->FilterFrame(Frame);
}

FO3DSenderPipelineProbe::FO3DSenderPipelineProbe()
	: Pipeline(MakeShared<FO3DSenderPipeline>())
{
	WeakPipeline = Pipeline;
}

FO3DSenderPipelineProbe::~FO3DSenderPipelineProbe() = default;

void FO3DSenderPipelineProbe::Start(bool bAsync)
{
	if (Pipeline.IsValid())
	{
		Pipeline->Start(bAsync);
	}
}

void FO3DSenderPipelineProbe::Stop()
{
	if (Pipeline.IsValid())
	{
		Pipeline->Stop();
	}
}

void FO3DSenderPipelineProbe::RemoveSubject(const FString& Subject)
{
	if (Pipeline.IsValid())
	{
		Pipeline->RemoveSubject(Subject);
	}
}

void FO3DSenderPipelineProbe::SetDepth(int32 Depth)
{
	if (Pipeline.IsValid())
	{
		Pipeline->SetDepthOverride(Depth);
	}
}

int32 FO3DSenderPipelineProbe::GetDepth() const
{
	return Pipeline.IsValid() ? Pipeline->GetDepth() : 0;
}

TUniquePtr<FO3DSPoseFrame> FO3DSenderPipelineProbe::AcquireFrame()
{
	return Pipeline.IsValid() ? Pipeline->AcquireFrame() : TUniquePtr<FO3DSPoseFrame>();
}

void FO3DSenderPipelineProbe::SubmitFrame(TUniquePtr<FO3DSPoseFrame>&& Frame)
{
	if (!Pipeline.IsValid() || !Frame.IsValid())
	{
		return;
	}
	if (Pipeline->IsAsync())
	{
		Pipeline->SubmitFrame(MoveTemp(Frame), false);
		return;
	}
	Pipeline->FilterFrameInline(*Frame);
	Pipeline->SubmitFrame(MoveTemp(Frame), true);
}

void FO3DSenderPipelineProbe::AttachSender(const TSharedPtr<IOpen3DSender>& Sender)
{
	if (Pipeline.IsValid())
	{
		Pipeline->AttachSender(Sender);
	}
}

void FO3DSenderPipelineProbe::DetachSender()
{
	if (Pipeline.IsValid())
	{
		Pipeline->DetachSender();
	}
}

void FO3DSenderPipelineProbe::SetSerializedFrameListener(FOnO3DSerializedFrame* Listener)
{
	if (Pipeline.IsValid())
	{
		Pipeline->SetSerializedFrameListener(Listener);
	}
}

FO3DSenderSerializer& FO3DSenderPipelineProbe::GetSerializer() const
{
	check(Pipeline.IsValid());
	return Pipeline->GetSerializer();
}

FO3DSenderPipelineStats FO3DSenderPipelineProbe::GetStats() const
{
	return Pipeline.IsValid() ? Pipeline->GetStats() : FO3DSenderPipelineStats();
}

bool FO3DSenderPipelineProbe::IsIdle() const
{
	return !Pipeline.IsValid() || Pipeline->IsIdle();
}

bool FO3DSenderPipelineProbe::WaitForIdle(double TimeoutSeconds) const
{
	return !Pipeline.IsValid() || Pipeline->WaitForIdle(TimeoutSeconds);
}

void FO3DSenderPipelineProbe::Release()
{
	Pipeline.Reset();
}

bool FO3DSenderPipelineProbe::IsPipelineAlive() const
{
	return WeakPipeline.IsValid();
}

bool FO3DSenderPipelineProbe::WaitForAllIdle(double TimeoutSeconds)
{
	return FO3DSenderPipeline::WaitForAllIdle(TimeoutSeconds);
}

int32 FO3DSenderPipelineProbe::GetNumActiveDrainTasks()
{
	return FO3DSenderPipeline::GetNumActiveDrainTasks();
}

int32 FO3DSenderPipelineProbe::GetMaxDepth()
{
	return FO3DSenderPipeline::MaxDepth;
}

void FO3DSenderComponentTestAccess::BuildLocalBoneTransforms(const TArray<FTransform>& ComponentSpaceTransforms,
	const TArray<int32>& CachedParentIndices,
	int32 NumBones,
	TFunctionRef<int32(int32)> ResolveFallbackParent,
	TArray<FTransform>& OutLocalTransforms,
	TArray<int32>* OutResolvedParents)
{
	FO3DSenderPoseSampler::BuildLocalBoneTransforms(ComponentSpaceTransforms, CachedParentIndices, NumBones, ResolveFallbackParent, OutLocalTransforms, OutResolvedParents);
}

void FO3DSenderComponentTestAccess::SetDescriptor(UO3DSenderComponent& Component, const FO3DSSkeletonDescriptor& Descriptor)
{
	Component.PoseSampler->SetDescriptor(Descriptor);
}

const FO3DSSkeletonDescriptor& FO3DSenderComponentTestAccess::GetDescriptorCache(const UO3DSenderComponent& Component)
{
	return Component.PoseSampler->GetDescriptor();
}

bool FO3DSenderComponentTestAccess::HasDescriptorSnapshot(const UO3DSenderComponent& Component)
{
	return Component.PoseSampler->HasDescriptorSnapshot();
}

FString FO3DSenderComponentTestAccess::GetCachedSubjectName(const UO3DSenderComponent& Component)
{
	return Component.PoseSampler->GetSubjectName();
}

FO3DSenderPoseSamplerProbe::FO3DSenderPoseSamplerProbe()
	: Sampler(MakeUnique<FO3DSenderPoseSampler>())
{
	Sampler->SetCallbacks(
		[this](const FString& Subject, const FO3DSSkeletonDescriptor& /*Descriptor*/) { DescriptorChanges.Add(Subject); },
		[this](const FString& PreviousName, const FString& NewName) { NameChanges.Add(PreviousName + TEXT(" -> ") + NewName); });
}

FO3DSenderPoseSamplerProbe::~FO3DSenderPoseSamplerProbe() = default;

bool FO3DSenderPoseSamplerProbe::EnsureSkeleton(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride)
{
	return Sampler->EnsureSkeleton(Mesh, SubjectOverride, false);
}

void FO3DSenderPoseSamplerProbe::ResetSkeleton()
{
	Sampler->ResetSkeleton();
}

FString FO3DSenderPoseSamplerProbe::ResolveSubjectName(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride)
{
	return Sampler->ResolveSubjectName(Mesh, SubjectOverride);
}

FString FO3DSenderPoseSamplerProbe::InvalidateSubjectName(const FString& SubjectOverride)
{
	return Sampler->InvalidateSubjectName(SubjectOverride);
}

void FO3DSenderPoseSamplerProbe::ResetFrameCounter()
{
	Sampler->ResetFrameCounter();
}

void FO3DSenderPoseSamplerProbe::FillShell(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride, double CaptureTimeSec, FO3DSPoseFrame& Frame)
{
	Sampler->FillShell(Mesh, SubjectOverride, CaptureTimeSec, FO3DSenderEncodingSettings(), Frame);
}

void FO3DSenderPoseSamplerProbe::SampleBones(const USkeletalMeshComponent* Mesh, FO3DSPoseFrame& Frame)
{
	Sampler->SampleBones(Mesh, Frame, false);
}

const FO3DSSkeletonDescriptor& FO3DSenderPoseSamplerProbe::GetDescriptor() const
{
	return Sampler->GetDescriptor();
}

bool FO3DSenderPoseSamplerProbe::HasDescriptorSnapshot() const
{
	return Sampler->HasDescriptorSnapshot();
}

#endif // WITH_DEV_AUTOMATION_TESTS
