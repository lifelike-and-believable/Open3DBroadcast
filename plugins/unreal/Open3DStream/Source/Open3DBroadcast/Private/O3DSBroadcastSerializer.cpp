// Copyright (c) Open3DStream Contributors

#include "O3DSBroadcastSerializer.h"
#include "O3DSBroadcastComponent.h"
#include "Open3DBroadcast.h"

// Core protocol headers
#include "o3ds/model.h"

// UE includes
#include "HAL/IConsoleManager.h"

namespace
{
    // Convert UE types to std::string (UTF-8)
    static inline std::string ToStd(const FString& S)
    {
        return std::string(TCHAR_TO_UTF8(*S));
    }
    static inline std::string ToStd(const FName& N)
    {
        return std::string(TCHAR_TO_UTF8(*N.ToString()));
    }

    // Debug CVars (editor/dev facing)
    static TAutoConsoleVariable<int32> CVarO3DSDebugSerialize(
        TEXT("o3ds.Broadcast.DebugSerialize"),
        0,
        TEXT("Enable verbose serialization logging (0=off,1=on)."),
        ECVF_Default);

    static TAutoConsoleVariable<int32> CVarO3DSDebugStats(
        TEXT("o3ds.Broadcast.DebugStats"),
        0,
        TEXT("Enable per-frame basic stats logging (0=off,1=on)."),
        ECVF_Default);
}

TArray<FO3DSBroadcastSerializer*> FO3DSBroadcastSerializer::GInstances;

FO3DSBroadcastSerializer::FO3DSBroadcastSerializer() = default;
FO3DSBroadcastSerializer::~FO3DSBroadcastSerializer() = default;

void FO3DSBroadcastSerializer::Attach(UO3DSBroadcastComponent* InComponent)
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

    // Register instance for console stats dumping
    GInstances.AddUnique(this);

    // Register console command once per process (idempotent)
    static bool bRegisteredCmd = false;
    if (!bRegisteredCmd)
    {
        IConsoleManager::Get().RegisterConsoleCommand(
            TEXT("o3ds.Broadcast.DumpStats"),
            TEXT("Dump per-subject serialization stats to the log"),
            FConsoleCommandDelegate::CreateStatic(&FO3DSBroadcastSerializer::DumpAllStats),
            ECVF_Default);
        bRegisteredCmd = true;
    }

    // Bind delegates
    Component->OnDescriptorReady.AddRaw(this, &FO3DSBroadcastSerializer::OnDescriptorReady);
    Component->OnPoseFrameReady.AddRaw(this, &FO3DSBroadcastSerializer::OnPoseFrameReady);
}

void FO3DSBroadcastSerializer::Detach(UO3DSBroadcastComponent* InComponent)
{
    if (Component && Component == InComponent)
    {
        Component->OnDescriptorReady.RemoveAll(this);
        Component->OnPoseFrameReady.RemoveAll(this);
        Component = nullptr;

        GInstances.Remove(this);
    }
}

void FO3DSBroadcastSerializer::BuildOrUpdateCache(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor)
{
    FSubjectCache& Cache = SubjectState.FindOrAdd(Subject);
    const bool bHashChanged = (Cache.SkeletonHash != Descriptor.Hash);
    Cache.SkeletonHash = Descriptor.Hash;
    Cache.ParentIndices = Descriptor.ParentIndices; // copy parent ids for mapping
    Cache.BoneNames = Descriptor.BoneNames;         // copy bone names
    // Reset descriptor-sent flag so next frame includes Subject
    Cache.bDescriptorSent = false;

    // Log descriptor built/rebuilt when enabled
    if (CVarO3DSDebugSerialize.GetValueOnAnyThread() != 0)
    {
        UE_LOG(LogO3DSBroadcast, Log, TEXT("Descriptor %s for Subject=%s Bones=%d Curves=%d Hash=%llu"),
            bHashChanged ? TEXT("rebuilt") : TEXT("built"),
            *Subject,
            Cache.BoneNames.Num(),
            Cache.CurveNames.Num(),
            (unsigned long long)Cache.SkeletonHash);
    }
}

void FO3DSBroadcastSerializer::EnsureCurveIndex(FSubjectCache& Cache)
{
    Cache.CurveIndex.Reset();
    for (int32 i = 0; i < Cache.CurveNames.Num(); ++i)
    {
        Cache.CurveIndex.Add(Cache.CurveNames[i], i);
    }
}

void FO3DSBroadcastSerializer::OnDescriptorReady(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor)
{
    BuildOrUpdateCache(Subject, Descriptor);
}

