// Copyright (c) Open3DStream Contributors

#include "O3DSenderSerializer.h"

#include "O3DSenderComponent.h"
#include "O3DSenderLogs.h"

#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

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

	// Residual coding (C2, roadmap doc §5) and quantization (D1, roadmap doc
	// §6) are configured through UPROPERTYs on UO3DSenderComponent, which the
	// component snapshots into FO3DSenderEncodingSettings on every sampled
	// frame (FO3DSPoseFrame::Encoding). The serializer holds no component and
	// never reads a UObject (SND-14, SND-22, ADR 0008 item 6, WP-A2a).

	/** Guards FO3DSenderSerializer::GInstances. */
	FCriticalSection& GetSerializerInstancesLock()
	{
		static FCriticalSection InstancesLock;
		return InstancesLock;
	}

	/** At most one dropped-frame warning per subject per this many seconds. */
	constexpr double DropWarningIntervalSeconds = 5.0;

	/** ADR 0005 (ii): FullSyncIntervalSeconds is clamped to this range. */
	constexpr float MinFullSyncIntervalSeconds = 0.25f;
	constexpr float MaxFullSyncIntervalSeconds = 10.0f;

	O3DS::ResidualPredictorId ToCorePredictor(EO3DSenderResidualPredictor Predictor)
	{
		switch (Predictor)
		{
		case EO3DSenderResidualPredictor::Hold: return O3DS::ResidualPredictorId::Hold;
		case EO3DSenderResidualPredictor::Quadratic: return O3DS::ResidualPredictorId::Quadratic;
		case EO3DSenderResidualPredictor::Linear:
		default: return O3DS::ResidualPredictorId::Linear;
		}
	}

	bool HasInvalidTransform(const FTransform& T)
	{
		const FVector Tr = T.GetTranslation();
		const FQuat Rot = T.GetRotation();
		const FVector Sc = T.GetScale3D();
		auto IsBad = [](double V) { return !FMath::IsFinite(V); };
		return IsBad(Tr.X) || IsBad(Tr.Y) || IsBad(Tr.Z) ||
			IsBad(Rot.X) || IsBad(Rot.Y) || IsBad(Rot.Z) || IsBad(Rot.W) ||
			IsBad(Sc.X) || IsBad(Sc.Y) || IsBad(Sc.Z);
	}
}

TArray<FO3DSenderSerializer*> FO3DSenderSerializer::GInstances;

/**
 * Registers the instance for o3ds.Sender.DumpStats. The serializer used to do this, and subscribe to
 * the component's OnPoseFrameReady, in Attach(UO3DSenderComponent*); it now holds no component
 * (SND-22, WP-A2a): the component calls SerializePoseFrame itself. Descriptors travel on every frame
 * (FO3DSPoseFrame::Descriptor, ADR 0005 (i)), so it does not listen to OnDescriptorReady either.
 */
FO3DSenderSerializer::FO3DSenderSerializer()
{
	// Registered once, the first time a serializer is created, as Attach() did. A function-local
	// static, so two first constructions cannot register it twice.
	static const bool bRegisteredCmd = []()
	{
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("o3ds.Sender.DumpStats"),
			TEXT("Dump per-subject serialization stats to the log"),
			FConsoleCommandDelegate::CreateStatic(&FO3DSenderSerializer::DumpAllStats),
			ECVF_Default);
		return true;
	}();
	(void)bRegisteredCmd;

	FScopeLock InstancesScopeLock(&GetSerializerInstancesLock());
	GInstances.AddUnique(this);
}

FO3DSenderSerializer::~FO3DSenderSerializer()
{
	FScopeLock InstancesScopeLock(&GetSerializerInstancesLock());
	GInstances.Remove(this);
}

void FO3DSenderSerializer::SetStatsLabel(const FString& InLabel)
{
	FScopeLock StateGuard(&StateLock);
	StatsLabel = InLabel;
}

int32 FO3DSenderSerializer::GetCacheCount() const
{
	FScopeLock StateGuard(&StateLock);
	return SubjectState.Num();
}

void FO3DSenderSerializer::RequestFullSync(const FString& Subject)
{
	FScopeLock StateGuard(&StateLock);
	if (FSubjectCache* Cache = SubjectState.Find(Subject))
	{
		Cache->SyncTracker.RequestFullSync();
	}
}

