// Copyright (c) Open3DStream Contributors

#include "O3DSenderSerializer.h"

#include "O3DSenderComponent.h"
#include "O3DSenderLogs.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"

#include "o3ds/model.h"

namespace
{
	static TAutoConsoleVariable<int32> CVarO3DSenderDebugSerialize(
		TEXT("o3ds.Sender.DebugSerialize"),
		0,
		TEXT("Enable verbose serialization logging for O3DS sender (0/1)."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarO3DSenderDebugStats(
		TEXT("o3ds.Sender.DebugStats"),
		0,
		TEXT("Enable per-frame serializer stats logging (0/1)."),
		ECVF_Default);
}

TArray<FO3DSenderSerializer*> FO3DSenderSerializer::GInstances;

FO3DSenderSerializer::FO3DSenderSerializer() = default;
FO3DSenderSerializer::~FO3DSenderSerializer() = default;

/** Register for descriptor/frame events emitted by the capture component. */
void FO3DSenderSerializer::Attach(UO3DSenderComponent* InComponent)
{
	if (!InComponent)
	{
		return;
	}
	if (Component == InComponent)
	{
		return;
	}

	Component = InComponent;
	GInstances.AddUnique(this);

	static bool bRegisteredCmd = false;
	if (!bRegisteredCmd)
	{
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("o3ds.Sender.DumpStats"),
			TEXT("Dump per-subject serialization stats to the log"),
			FConsoleCommandDelegate::CreateStatic(&FO3DSenderSerializer::DumpAllStats),
			ECVF_Default);
		bRegisteredCmd = true;
	}

	Component->OnDescriptorReady.AddRaw(this, &FO3DSenderSerializer::OnDescriptorReady);
	Component->OnPoseFrameReady.AddRaw(this, &FO3DSenderSerializer::OnPoseFrameReady);
}

/** Remove previously registered delegates and release the owning component reference. */
void FO3DSenderSerializer::Detach(UO3DSenderComponent* InComponent)
{
	if (Component && Component == InComponent)
	{
		Component->OnDescriptorReady.RemoveAll(this);
		Component->OnPoseFrameReady.RemoveAll(this);
		Component = nullptr;
	}
	GInstances.Remove(this);
	ClearAllCaches();
}

void FO3DSenderSerializer::RemoveSubjectCache(const FString& Subject)
{
	if (Subject.IsEmpty())
	{
		return;
	}

	if (SubjectState.Remove(Subject) > 0)
	{
		UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("Removed serializer cache for subject '%s'"), *Subject);
	}
}

void FO3DSenderSerializer::ClearAllCaches()
{
	if (SubjectState.Num() > 0)
	{
		SubjectState.Empty();
	}
}

/** Refresh the per-subject skeleton cache, invalidating pending descriptor state if the hash changes. */
void FO3DSenderSerializer::BuildOrUpdateCache(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor)
{
	FSubjectCache& Cache = SubjectState.FindOrAdd(Subject);
	const bool bHashChanged = (Cache.SkeletonHash != Descriptor.Hash);
	Cache.SkeletonHash = Descriptor.Hash;
	Cache.ParentIndices = Descriptor.ParentIndices;
	Cache.BoneNames = Descriptor.BoneNames;
	Cache.bDescriptorSent = false;

	if (CVarO3DSenderDebugSerialize.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogO3DSenderSerializer, Log, TEXT("Descriptor %s for Subject=%s Bones=%d Hash=%llu"),
			bHashChanged ? TEXT("rebuilt") : TEXT("built"),
			*Subject,
			Cache.BoneNames.Num(),
			(unsigned long long)Cache.SkeletonHash);
	}
}

void FO3DSenderSerializer::OnDescriptorReady(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor)
{
	BuildOrUpdateCache(Subject, Descriptor);
}

/** Rebuild a name->index lookup for the curve array when curve names change. */
void FO3DSenderSerializer::EnsureCurveIndex(FSubjectCache& Cache)
{
	Cache.CurveIndex.Reset();
	for (int32 i = 0; i < Cache.CurveNames.Num(); ++i)
	{
		Cache.CurveIndex.Add(Cache.CurveNames[i], i);
	}
}

static inline bool HasInvalidTransform(const FTransform& T)
{
	const FVector Tr = T.GetTranslation();
	const FQuat Rot = T.GetRotation();
	const FVector Sc = T.GetScale3D();
	auto IsBad = [](double V) { return !FMath::IsFinite(V); };
	return IsBad(Tr.X) || IsBad(Tr.Y) || IsBad(Tr.Z) ||
		IsBad(Rot.X) || IsBad(Rot.Y) || IsBad(Rot.Z) || IsBad(Rot.W) ||
		IsBad(Sc.X) || IsBad(Sc.Y) || IsBad(Sc.Z);
}

