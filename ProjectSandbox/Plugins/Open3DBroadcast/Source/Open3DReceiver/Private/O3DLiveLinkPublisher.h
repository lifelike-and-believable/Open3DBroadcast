// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LiveLinkTypes.h"
#include "Misc/Optional.h"
#include "Misc/QualifiedFrameTime.h"
#include "O3DSceneTimeMapper.h"
#include "Templates/Function.h"

class ILiveLinkClient;
struct FO3DDecodedSubject;

namespace O3DS
{
	struct SceneTime;
}

/**
 * Publishes decoded subjects to LiveLink for one receiver source (WP-A3, RCV-29): creates a
 * subject once per session (RCV-7), re-pushes static data when its bone or curve names change,
 * pushes frames, and clears the frames of subjects that stopped sending. Game thread.
 */
class FO3DLiveLinkPublisher
{
public:
	/** Test seam (WP-S4): when both are bound, pushes go here instead of the client. */
	using FStaticPushHook = TFunction<void(const FLiveLinkSubjectKey&, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPushThisSession)>;
	using FFramePushHook = TFunction<void(const FLiveLinkSubjectKey&, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double WorldTime,
		const TOptional<FQualifiedFrameTime>& SceneTime)>;

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

	/**
	 * Pushes one real frame and records the subject as active. SenderSceneTime is the frame's
	 * SubjectList.scene_time, or null (RCV-8, ADR 0013); FO3DSceneTimeMapper turns it into the
	 * frame's LiveLink SceneTime.
	 */
	void PublishFrame(FName Subject, const TArray<FTransform>& BoneTransforms, const TArray<FName>& CurveNames, const TArray<float>& CurveValues,
		double WorldTimeSecondsOverride, const O3DS::SceneTime* SenderSceneTime);

	/**
	 * Pushes a synthesized (concealed) frame for a subject already published; no static push. Its
	 * SceneTime continues the subject's timeline (FO3DSceneTimeMapper).
	 */
	void PublishSyntheticFrame(FName Subject, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double Time);

	/**
	 * Clears the LiveLink frames of every subject that published nothing for longer than
	 * ThresholdSeconds, and forgets it here; OnCleared runs for each so the caller can drop its own
	 * state. The LiveLink subject and its settings stay; its next frame pushes static data again
	 * (RCV-6). A threshold of 0 or less: never.
	 */
	void ClearInactiveSubjects(double NowSeconds, double ThresholdSeconds, TFunctionRef<void(FName)> OnCleared);

	/** Subjects that published a frame and were not cleared since. */
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
		double WorldTimeSecondsOverride, const O3DS::SceneTime* SenderSceneTime);

	ILiveLinkClient* Client = nullptr;
	FGuid SourceGuid;

	TSet<FName> InitializedSubjects;
	TMap<FName, uint64> SubjectSkeletonHashes;
	TMap<FName, uint64> SubjectCurveHashes;
	TMap<FName, double> SubjectLastUpdateTime;
	/** Each frame's LiveLink SceneTime (RCV-8, ADR 0013). */
	FO3DSceneTimeMapper SceneTimeMapper;
	uint64 FrameCounter = 0;
	double LastSlowPushWarningTime = -1.0e9;
	int32 SlowPushesNotLogged = 0;

	FStaticPushHook TestStaticPushHook;
	FFramePushHook TestFramePushHook;
};