void FO3DSenderSerializer::RemoveSubjectCache(const FString& Subject)
{
	if (Subject.IsEmpty())
	{
		return;
	}

	FScopeLock StateGuard(&StateLock);
	if (SubjectState.Remove(Subject) > 0)
	{
		UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("Removed serializer cache for subject '%s'"), *Subject);
	}

	// Drop the persistent residual/quantized Subject too, so if this name
	// reappears later it starts with a fresh full sync, encoder and anchors.
	// PersistentSubjects owns its Subjects through raw pointers, so the entry
	// is deleted here rather than just erased.
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
	FScopeLock StateGuard(&StateLock);
	if (SubjectState.Num() > 0)
	{
		SubjectState.Empty();
	}

	// Dropping the whole SubjectList is safe here: ~SubjectList() deletes
	// every owned O3DS::Subject*.
	PersistentSubjects.Reset();
}

FO3DSenderSerializer::FSubjectStats FO3DSenderSerializer::GetSubjectStats(const FString& Subject) const
{
	FSubjectStats Stats;
	FScopeLock StateGuard(&StateLock);
	if (const FSubjectCache* Cache = SubjectState.Find(Subject))
	{
		Stats.FramesSerialized = Cache->FramesSerialized;
		Stats.FullSyncsSent = Cache->FullSyncsSent;
		Stats.DroppedFrames = Cache->DroppedFrames;
	}
	return Stats;
}

void FO3DSenderSerializer::DropFrame(const FString& Subject, FSubjectCache& Cache, const FString& Reason)
{
	Cache.DroppedFrames++;
	Cache.DroppedSinceLastWarning++;
	Cache.LastError = Reason;

	const double Now = FPlatformTime::Seconds();
	if (Now - Cache.LastDropWarningTime >= DropWarningIntervalSeconds)
	{
		UE_LOG(LogO3DSenderSerializer, Warning, TEXT("Dropping frame for subject '%s': %s (%llu frame(s) dropped since the last warning)."),
			*Subject,
			*Reason,
			(unsigned long long)Cache.DroppedSinceLastWarning);
		Cache.LastDropWarningTime = Now;
		Cache.DroppedSinceLastWarning = 0;
	}
}

void FO3DSenderSerializer::SerializePoseFrame(const FString& Subject, const FO3DSPoseFrame& Frame)
{
	TArray<uint8> Bytes;
	bool bFullSync = false;
	SerializePoseFrameTo(Subject, Frame, Bytes, bFullSync);
}

/** Validate a captured pose frame and serialise it with the encoding the frame carries. */
bool FO3DSenderSerializer::SerializePoseFrameTo(const FString& Subject, const FO3DSPoseFrame& Frame, TArray<uint8>& OutBytes, bool& bOutFullSync)
{
	OutBytes.Reset();
	bOutFullSync = false;

	FScopeLock StateGuard(&StateLock);
	FSubjectCache& Cache = SubjectState.FindOrAdd(Subject);

	// ADR 0005 (i) / SND-1: never pad a frame to fit a descriptor. A frame
	// without one, or with a different bone count, is dropped.
	const FO3DSSkeletonDescriptor* Descriptor = Frame.Descriptor.Get();
	if (Descriptor == nullptr || !Descriptor->IsValid())
	{
		DropFrame(Subject, Cache, TEXT("no skeleton descriptor on the frame"));
		return false;
	}

	const int32 BoneCount = Frame.BoneLocalTransforms.Num();
	if (BoneCount != Descriptor->BoneNames.Num())
	{
		DropFrame(Subject, Cache, FString::Printf(TEXT("frame has %d bones but its skeleton descriptor has %d"), BoneCount, Descriptor->BoneNames.Num()));
		return false;
	}

	if (Frame.CurveValues.Num() != Frame.CurveNames.Num())
	{
		DropFrame(Subject, Cache, FString::Printf(TEXT("frame has %d curve names but %d curve values"), Frame.CurveNames.Num(), Frame.CurveValues.Num()));
		return false;
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("O3D.Sender.Serializer.Validate");
		for (int32 Index = 0; Index < BoneCount; ++Index)
		{
			if (HasInvalidTransform(Frame.BoneLocalTransforms[Index]))
			{
				DropFrame(Subject, Cache, FString::Printf(TEXT("NaN/Inf at bone %d"), Index));
				return false;
			}
		}
	}

	switch (Frame.Encoding.Mode)
	{
	case EO3DSenderEncodingMode::Residual:
	case EO3DSenderEncodingMode::Quantized:
		bOutFullSync = SerializeFramePersistent(Subject, *Descriptor, Frame, Cache, OutBytes);
		break;
	case EO3DSenderEncodingMode::Legacy:
	default:
		// Every legacy frame is a full Subject.
		SerializeFrameLegacy(Subject, *Descriptor, Frame, Cache, OutBytes);
		bOutFullSync = true;
		break;
	}
	return OutBytes.Num() > 0;
}