static inline bool HasInvalid(const FTransform& T)
{
    const FVector Tr = T.GetTranslation();
    const FQuat R = T.GetRotation();
    const FVector S = T.GetScale3D();
    auto IsBad = [](double v){ return !FMath::IsFinite(v); };
    if (IsBad(Tr.X) || IsBad(Tr.Y) || IsBad(Tr.Z)) return true;
    if (IsBad(R.X) || IsBad(R.Y) || IsBad(R.Z) || IsBad(R.W)) return true;
    if (IsBad(S.X) || IsBad(S.Y) || IsBad(S.Z)) return true;
    return false;
}

void FO3DSBroadcastSerializer::OnPoseFrameReady(const FString& Subject, const FO3DSPoseFrame& Frame)
{
    if (!Component)
    {
        return;
    }

    // Initialize curves on first frame
    FSubjectCache& Cache = SubjectState.FindOrAdd(Subject);
    if (Cache.CurveNames.Num() == 0 && Frame.CurveNames.Num() > 0)
    {
        Cache.CurveNames = Frame.CurveNames; // stable-sorted by component
        EnsureCurveIndex(Cache);
        Cache.bDescriptorSent = false; // ensure descriptor goes out with curves
    }

    // Validate sizes
    const int32 BoneCount = Frame.BoneLocalTransforms.Num();
    if (BoneCount <= 0)
    {
        Cache.DroppedFrames++;
        Cache.LastError = TEXT("Empty transform array");
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("Skipping frame: Subject=%s reason=%s"), *Subject, *Cache.LastError);
        return;
    }

    // Validate data
    for (int32 i = 0; i < BoneCount; ++i)
    {
        if (HasInvalid(Frame.BoneLocalTransforms[i]))
        {
            Cache.DroppedFrames++;
            Cache.LastError = FString::Printf(TEXT("NaN/Inf at bone %d"), i);
            UE_LOG(LogO3DSBroadcast, Warning, TEXT("Skipping frame: Subject=%s reason=%s"), *Subject, *Cache.LastError);
            return;
        }
    }

    // Build a descriptor using cached skeleton and clamp to current required bones count
    FO3DSSkeletonDescriptor Desc;
    Desc.ParentIndices = Cache.ParentIndices;
    Desc.BoneNames = Cache.BoneNames;
    Desc.Hash = Cache.SkeletonHash;

    const int32 RequiredCount = Frame.BoneLocalTransforms.Num();
    if (Desc.ParentIndices.Num() != RequiredCount)
    {
        // Clamp to match the transforms count so indices and names remain valid
        Desc.ParentIndices.SetNum(RequiredCount);
        Desc.BoneNames.SetNum(RequiredCount);
        if (CVarO3DSDebugSerialize.GetValueOnAnyThread() != 0)
        {
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Clamped descriptor to RequiredCount=%d for Subject=%s"), RequiredCount, *Subject);
        }
    }

    SerializeFrame(Subject, Desc, Frame);
}

void FO3DSBroadcastSerializer::BuildSubjectFromDescriptor(const FString& SubjectName, const FO3DSSkeletonDescriptor& Descriptor, O3DS::Subject& OutSubject)
{
    using namespace O3DS;
    OutSubject.mName = ToStd(SubjectName);
    OutSubject.clear();

    const int32 BoneCount = Descriptor.ParentIndices.Num();
    for (int32 i = 0; i < BoneCount; ++i)
    {
        const int32 ParentId = Descriptor.ParentIndices.IsValidIndex(i) ? Descriptor.ParentIndices[i] : -1;
        const char* NodeName = "";
        std::string NodeNameStd;
        if (Descriptor.BoneNames.IsValidIndex(i))
        {
            NodeNameStd = ToStd(Descriptor.BoneNames[i]);
            NodeName = NodeNameStd.c_str();
        }
        auto* t = OutSubject.addTransform(NodeName, ParentId);
        // Identity TRS in descriptor; values arrive per-frame
        t->translation.value = O3DS::Vector3d(0.0, 0.0, 0.0);
        t->rotation.value    = O3DS::Vector4d(0.0, 0.0, 0.0, 1.0);
        t->scale.value       = O3DS::Vector3d(1.0, 1.0, 1.0);
        t->transformOrder    = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
    }
}

