// Copyright (c) Open3DStream Contributors

#include "O3DSBroadcastSerializer.h"
#include "O3DSBroadcastComponent.h"
#include "Open3DBroadcast.h"

// Core protocol headers
#include "o3ds/model.h"

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
}

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
    }
}

void FO3DSBroadcastSerializer::BuildOrUpdateCache(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor)
{
    FSubjectCache& Cache = SubjectState.FindOrAdd(Subject);
    Cache.SkeletonHash = Descriptor.Hash;
    Cache.ParentIndices = Descriptor.ParentIndices; // copy parent ids for mapping
    Cache.BoneNames = Descriptor.BoneNames;         // copy bone names
    // Reset descriptor-sent flag so next frame includes Subject
    Cache.bDescriptorSent = false;
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

    // Build a minimal descriptor using cached parent indices if available
    FO3DSSkeletonDescriptor Desc;
    Desc.ParentIndices = Cache.ParentIndices;
    Desc.BoneNames = Cache.BoneNames;
    Desc.Hash = Cache.SkeletonHash;
    if (Desc.ParentIndices.Num() != Frame.BoneLocalTransforms.Num())
    {
        // fallback: linear chain if sizes mismatch
        Desc.ParentIndices.SetNum(Frame.BoneLocalTransforms.Num());
        Desc.BoneNames.SetNum(Frame.BoneLocalTransforms.Num());
        for (int32 i = 0; i < Frame.BoneLocalTransforms.Num(); ++i)
        {
            Desc.ParentIndices[i] = (i == 0) ? -1 : i - 1;
            if (!Desc.BoneNames.IsValidIndex(i)) { Desc.BoneNames.Add(FName(TEXT("Bone"))); }
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

    // Serialize
    std::vector<char> out;
    list.Serialize(out, /*timestamp*/ 0.0);

    if (!out.empty())
    {
        TArray<uint8> Buf;
        Buf.SetNumUninitialized((int32)out.size());
        FMemory::Memcpy(Buf.GetData(), out.data(), out.size());
        const double Now = FPlatformTime::Seconds();
        OnSerializedFrame.Broadcast(Subject, Buf, Now);
        UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[M2] Serialized Subject=%s Bones=%d Curves=%d Bytes=%d"), *Subject, Frame.BoneLocalTransforms.Num(), Frame.CurveValues.Num(), Buf.Num());
    }
}