/** Populate a FlatBuffer Subject's transforms (names, parents, component order) from a descriptor. */
void FO3DSenderSerializer::BuildSubjectFromDescriptor(const FString& SubjectName, const FO3DSSkeletonDescriptor& Descriptor, O3DS::Subject& OutSubject)
{
	using namespace O3DS;
	OutSubject.mName = std::string(TCHAR_TO_UTF8(*SubjectName));
	OutSubject.clear();

	const int32 BoneCount = Descriptor.BoneNames.Num();
	for (int32 Index = 0; Index < BoneCount; ++Index)
	{
		const int32 ParentId = Descriptor.ParentIndices.IsValidIndex(Index) ? Descriptor.ParentIndices[Index] : -1;
		const std::string NodeName = std::string(TCHAR_TO_UTF8(*Descriptor.BoneNames[Index].ToString()));
		auto* Transform = OutSubject.addTransform(NodeName, ParentId);
		Transform->translation.value = O3DS::Vector3d(0.0, 0.0, 0.0);
		Transform->rotation.value = O3DS::Vector4d(0.0, 0.0, 0.0, 1.0);
		Transform->scale.value = O3DS::Vector3d(1.0, 1.0, 1.0);
		Transform->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
	}
}

/** Copy the frame's bone values into an already-built Subject. Callers guarantee matching counts. */
void FO3DSenderSerializer::FillFrameValues(const FO3DSPoseFrame& Frame, O3DS::Subject& InOutSubject)
{
	using namespace O3DS;
	const int32 BoneCount = Frame.BoneLocalTransforms.Num();
	if ((int32)InOutSubject.size() != BoneCount)
	{
		// SerializePoseFrame validated the frame against its descriptor and
		// the Subject was built from that descriptor; never fabricate bones.
		return;
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

/** Replace the Subject's curve names and values with the frame's. */
void FO3DSenderSerializer::FillCurves(const FO3DSPoseFrame& Frame, O3DS::Subject& InOutSubject)
{
	InOutSubject.mCurveNames.clear();
	InOutSubject.mCurveValues.clear();
	InOutSubject.mCurveNames.reserve(Frame.CurveNames.Num());
	InOutSubject.mCurveValues.reserve(Frame.CurveNames.Num());
	for (int32 Index = 0; Index < Frame.CurveNames.Num(); ++Index)
	{
		InOutSubject.mCurveNames.push_back(std::string(TCHAR_TO_UTF8(*Frame.CurveNames[Index].ToString())));
		InOutSubject.mCurveValues.push_back(Frame.CurveValues[Index]);
	}
}

/** Identity of the ordered curve name list (SND-3: compare names, not just the count). */
uint64 FO3DSenderSerializer::HashCurveNames(const TArray<FName>& Names)
{
	uint64 Hash = 1469598103934665603ull;
	auto Mix = [&Hash](uint64 Value)
	{
		Hash ^= Value;
		Hash *= 1099511628211ull;
	};
	Mix((uint64)Names.Num());
	for (const FName& Name : Names)
	{
		Mix((uint64)GetTypeHash(Name));
	}
	return Hash;
}

/** Legacy encoding: a fresh SubjectList/Subject every frame, a full topology and value snapshot. */
void FO3DSenderSerializer::SerializeFrameLegacy(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame, FSubjectCache& Cache, TArray<uint8>& OutBytes)
{
	using namespace O3DS;

	// Every legacy frame is a full Subject, which also resets the receiver's
	// anchors and residual decoder for this subject. Switching back to a
	// persistent encoding later must therefore start with a full sync.
	if (Cache.SyncTracker.HasSentFull())
	{
		Cache.SyncTracker.Reset();
	}

	TSharedPtr<SubjectList> SubjectListPtr;
	O3DS::Subject* SubjectObject = nullptr;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("O3D.Sender.Serializer.Build");
		SubjectListPtr = MakeShared<SubjectList>();
		SubjectObject = SubjectListPtr->addSubject(std::string(TCHAR_TO_UTF8(*Subject)));

		BuildSubjectFromDescriptor(Subject, Descriptor, *SubjectObject);
		FillFrameValues(Frame, *SubjectObject);
		if (Frame.CurveNames.Num() > 0)
		{
			FillCurves(Frame, *SubjectObject);
		}
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("O3D.Sender.Serializer.CalcMatrices");
		SubjectObject->CalcMatrices();
	}

	// ADR 0008 item 7: the wire time is the sampling time, not the time of
	// serialization (WP-A2a).
	std::vector<char> Buffer;
	const double Timestamp = Frame.CaptureTimeSec;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("O3D.Sender.Serializer.Core");
		SubjectListPtr->Serialize(Buffer, Timestamp);
	}

	// Transports receive frames only through the bytes below (the sender pipeline hands OutBytes
	// to SendSerialized, WP-A2c).
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("O3D.Sender.Serializer.Copy");
		BroadcastSerializedBuffer(Subject, Buffer, Timestamp, Cache, OutBytes);
	}
	Cache.FullSyncsSent++;

	if (CVarO3DSenderDebugSerialize.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("Serialized Subject=%s Bones=%d Curves=%d Bytes=%d"),
			*Subject,
			Frame.BoneLocalTransforms.Num(),
			Frame.CurveValues.Num(),
			(int32)Buffer.size());
	}
}