void FO3DSBroadcastSerializer::FillFrameValues(const FO3DSPoseFrame& Frame, O3DS::Subject& InOutSubject)
{
    using namespace O3DS;
    const int32 BoneCount = Frame.BoneLocalTransforms.Num();
    if ((int32)InOutSubject.size() != BoneCount)
    {
        // Rebuild transforms if size mismatch (defensive)
        InOutSubject.clear();
        for (int32 i = 0; i < BoneCount; ++i)
        {
            auto* t = InOutSubject.addTransform("", (i == 0) ? -1 : i - 1);
            t->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
        }
    }

    for (int32 i = 0; i < BoneCount; ++i)
    {
        const FTransform& Rel = Frame.BoneLocalTransforms[i];
        const FVector T = Rel.GetTranslation();
        const FQuat   Q = Rel.GetRotation();
        const FVector S = Rel.GetScale3D();
        auto* t = InOutSubject.mTransforms[i];
        t->translation.value = O3DS::Vector3d((double)T.X, (double)T.Y, (double)T.Z);
        t->rotation.value    = O3DS::Vector4d((double)Q.X, (double)Q.Y, (double)Q.Z, (double)Q.W);
        t->scale.value       = O3DS::Vector3d((double)S.X, (double)S.Y, (double)S.Z);
        if (t->transformOrder.empty())
        {
            t->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
        }
    }
}

void FO3DSBroadcastSerializer::SerializeFrame(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame)
{
    using namespace O3DS;

    SubjectList list;
    O3DS::Subject* o3subj = list.addSubject(ToStd(Subject));

    // Build descriptor skeleton and fill values
    BuildSubjectFromDescriptor(Subject, Descriptor, *o3subj);
    FillFrameValues(Frame, *o3subj);

    // Curves (full frame for M2)
    if (Frame.CurveNames.Num() > 0)
    {
        o3subj->mCurveNames.clear();
        o3subj->mCurveValues.clear();
        o3subj->mCurveNames.reserve(Frame.CurveNames.Num());
        o3subj->mCurveValues.reserve(Frame.CurveNames.Num());
        for (int32 i = 0; i < Frame.CurveNames.Num(); ++i)
        {
            o3subj->mCurveNames.push_back(ToStd(Frame.CurveNames[i]));
            o3subj->mCurveValues.push_back(i < Frame.CurveValues.Num() ? Frame.CurveValues[i] : 0.0f);
        }
    }

    // Ensure matrices are up-to-date for senders that rely on them
    o3subj->CalcMatrices();

    // Serialize with real timestamp
    std::vector<char> out;
    const double Now = FPlatformTime::Seconds();
    list.Serialize(out, /*timestamp*/ Now);

    // Metrics accounting
    FSubjectCache& Cache = SubjectState.FindOrAdd(Subject);

    if (!out.empty())
    {
        TArray<uint8> Buf;
        Buf.SetNumUninitialized((int32)out.size());
        FMemory::Memcpy(Buf.GetData(), out.data(), out.size());
        OnSerializedFrame.Broadcast(Subject, Buf, Now);

        Cache.FramesSerialized++;
        Cache.BytesSerialized += (uint64)Buf.Num();

        const bool bLogSerialize = (CVarO3DSDebugSerialize.GetValueOnAnyThread() != 0);
        if (bLogSerialize)
        {
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[M2] Serialized Subject=%s Bones=%d Curves=%d Bytes=%d"), *Subject, Frame.BoneLocalTransforms.Num(), Frame.CurveValues.Num(), Buf.Num());
        }

        if (CVarO3DSDebugStats.GetValueOnAnyThread() != 0)
        {
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Stats Subject=%s Frames=%llu Bytes=%llu Dropped=%llu"),
                *Subject,
                (unsigned long long)Cache.FramesSerialized,
                (unsigned long long)Cache.BytesSerialized,
                (unsigned long long)Cache.DroppedFrames);
        }
    }
}

void FO3DSBroadcastSerializer::DumpStatsInstance() const
{
    for (const TPair<FString, FSubjectCache>& Pair : SubjectState)
    {
        const FString& Subject = Pair.Key;
        const FSubjectCache& Cache = Pair.Value;
        UE_LOG(LogO3DSBroadcast, Display, TEXT("Subject=%s Frames=%llu Bytes=%llu Dropped=%llu LastError=%s"),
            *Subject,
            (unsigned long long)Cache.FramesSerialized,
            (unsigned long long)Cache.BytesSerialized,
            (unsigned long long)Cache.DroppedFrames,
            Cache.LastError.IsEmpty() ? TEXT("<none>") : *Cache.LastError);
    }
}

void FO3DSBroadcastSerializer::DumpAllStats()
{
    UE_LOG(LogO3DSBroadcast, Display, TEXT("---- O3DS Broadcast Serializer Stats ----"));
    if (GInstances.Num() == 0)
    {
        UE_LOG(LogO3DSBroadcast, Display, TEXT("(no active serializer instances)"));
        return;
    }
    for (const FO3DSBroadcastSerializer* Instance : GInstances)
    {
        if (Instance)
        {
            Instance->DumpStatsInstance();
        }
    }
}