/** Validate and serialise a captured pose frame, emitting FlatBuffer payloads for transports. */
void FO3DSenderSerializer::OnPoseFrameReady(const FString& Subject, const FO3DSPoseFrame& Frame)
{
	if (!Component)
	{
		return;
	}

	FSubjectCache& Cache = SubjectState.FindOrAdd(Subject);
	if (Cache.CurveNames.Num() == 0 && Frame.CurveNames.Num() > 0)
	{
		Cache.CurveNames = Frame.CurveNames;
		EnsureCurveIndex(Cache);
		Cache.bDescriptorSent = false;
	}

	const int32 BoneCount = Frame.BoneLocalTransforms.Num();
	if (BoneCount <= 0)
	{
		Cache.DroppedFrames++;
		Cache.LastError = TEXT("Empty transform array");
		UE_LOG(LogO3DSenderSerializer, Warning, TEXT("Skipping frame: Subject=%s reason=%s"), *Subject, *Cache.LastError);
		return;
	}

	for (int32 Index = 0; Index < BoneCount; ++Index)
	{
		if (HasInvalidTransform(Frame.BoneLocalTransforms[Index]))
		{
			Cache.DroppedFrames++;
			Cache.LastError = FString::Printf(TEXT("NaN/Inf at bone %d"), Index);
			UE_LOG(LogO3DSenderSerializer, Warning, TEXT("Skipping frame: Subject=%s reason=%s"), *Subject, *Cache.LastError);
			return;
		}
	}

	FO3DSSkeletonDescriptor DescriptorSnapshot;
	DescriptorSnapshot.ParentIndices = Cache.ParentIndices;
	DescriptorSnapshot.BoneNames = Cache.BoneNames;
	DescriptorSnapshot.Hash = Cache.SkeletonHash;

	const int32 RequiredCount = Frame.BoneLocalTransforms.Num();
	DescriptorSnapshot.ParentIndices.SetNum(RequiredCount);
	DescriptorSnapshot.BoneNames.SetNum(RequiredCount);

	SerializeFrame(Subject, DescriptorSnapshot, Frame);
}

/** Populate a mutable FlatBuffer Subject from a cached descriptor template. */
void FO3DSenderSerializer::BuildSubjectFromDescriptor(const FString& SubjectName, const FO3DSSkeletonDescriptor& Descriptor, O3DS::Subject& OutSubject)
{
	using namespace O3DS;
	OutSubject.mName = std::string(TCHAR_TO_UTF8(*SubjectName));
	OutSubject.clear();

	const int32 BoneCount = Descriptor.ParentIndices.Num();
	for (int32 Index = 0; Index < BoneCount; ++Index)
	{
		const int32 ParentId = Descriptor.ParentIndices.IsValidIndex(Index) ? Descriptor.ParentIndices[Index] : -1;
		std::string NodeName;
		if (Descriptor.BoneNames.IsValidIndex(Index))
		{
			NodeName = std::string(TCHAR_TO_UTF8(*Descriptor.BoneNames[Index].ToString()));
		}
		auto* Transform = OutSubject.addTransform(NodeName.c_str(), ParentId);
		Transform->translation.value = O3DS::Vector3d(0.0, 0.0, 0.0);
		Transform->rotation.value = O3DS::Vector4d(0.0, 0.0, 0.0, 1.0);
		Transform->scale.value = O3DS::Vector3d(1.0, 1.0, 1.0);
		Transform->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
	}
}

/** Copy current pose data into the FlatBuffer subject structure in-place. */
void FO3DSenderSerializer::FillFrameValues(const FO3DSPoseFrame& Frame, O3DS::Subject& InOutSubject)
{
	using namespace O3DS;
	const int32 BoneCount = Frame.BoneLocalTransforms.Num();
	if ((int32)InOutSubject.size() != BoneCount)
	{
		InOutSubject.clear();
		for (int32 Index = 0; Index < BoneCount; ++Index)
		{
			auto* Transform = InOutSubject.addTransform("", (Index == 0) ? -1 : Index - 1);
			Transform->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
		}
	}

	for (int32 Index = 0; Index < BoneCount; ++Index)
	{
		const FTransform& Rel = Frame.BoneLocalTransforms[Index];
		const FVector Translation = Rel.GetTranslation();
		const FQuat Rotation = Rel.GetRotation();
		const FVector Scale = Rel.GetScale3D();
		auto* Transform = InOutSubject.mTransforms[Index];
		Transform->translation.value = O3DS::Vector3d((double)Translation.X, (double)Translation.Y, (double)Translation.Z);
		Transform->rotation.value = O3DS::Vector4d((double)Rotation.X, (double)Rotation.Y, (double)Rotation.Z, (double)Rotation.W);
		Transform->scale.value = O3DS::Vector3d((double)Scale.X, (double)Scale.Y, (double)Scale.Z);
		if (Transform->transformOrder.empty())
		{
			Transform->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
		}
	}
}