/**
 * Residual (C2, roadmap doc §5) and quantized (D1, roadmap doc §6) encodings: one persistent Subject
 * per subject name, a full Subject when ADR 0005 (ii) says one is due, updates otherwise.
 */
bool FO3DSenderSerializer::SerializeFramePersistent(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame, FSubjectCache& Cache, TArray<uint8>& OutBytes)
{
	using namespace O3DS;

	const FO3DSenderEncodingSettings& Settings = Frame.Encoding;
	const bool bResidual = (Settings.Mode == EO3DSenderEncodingMode::Residual);

	if (!PersistentSubjects.IsValid())
	{
		PersistentSubjects = MakeShared<SubjectList>();
	}

	const std::string SubjectNameUtf8 = std::string(TCHAR_TO_UTF8(*Subject));
	O3DS::Subject* SubjectObject = PersistentSubjects->findSubject(SubjectNameUtf8);

	// Full sync policy (ADR 0005 (ii)): first frame for this subject (after
	// StartCapture, a rename, or a switch from the legacy encoding), a
	// descriptor change, a curve NAME list change (SND-3), an encoding
	// settings change (SND-14), or FullSyncIntervalSeconds elapsed (SND-13).
	FullSyncInputs SyncInputs;
	SyncInputs.descriptorHash = Descriptor.Hash;
	SyncInputs.curveNamesHash = HashCurveNames(Frame.CurveNames);
	SyncInputs.encodingFingerprint = Settings.GetFullSyncFingerprint();
	SyncInputs.nowSeconds = Frame.CaptureTimeSec;
	SyncInputs.intervalSeconds = (double)FMath::Clamp(Settings.FullSyncIntervalSeconds, MinFullSyncIntervalSeconds, MaxFullSyncIntervalSeconds);

	uint32 SyncReasons = Cache.SyncTracker.Evaluate(SyncInputs);
	if (SubjectObject == nullptr)
	{
		SyncReasons |= FullSyncTracker::First;
	}
	else if (SubjectObject->mCurveValues.size() != (size_t)Frame.CurveValues.Num())
	{
		// Only reachable on a curve-name hash collision; keeps the by-index
		// value copy below in bounds regardless.
		SyncReasons |= FullSyncTracker::CurveNames;
	}
	const bool bNeedFullSync = (SyncReasons != FullSyncTracker::None);

	// ADR 0008 item 7: the wire time is the sampling time (WP-A2a).
	const double Timestamp = Frame.CaptureTimeSec;
	std::vector<char> Buffer;

	if (bNeedFullSync)
	{
		if (!SubjectObject)
		{
			SubjectObject = PersistentSubjects->addSubject(SubjectNameUtf8);
		}

		// Transforms are rebuilt only when the topology changed. Keeping them
		// otherwise is an allocation saving, not a correctness requirement:
		// both ends re-anchor quantization at every full sync (ADR 0005 (vii),
		// Subject::Serialize / SubjectList::ParseSubject).
		const bool bRebuildTransforms = !Cache.SyncTracker.HasSentFull()
			|| Cache.BuiltSkeletonHash != Descriptor.Hash
			|| (int32)SubjectObject->size() != Descriptor.BoneNames.Num();
		if (bRebuildTransforms)
		{
			BuildSubjectFromDescriptor(Subject, Descriptor, *SubjectObject);
			Cache.BuiltSkeletonHash = Descriptor.Hash;
		}

		FillFrameValues(Frame, *SubjectObject);
		FillCurves(Frame, *SubjectObject);
		SubjectObject->CalcMatrices();

		// A full sync doubles as a residual keyframe (ADR 0005 (ii)): the
		// receiver resets its decoder on a full Subject, so the encoder starts
		// clean too, with the predictor and keyframe interval this frame
		// carries. std::make_unique, not MakeUnique: Subject::SetResidualEncoder
		// takes a std::unique_ptr.
		if (bResidual)
		{
			const uint32 KeyframeInterval = (uint32)FMath::Max(0, Settings.ResidualKeyframeIntervalFrames);
			SubjectObject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ToCorePredictor(Settings.ResidualPredictor), KeyframeInterval));
		}
		else
		{
			SubjectObject->SetResidualEncoder(nullptr);
		}

		SubjectObject->Serialize(Buffer, Timestamp);
		Cache.SyncTracker.MarkFullSent(SyncInputs);
		Cache.FullSyncsSent++;
	}
	else
	{
		FillFrameValues(Frame, *SubjectObject);

		// Same curve names in the same order as the last full sync (the
		// name-list hash matched), so values map by index.
		for (int32 Index = 0; Index < Frame.CurveValues.Num(); ++Index)
		{
			SubjectObject->mCurveValues[Index] = Frame.CurveValues[Index];
		}

		SubjectObject->CalcMatrices();

		size_t Count = 0;
		if (bResidual)
		{
			const double DeltaThreshold = (double)FMath::Max(0.0f, Settings.ResidualDeltaThreshold);
			SubjectObject->SerializeUpdateResidual(Buffer, Count, DeltaThreshold, Timestamp);
		}
		else
		{
			O3DS::QuantRanges Ranges;
			Ranges.byteRange = (double)FMath::Max(0.0f, Settings.QuantizationByteRange);
			Ranges.halfRange = (double)FMath::Max(0.0f, Settings.QuantizationHalfRange);
			const double DeltaThreshold = (double)FMath::Max(0.0f, Settings.QuantizationDeltaThreshold);
			SubjectObject->SerializeUpdate(Buffer, Count, DeltaThreshold, Timestamp, &Ranges);
		}
	}

	BroadcastSerializedBuffer(Subject, Buffer, Timestamp, Cache, OutBytes);

	if (CVarO3DSenderDebugSerialize.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogO3DSenderSerializer, Verbose, TEXT("Serialized%s Subject=%s Bones=%d Curves=%d Bytes=%d FullSync=%s Reasons=0x%x"),
			bResidual ? TEXT("Residual") : TEXT("Quantized"),
			*Subject,
			Frame.BoneLocalTransforms.Num(),
			Frame.CurveValues.Num(),
			(int32)Buffer.size(),
			bNeedFullSync ? TEXT("true") : TEXT("false"),
			SyncReasons);
	}
	return bNeedFullSync;
}


