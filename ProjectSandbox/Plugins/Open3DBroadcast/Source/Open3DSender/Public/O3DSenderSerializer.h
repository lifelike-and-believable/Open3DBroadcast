// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/sender_sync.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

namespace O3DS
{
	class Subject;
	class SubjectList;
}

/**
 * Serialized frame event: Subject, Buffer (FlatBuffer bytes), Timestamp seconds. The only output
 * of the serializer: the unused SubjectList event (OnSubjectListReady) was deleted in WP-A1 PR 5c.
 */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnO3DSerializedFrame, const FString& /*Subject*/, const TArray<uint8>& /*Buffer*/, double /*Timestamp*/);

struct FO3DSSkeletonDescriptor;
struct FO3DSPoseFrame;

/**
 * Lightweight broadcaster-side serializer. Owns per-subject encoding state and surfaces serialized
 * FlatBuffer payloads to downstream listeners.
 */
class OPEN3DSENDER_API FO3DSenderSerializer
{
public:
	FO3DSenderSerializer();
	~FO3DSenderSerializer();

	FO3DSenderSerializer(const FO3DSenderSerializer&) = delete;
	FO3DSenderSerializer& operator=(const FO3DSenderSerializer&) = delete;

	/**
	 * Name shown for this instance by o3ds.Sender.DumpStats (the owning component sets its path).
	 * Replaces the component pointer the serializer used to hold (SND-22, WP-A2a).
	 */
	void SetStatsLabel(const FString& InLabel) { StatsLabel = InLabel; }

	/**
	 * Emitted after a SubjectList buffer is produced for a frame. The timestamp is the frame's
	 * sampling time (FO3DSPoseFrame::CaptureTimeSec, ADR 0008 item 7). Fires on the thread that
	 * calls SerializePoseFrame: the game thread until the WP-A2c pipeline.
	 */
	FOnO3DSerializedFrame OnSerializedFrame;

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

	/**
	 * Serialize one sampled frame and broadcast the bytes through OnSerializedFrame. Names, parents,
	 * curves, settings and the wire timestamp all come from the frame itself (FO3DSPoseFrame::
	 * Descriptor, ::CurveNames/CurveValues as filtered, ::Encoding, ::CaptureTimeSec; ADR 0005 (i),
	 * ADR 0008 items 6 and 7). The serializer holds no component and touches no UObject (SND-22,
	 * WP-A2a), so it works standalone. A frame whose descriptor is missing, or whose bone count
	 * differs from the descriptor, is dropped with a rate-limited warning and never padded. Called
	 * by the component after sampling and curve filtering; public so tests can drive it directly.
	 */
	void SerializePoseFrame(const FString& Subject, const FO3DSPoseFrame& Frame);

	/** Per-subject counters (primarily for diagnostics/tests). */
	struct FSubjectStats
	{
		uint64 FramesSerialized = 0;
		uint64 FullSyncsSent = 0;
		uint64 DroppedFrames = 0;
	};
	FSubjectStats GetSubjectStats(const FString& Subject) const;

private:
	struct FSubjectCache
	{
		/** Descriptor hash the persistent Subject's transforms were last built from. */
		uint64 BuiltSkeletonHash = 0;
		/** Full-sync policy for the residual and quantized encodings (ADR 0005 (ii)). */
		O3DS::FullSyncTracker SyncTracker;

		uint64 FramesSerialized = 0;
		uint64 BytesSerialized = 0;
		uint64 FullSyncsSent = 0;
		uint64 DroppedFrames = 0;
		uint64 DroppedSinceLastWarning = 0;
		double LastDropWarningTime = -1.0e9;
		FString LastError;
	};

	TMap<FString, FSubjectCache> SubjectState;
	FString StatsLabel;

	// C2/D1: persistent per-subject state for the residual and quantized
	// encodings. Unlike the legacy path - which allocates a fresh
	// O3DS::SubjectList/Subject every frame, since a full Serialize()
	// snapshot needs no state to carry over - residual coding needs the
	// predictor's history, and both encodings need the per-channel last-sent
	// values and quantization anchors, to survive across frames. One
	// SubjectList persists for the serializer's lifetime, holding one
	// O3DS::Subject per subject name. Lazily created on first use.
	TSharedPtr<O3DS::SubjectList> PersistentSubjects;

	void DropFrame(const FString& Subject, FSubjectCache& Cache, const FString& Reason);
	void SerializeFrameLegacy(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame, FSubjectCache& Cache);
	void SerializeFramePersistent(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor, const FO3DSPoseFrame& Frame, FSubjectCache& Cache);
	void BroadcastSerializedBuffer(const FString& Subject, const std::vector<char>& Buffer, double Timestamp, FSubjectCache& Cache);
	static void BuildSubjectFromDescriptor(const FString& SubjectName, const FO3DSSkeletonDescriptor& Descriptor, O3DS::Subject& OutSubject);
	static void FillFrameValues(const FO3DSPoseFrame& Frame, O3DS::Subject& InOutSubject);
	static void FillCurves(const FO3DSPoseFrame& Frame, O3DS::Subject& InOutSubject);
	static uint64 HashCurveNames(const TArray<FName>& Names);
	void DumpStatsInstance() const;

	/** Every live serializer, for DumpAllStats. Guarded by a lock in the .cpp (instances may be destroyed off the game thread from WP-A2c on). */
	static TArray<FO3DSenderSerializer*> GInstances;
};
