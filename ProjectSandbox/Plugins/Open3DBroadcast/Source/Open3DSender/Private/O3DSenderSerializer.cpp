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

	// Residual coding (C2, roadmap doc §5) and quantization (D1, roadmap
	// doc §6) configuration used to live here as cvars. Both are
	// production tuning knobs a project would set per-instance, not
	// debug/iteration toggles (unlike DebugSerialize/DebugStats above), so
	// they're now real UPROPERTY fields on UO3DSenderComponent - see
	// bEnableResidualCoding/ResidualPredictor/ResidualKeyframeIntervalFrames/
	// ResidualDeltaThreshold and bEnableQuantization/QuantizationByteRange/
	// QuantizationHalfRange/QuantizationDeltaThreshold there, editable from
	// the component's Details panel instead of requiring a console command.
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

	// C2/D1: drop the persistent residual- or quantization-mode Subject too
	// (PersistentSubjects is shared by both - see its own declaration), so
	// if this name reappears later it gets a fresh full sync + encoder/
	// anchor (bDescriptorSent is gone along with the SubjectState entry
	// above, which already forces that - this just avoids leaking the now-
	// orphaned O3DS::Subject object, which PersistentSubjects owns via a
	// raw pointer it only frees in its own destructor or a bare vector
	// erase, same pattern as the SubjectList::Parse clearInactive leak
	// fixed earlier this session).
	if (PersistentSubjects.IsValid())
	{
		const std::string SubjectNameUtf8 = std::string(TCHAR_TO_UTF8(*Subject));
		auto& Items = PersistentSubjects->mItems;
		for (size_t Index = 0; Index < Items.size(); ++Index)
		{
			if (Items[Index] && Items[Index]->mName == SubjectNameUtf8)
			{
				delete Items[Index];
				Items.erase(Items.begin() + Index);
				break;
			}
		}
	}
}

void FO3DSenderSerializer::ClearAllCaches()
{
	if (SubjectState.Num() > 0)
	{
		SubjectState.Empty();
	}

	// Dropping the whole SubjectList (not just clearing SubjectState above)
	// is safe here: ~SubjectList() deletes every owned O3DS::Subject* for
	// us, unlike the manual per-entry removal RemoveSubjectCache() needs.
	PersistentSubjects.Reset();
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

/** Dispatch point: legacy full-snapshot transmission (default, unchanged
 *  behavior) or C2 delta/residual transmission (o3ds.Sender.Residual.Enabled),
 *  decided once here rather than per-transport - see O3DSenderInterface.h's
 *  SendSerialized() doc comment for why that matters. */
void FO3DSenderSerializer::SerializeFrame(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame)
{
	FSubjectCache& Cache = SubjectState.FindOrAdd(Subject);

	// D1 doesn't compose with C2 yet (see bEnableQuantization's own doc
	// comment on UO3DSenderComponent) - Residual takes precedence if both
	// are enabled, rather than silently ignoring whichever the caller
	// thought was in effect.
	if (Component != nullptr && Component->bEnableResidualCoding)
	{
		SerializeFrameResidual(Subject, Descriptor, Frame, Cache);
	}
	else if (Component != nullptr && Component->bEnableQuantization)
	{
		SerializeFrameQuantized(Subject, Descriptor, Frame, Cache);
	}
	else
	{
		SerializeFrameLegacy(Subject, Descriptor, Frame, Cache);
	}
}

/** Today's behavior, unmodified: a fresh SubjectList/Subject every frame, a full topology+value snapshot. */
void FO3DSenderSerializer::SerializeFrameLegacy(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame, FSubjectCache& Cache)
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

	// Deliberately NOT broadcasting OnSubjectListReady here (unlike the
	// pre-C2 version of this function): UO3DSenderComponent is still bound
	// to it and its handler still calls IOpen3DSender::Send(SubjectList&)
	// - broadcasting both this AND OnSerializedFrame below would send every
	// legacy-mode frame TWICE (once via Send(), once via SendSerialized()).
	// OnSubjectListReady/Send() are left in the codebase for any OTHER
	// caller that wants direct SubjectList access, but the normal frame
	// pipeline now reaches transports exclusively through the bytes below
	// (see UO3DSenderComponent::HandleSerializedFrameForward's own comment
	// on why a transport handed a live object would otherwise call its own
	// Serialize() and silently discard whichever encoding was chosen here).
	BroadcastSerializedBuffer(Subject, Buffer, Now, Cache);

	if (CVarO3DSenderDebugSerialize.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("Serialized Subject=%s Bones=%d Curves=%d Bytes=%d"),
			*Subject,
			Frame.BoneLocalTransforms.Num(),
			Frame.CurveValues.Num(),
			(int32)Buffer.size());
	}
}

