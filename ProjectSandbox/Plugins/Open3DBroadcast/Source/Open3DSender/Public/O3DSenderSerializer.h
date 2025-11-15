// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

class UO3DSenderComponent;

namespace O3DS
{
	class Subject;
	class SubjectList;
}

/** Serialized frame event: Subject, Buffer (FlatBuffer bytes), Timestamp seconds. */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnO3DSerializedFrame, const FString& /*Subject*/, const TArray<uint8>& /*Buffer*/, double /*Timestamp*/);
/** SubjectList event for transports that prefer direct access to the FlatBuffer object model. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnO3DSubjectListReady, const FString& /*Subject*/, const TSharedPtr<O3DS::SubjectList>& /*Payload*/);

struct FO3DSSkeletonDescriptor;
struct FO3DSPoseFrame;

/**
 * Lightweight broadcaster-side serializer. Owns descriptor/curve caches per subject and surfaces both
 * raw SubjectList objects and serialized FlatBuffer payloads to downstream listeners.
 */
class OPEN3DSENDER_API FO3DSenderSerializer
{
public:
	FO3DSenderSerializer();
	~FO3DSenderSerializer();

	// Attach to a capture component. Safe to call multiple times; no-op if already attached.
	void Attach(UO3DSenderComponent* InComponent);

	// Detach from the capture component.
	void Detach(UO3DSenderComponent* InComponent);

	// Emitted after a SubjectList buffer is produced for a frame
	FOnO3DSerializedFrame OnSerializedFrame;
	// Emitted with a shared SubjectList prior to serialization
	FOnO3DSubjectListReady OnSubjectListReady;

	// Console hook to dump all serializer stats across live instances
	static void DumpAllStats();

	/**
	 * Removes a specific subject cache entry. Useful when subjects are dynamically destroyed or renamed
	 * so long-running serializer instances do not retain stale metadata.
	 */
	void RemoveSubjectCache(const FString& Subject);

	/** Clears every cached subject, typically invoked when the owning component stops capturing. */
	void ClearAllCaches();

	/** Returns the number of cached subjects (primarily for diagnostics/tests). */
	int32 GetCacheCount() const { return SubjectState.Num(); }

private:
	void OnDescriptorReady(const FString& Subject, const struct FO3DSSkeletonDescriptor& Descriptor);
	void OnPoseFrameReady(const FString& Subject, const struct FO3DSPoseFrame& Frame);

	struct FSubjectCache
	{
		uint64 SkeletonHash = 0;
		TArray<FName> BoneNames;
		TArray<int32> ParentIndices;
		TArray<FName> CurveNames;
		TMap<FName, int32> CurveIndex;
		bool bDescriptorSent = false;

		uint64 FramesSerialized = 0;
		uint64 BytesSerialized = 0;
		uint64 DroppedFrames = 0;
		FString LastError;
	};

	TMap<FString, FSubjectCache> SubjectState;
	UO3DSenderComponent* Component = nullptr;

	void BuildOrUpdateCache(const FString& Subject, const struct FO3DSSkeletonDescriptor& Descriptor);
	void EnsureCurveIndex(FSubjectCache& Cache);
	void SerializeFrame(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame);
	void BuildSubjectFromDescriptor(const FString& SubjectName, const FO3DSSkeletonDescriptor& Descriptor, O3DS::Subject& OutSubject);
	void FillFrameValues(const FO3DSPoseFrame& Frame, O3DS::Subject& InOutSubject);
	void DumpStatsInstance() const;

	static TArray<FO3DSenderSerializer*> GInstances;
};
