// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

class UO3DSBroadcastComponent;

namespace O3DS { class Subject; class SubjectList; }

// Serialized frame event: Subject, Buffer (flatbuffer bytes), Timestamp seconds
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnO3DSSerializedFrame, const FString& /*Subject*/, const TArray<uint8>& /*Buffer*/, double /*Timestamp*/);

// Forward decl of structs from component header
struct FO3DSSkeletonDescriptor;
struct FO3DSPoseFrame;

// Lightweight broadcaster-side serializer. Not a UObject; owned by a component or manager.
class OPEN3DBROADCAST_API FO3DSBroadcastSerializer
{
public:
    FO3DSBroadcastSerializer();
    ~FO3DSBroadcastSerializer();

    // Attach to a capture component. Safe to call multiple times; no-op if already attached.
    void Attach(UO3DSBroadcastComponent* InComponent);

    // Detach from the capture component.
    void Detach(UO3DSBroadcastComponent* InComponent);

    // Emitted after a SubjectList buffer is produced for a frame
    FOnO3DSSerializedFrame OnSerializedFrame;

private:
    // Delegate sinks
    void OnDescriptorReady(const FString& Subject, const struct FO3DSSkeletonDescriptor& Descriptor);
    void OnPoseFrameReady(const FString& Subject, const struct FO3DSPoseFrame& Frame);

    struct FSubjectCache
    {
        uint64 SkeletonHash = 0;                  // hash from FO3DSSkeletonDescriptor
        TArray<FName> BoneNames;                  // cached bone names from descriptor
        TArray<int32> ParentIndices;              // cached parent indices from descriptor
        TArray<FName> CurveNames;                 // stable full curve name set for descriptor
        TMap<FName, int32> CurveIndex;            // map for quick value placement
        bool bDescriptorSent = false;             // track if descriptor was emitted at least once
    };

    // Per-subject cache
    TMap<FString, FSubjectCache> SubjectState;

    // Bound component and delegate handles
    UO3DSBroadcastComponent* Component = nullptr;

    // Helpers
    void BuildOrUpdateCache(const FString& Subject, const struct FO3DSSkeletonDescriptor& Descriptor);
    void EnsureCurveIndex(FSubjectCache& Cache);

    // Serialization helpers (best-effort to the documented model API)
    void SerializeFrame(const FString& Subject, const struct FO3DSSkeletonDescriptor& Descriptor, const struct FO3DSPoseFrame& Frame);

    // Mapping helpers (for tests/clarity)
    void BuildSubjectFromDescriptor(const FString& SubjectName, const struct FO3DSSkeletonDescriptor& Descriptor, O3DS::Subject& OutSubject);
    void FillFrameValues(const struct FO3DSPoseFrame& Frame, O3DS::Subject& InOutSubject);
};