/** C2 (roadmap doc §5/C2): persistent-Subject delta/residual transmission. */
void FO3DSenderSerializer::SerializeFrameResidual(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame, FSubjectCache& Cache)
{
	using namespace O3DS;

	if (!PersistentSubjects.IsValid())
	{
		PersistentSubjects = MakeShared<SubjectList>();
	}

	const std::string SubjectNameUtf8 = std::string(TCHAR_TO_UTF8(*Subject));
	O3DS::Subject* SubjectObject = PersistentSubjects->findSubject(SubjectNameUtf8);

	// A fresh full sync is needed on the very first frame for this subject,
	// whenever its descriptor changed (BuildOrUpdateCache clears
	// bDescriptorSent on a skeleton hash change), or whenever its curve
	// COUNT changed - a residual encoder indexed by the OLD topology could
	// otherwise silently misalign channels under a new one. Curve set
	// changes don't affect the skeleton hash, so bDescriptorSent alone
	// can't catch them: without this check, curves added after the first
	// sync would be silently dropped forever (Min()-clamped away in the
	// steady-state branch below), and curves removed would keep being
	// residual-coded against stale reference values - neither of which
	// self-corrects via the periodic keyframe, since a keyframe just
	// re-emits whatever mCurveValues already holds.
	const bool bCurveCountChanged = (SubjectObject != nullptr)
		&& ((size_t)Frame.CurveValues.Num() != SubjectObject->mCurveValues.size());
	const bool bNeedFullSync = (SubjectObject == nullptr) || !Cache.bDescriptorSent || bCurveCountChanged;

	const double Now = FPlatformTime::Seconds();
	std::vector<char> Buffer;

	if (bNeedFullSync)
	{
		if (!SubjectObject)
		{
			SubjectObject = PersistentSubjects->addSubject(SubjectNameUtf8);
		}

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

		// Fresh encoder: this is either the first frame ever for this
		// subject, or its topology/descriptor just changed - either way
		// the predictor's history must start clean (a stale one indexed
		// by the OLD topology could silently misalign channels - the same
		// hazard ResidualEncoder::BeginFrame's own topology-change
		// detection guards against for in-stream changes; this covers the
		// "detected ahead of time via the descriptor pipeline" case).
		// std::make_unique, NOT UE's MakeUnique: ResidualEncoder's owner
		// (Subject::SetResidualEncoder) takes a standard std::unique_ptr,
		// not UE's TUniquePtr - see the C1 UE glue fix earlier this
		// session for the exact same mismatch on the receiver side.
		ResidualPredictorId PredictorId = ResidualPredictorId::Linear;
		if (Component != nullptr)
		{
			switch (Component->ResidualPredictor)
			{
			case EO3DSenderResidualPredictor::Hold: PredictorId = ResidualPredictorId::Hold; break;
			case EO3DSenderResidualPredictor::Quadratic: PredictorId = ResidualPredictorId::Quadratic; break;
			case EO3DSenderResidualPredictor::Linear:
			default: PredictorId = ResidualPredictorId::Linear; break;
			}
		}
		const uint32 KeyframeInterval = (Component != nullptr) ? (uint32)FMath::Max(0, Component->ResidualKeyframeIntervalFrames) : 300u;
		SubjectObject->SetResidualEncoder(std::make_unique<ResidualEncoder>(PredictorId, KeyframeInterval));

		SubjectObject->Serialize(Buffer, Now);
		Cache.bDescriptorSent = true;
	}
	else
	{
		FillFrameValues(Frame, *SubjectObject);

		// Curve VALUES only - curve identity (mCurveNames) is treated as
		// stable for a subject's lifetime once first assigned, same
		// simplification OnPoseFrameReady's own CurveNames.Num()==0 guard
		// already makes for the legacy path.
		const int32 CurveCount = FMath::Min(Frame.CurveValues.Num(), (int32)SubjectObject->mCurveValues.size());
		for (int32 Index = 0; Index < CurveCount; ++Index)
		{
			SubjectObject->mCurveValues[Index] = Frame.CurveValues[Index];
		}

		SubjectObject->CalcMatrices();

		size_t Count = 0;
		const double DeltaThreshold = (Component != nullptr) ? (double)FMath::Max(0.0f, Component->ResidualDeltaThreshold) : 0.0001;
		SubjectObject->SerializeUpdateResidual(Buffer, Count, DeltaThreshold, Now);
	}

	BroadcastSerializedBuffer(Subject, Buffer, Now, Cache);

	if (CVarO3DSenderDebugSerialize.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("SerializedResidual Subject=%s Bones=%d Curves=%d Bytes=%d FullSync=%s"),
			*Subject,
			Frame.BoneLocalTransforms.Num(),
			Frame.CurveValues.Num(),
			(int32)Buffer.size(),
			bNeedFullSync ? TEXT("true") : TEXT("false"));
	}
}

