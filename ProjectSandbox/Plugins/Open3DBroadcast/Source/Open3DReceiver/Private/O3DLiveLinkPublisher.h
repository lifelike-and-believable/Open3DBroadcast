// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LiveLinkTypes.h"
#include "Templates/Function.h"

class ILiveLinkClient;
struct FO3DDecodedSubject;

/**
 * Publishes decoded subjects to LiveLink for one receiver source (WP-A3, RCV-29): creates a
 * subject once per session (RCV-7), re-pushes static data when its bone or curve names change,
 * pushes frames, and removes subjects that stopped sending. Game thread.
 */
class FO3DLiveLinkPublisher
{
public:
	/** Test seam (WP-S4): when both are bound, pushes go here instead of the client. */
	using FStaticPushHook = TFunction<void(const FLiveLinkSubjectKey&, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPushThisSession)>;
	using FFramePushHook = TFunction<void(const FLiveLinkSubjectKey&, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double WorldTime)>;

	void SetClient(ILiveLinkClient* InClient, const FGuid& InSourceGuid);
	void SetSourceGuid(const FGuid& InSourceGuid) { SourceGuid = InSourceGuid; }
	void SetTestHooks(FStaticPushHook InStaticHook, FFramePushHook InFrameHook);

	/** True when frames can be published: a LiveLink client, or both test hooks. */
	bool CanPublish() const;

	/**
	 * Pushes static data for a decoded real frame when the subject is new this session or its bone
	 * or curve names changed. Returns true when the names changed (or the subject is new), which
	 * means the topology changed for anything that keeps per-subject state.
	 */
	bool PublishStatic(const FO3DDecodedSubject& Decoded);

	/** Pushes one real frame and records the subject as active. */
	void PublishFrame(FName Subject, const TArray<FTransform>& BoneTransforms, const TArray<FName>& CurveNames, const TArray<float>& CurveValues,
		double SubjectListTime, double WorldTimeSecondsOverride, uint64 CurveHash);

	/** Pushes a synthesized (concealed) frame for a subject already published; no static push. */
	void PublishSyntheticFrame(FName Subject, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double Time);

	/**
	 * Removes every subject that published nothing for longer than ThresholdSeconds, from LiveLink
	 * and from this publisher; OnRemoved runs for each so the caller can drop its own state.
	 */
	void RemoveInactiveSubjects(double NowSeconds, double ThresholdSeconds, TFunctionRef<void(FName)> OnRemoved);

	/** Subjects that published a frame and were not removed since. */
	int32 GetActiveSubjectCount() const { return SubjectLastUpdateTime.Num(); }

	/** Forgets every subject and restarts frame ids (the transport stopped). */
	void Reset();

	/**
	 * Logs a slow frame push at most once per 5 s, with the count of those not logged since (RCV-26).
	 * PublishFrame calls it; public for its test.
	 */
	void NoteSlowFramePush(FName Subject, double PushMs, double NowSeconds);
	int32 GetSlowPushesNotLogged() const { return SlowPushesNotLogged; }

private:
	FLiveLinkSubjectKey MakeKey(FName Subject) const;
	void PushStaticData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPushThisSession);
	void PushFrameData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues,
		double TimestampSeconds, double WorldTimeSecondsOverride, uint64 CurveHash);

	ILiveLinkClient* Client = nullptr;
	FGuid SourceGuid;

	TSet<FName> InitializedSubjects;
	TMap<FName, uint64> SubjectSkeletonHashes;
	TMap<FName, uint64> SubjectCurveHashes;
	TMap<FName, double> SubjectLastUpdateTime;
	uint64 FrameCounter = 0;
	double LastSlowPushWarningTime = -1.0e9;
	int32 SlowPushesNotLogged = 0;

	FStaticPushHook TestStaticPushHook;
	FFramePushHook TestFramePushHook;
};