/** Construct the FlatBuffer SubjectList for the supplied frame and broadcast delegate notifications. */
void FO3DSenderSerializer::SerializeFrame(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame)
{
	using namespace O3DS;

	TSharedPtr<SubjectList> SubjectListPtr = MakeShared<SubjectList>();
	O3DS::Subject* SubjectObject = SubjectListPtr->addSubject(std::string(TCHAR_TO_UTF8(*Subject)));

	BuildSubjectFromDescriptor(Subject, Descriptor, *SubjectObject);
	FillFrameValues(Frame, *SubjectObject);

	if (Frame.CurveNames.Num() > 0)
	{
		SubjectObject->mCurveNames.clear();
		SubjectObject->mCurveValues.clear();
		SubjectObject->mCurveNames.reserve(Frame.CurveNames.Num());
		SubjectObject->mCurveValues.reserve(Frame.CurveNames.Num());
		for (int32 Index = 0; Index < Frame.CurveNames.Num(); ++Index)
		{
			SubjectObject->mCurveNames.push_back(std::string(TCHAR_TO_UTF8(*Frame.CurveNames[Index].ToString())));
			SubjectObject->mCurveValues.push_back(Index < Frame.CurveValues.Num() ? Frame.CurveValues[Index] : 0.0f);
		}
	}

	SubjectObject->CalcMatrices();

	std::vector<char> Buffer;
	const double Now = FPlatformTime::Seconds();
	SubjectListPtr->Serialize(Buffer, Now);

	FSubjectCache& Cache = SubjectState.FindOrAdd(Subject);

	if (!Buffer.empty())
	{
		TArray<uint8> Payload;
		Payload.SetNumUninitialized((int32)Buffer.size());
		FMemory::Memcpy(Payload.GetData(), Buffer.data(), Buffer.size());

		// Broadcast SubjectList first to allow transports to use it via Send()
		// Keep SubjectListPtr alive during this broadcast to prevent premature destruction
		if (OnSubjectListReady.IsBound())
		{
			OnSubjectListReady.Broadcast(Subject, SubjectListPtr);
		}

		OnSerializedFrame.Broadcast(Subject, Payload, Now);

		Cache.FramesSerialized++;
		Cache.BytesSerialized += (uint64)Payload.Num();

		if (CVarO3DSenderDebugSerialize.GetValueOnAnyThread() != 0)
		{
			UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("Serialized Subject=%s Bones=%d Curves=%d Bytes=%d"),
				*Subject,
				Frame.BoneLocalTransforms.Num(),
				Frame.CurveValues.Num(),
				Payload.Num());
		}

		if (CVarO3DSenderDebugStats.GetValueOnAnyThread() != 0)
		{
			UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("Stats Subject=%s Frames=%llu Bytes=%llu Dropped=%llu"),
				*Subject,
				(unsigned long long)Cache.FramesSerialized,
				(unsigned long long)Cache.BytesSerialized,
				(unsigned long long)Cache.DroppedFrames);
		}
	}
}

/** Emit per-subject stats for this serializer instance (invoked via console command). */
void FO3DSenderSerializer::DumpStatsInstance() const
{
	for (const TPair<FString, FSubjectCache>& Pair : SubjectState)
	{
		const FString& Subject = Pair.Key;
		const FSubjectCache& Cache = Pair.Value;
		UE_LOG(LogO3DSenderSerializer, Display, TEXT("Subject=%s Frames=%llu Bytes=%llu Dropped=%llu LastError=%s"),
			*Subject,
			(unsigned long long)Cache.FramesSerialized,
			(unsigned long long)Cache.BytesSerialized,
			(unsigned long long)Cache.DroppedFrames,
			Cache.LastError.IsEmpty() ? TEXT("<none>") : *Cache.LastError);
	}
	UE_LOG(LogO3DSenderSerializer, Display, TEXT("Serializer(Component=%s) subjects=%d"), *GetNameSafe(Component), SubjectState.Num());
}

/** Console command handler that walks all live serializer instances and logs aggregate stats. */
void FO3DSenderSerializer::DumpAllStats()
{
	UE_LOG(LogO3DSenderSerializer, Display, TEXT("---- O3DS Sender Serializer Stats ----"));
	if (GInstances.Num() == 0)
	{
		UE_LOG(LogO3DSenderSerializer, Display, TEXT("(no active serializer instances)"));
		return;
	}

	for (const FO3DSenderSerializer* Instance : GInstances)
	{
		if (Instance)
		{
			Instance->DumpStatsInstance();
		}
	}
}