/** D1 (roadmap doc §6/D1): persistent-Subject adaptive channel quantization on
 *  the legacy delta-threshold path. Structurally mirrors SerializeFrameResidual
 *  (a persistent Subject is required either way, since quantization needs its
 *  own rest-pose anchor to survive across frames - see Transform::
 *  mQuantAnchorTranslation's doc comment in model.h), but without any
 *  predictor/encoder state: D1 only quantizes translation/rotation (curves
 *  are unaffected either way, still sent via the same unquantized path), so
 *  unlike Residual there's no curve-count-driven resync condition to add on
 *  top of the descriptor-hash one. */
void FO3DSenderSerializer::SerializeFrameQuantized(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame, FSubjectCache& Cache)
{
	using namespace O3DS;

	if (!PersistentSubjects.IsValid())
	{
		PersistentSubjects = MakeShared<SubjectList>();
	}

	const std::string SubjectNameUtf8 = std::string(TCHAR_TO_UTF8(*Subject));
	O3DS::Subject* SubjectObject = PersistentSubjects->findSubject(SubjectNameUtf8);

	const bool bNeedFullSync = (SubjectObject == nullptr) || !Cache.bDescriptorSent;

	const double Now = FPlatformTime::Seconds();
	std::vector<char> Buffer;

	if (bNeedFullSync)
	{
		if (!SubjectObject)
		{
			SubjectObject = PersistentSubjects->addSubject(SubjectNameUtf8);
		}

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

		// Subject::Serialize() itself captures each Transform's rest-pose
		// quantization anchor - but only once, ever, per Transform object
		// (see model.h/model.cpp) - so a genuinely fresh Transform (first
		// frame, or just rebuilt by BuildSubjectFromDescriptor above after a
		// topology change) gets a fresh anchor here, while a full resync of
		// UNCHANGED topology (e.g. a periodic keyframe-equivalent full sync
		// some future caller might add) would correctly preserve the
		// existing one instead of silently moving it. Nothing to do here
		// beyond calling Serialize() - no encoder/predictor to construct,
		// unlike Residual.
		SubjectObject->Serialize(Buffer, Now);
		Cache.bDescriptorSent = true;
	}
	else
	{
		FillFrameValues(Frame, *SubjectObject);

		const int32 CurveCount = FMath::Min(Frame.CurveValues.Num(), (int32)SubjectObject->mCurveValues.size());
		for (int32 Index = 0; Index < CurveCount; ++Index)
		{
			SubjectObject->mCurveValues[Index] = Frame.CurveValues[Index];
		}

		SubjectObject->CalcMatrices();

		O3DS::QuantRanges Ranges;
		Ranges.byteRange = (Component != nullptr) ? (double)FMath::Max(0.0f, Component->QuantizationByteRange) : 0.01;
		Ranges.halfRange = (Component != nullptr) ? (double)FMath::Max(0.0f, Component->QuantizationHalfRange) : 1.0;

		size_t Count = 0;
		const double DeltaThreshold = (Component != nullptr) ? (double)FMath::Max(0.0f, Component->QuantizationDeltaThreshold) : 0.0001;
		SubjectObject->SerializeUpdate(Buffer, Count, DeltaThreshold, Now, &Ranges);
	}

	BroadcastSerializedBuffer(Subject, Buffer, Now, Cache);

	if (CVarO3DSenderDebugSerialize.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("SerializedQuantized Subject=%s Bones=%d Curves=%d Bytes=%d FullSync=%s"),
			*Subject,
			Frame.BoneLocalTransforms.Num(),
			Frame.CurveValues.Num(),
			(int32)Buffer.size(),
			bNeedFullSync ? TEXT("true") : TEXT("false"));
	}
}

/** Shared broadcast + stats tail for the legacy, residual, and quantized serialization paths. */
void FO3DSenderSerializer::BroadcastSerializedBuffer(const FString& Subject, const std::vector<char>& Buffer, double Now, FSubjectCache& Cache)
{
	if (Buffer.empty())
	{
		return;
	}

	TArray<uint8> Payload;
	Payload.SetNumUninitialized((int32)Buffer.size());
	FMemory::Memcpy(Payload.GetData(), Buffer.data(), Buffer.size());

	OnSerializedFrame.Broadcast(Subject, Payload, Now);

	Cache.FramesSerialized++;
	Cache.BytesSerialized += (uint64)Payload.Num();

	if (CVarO3DSenderDebugStats.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("Stats Subject=%s Frames=%llu Bytes=%llu Dropped=%llu"),
			*Subject,
			(unsigned long long)Cache.FramesSerialized,
			(unsigned long long)Cache.BytesSerialized,
			(unsigned long long)Cache.DroppedFrames);
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