/** Shared broadcast + stats tail for the legacy, residual, and quantized serialization paths. */
void FO3DSenderSerializer::BroadcastSerializedBuffer(const FString& Subject, const std::vector<char>& Buffer, double Timestamp, FSubjectCache& Cache, TArray<uint8>& OutBytes)
{
	if (Buffer.empty())
	{
		return;
	}

	OutBytes.SetNumUninitialized((int32)Buffer.size());
	FMemory::Memcpy(OutBytes.GetData(), Buffer.data(), Buffer.size());

	OnSerializedFrame.Broadcast(Subject, OutBytes, Timestamp);

	Cache.FramesSerialized++;
	Cache.BytesSerialized += (uint64)OutBytes.Num();

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
	// WP-A2c (A2a deviation 6): the owning pipeline serializes on its worker; reading under the
	// instance's lock gives a consistent view and never races with the worker's writes.
	FScopeLock StateGuard(&StateLock);
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
	UE_LOG(LogO3DSenderSerializer, Display, TEXT("Serializer(%s) subjects=%d"), StatsLabel.IsEmpty() ? TEXT("<unnamed>") : *StatsLabel, SubjectState.Num());
}

/** Console command handler that walks all live serializer instances and logs aggregate stats. */
void FO3DSenderSerializer::DumpAllStats()
{
	UE_LOG(LogO3DSenderSerializer, Display, TEXT("---- O3DS Sender Serializer Stats ----"));
	FScopeLock InstancesScopeLock(&GetSerializerInstancesLock());
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
