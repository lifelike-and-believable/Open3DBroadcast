// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DLiveLinkPublisher.h"

#include "Misc/App.h"

#include "O3DReceiverFrameDecoder.h"
#include "O3DReceiverLogs.h"

#include "HAL/PlatformTime.h"
#include "ILiveLinkClient.h"
#include "LiveLinkPresetTypes.h"
#include "LiveLinkSubjectSettings.h"
#include "Misc/QualifiedFrameTime.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"

namespace O3DLiveLinkPublisherPrivate
{
	/** A push slower than this is logged as a likely LiveLink client stall. */
	constexpr double SlowPushWarningMs = 5.0;
	/** Slow frame pushes are logged at most this often: a sustained stall would flood the log (RCV-26). */
	constexpr double SlowPushWarningIntervalSeconds = 5.0;
}

void FO3DLiveLinkPublisher::SetClient(ILiveLinkClient* InClient, const FGuid& InSourceGuid)
{
	Client = InClient;
	SourceGuid = InSourceGuid;
}

void FO3DLiveLinkPublisher::SetTestHooks(FStaticPushHook InStaticHook, FFramePushHook InFrameHook)
{
	TestStaticPushHook = MoveTemp(InStaticHook);
	TestFramePushHook = MoveTemp(InFrameHook);
}

bool FO3DLiveLinkPublisher::CanPublish() const
{
	return Client != nullptr || (TestStaticPushHook && TestFramePushHook);
}

FLiveLinkSubjectKey FO3DLiveLinkPublisher::MakeKey(FName Subject) const
{
	return FLiveLinkSubjectKey(SourceGuid, FLiveLinkSubjectName(Subject));
}

bool FO3DLiveLinkPublisher::PublishStatic(const FO3DDecodedSubject& Decoded)
{
	const FName SubjectFName = Decoded.SubjectName;
	const uint64 SkeletonHash = Decoded.SkeletonHash;
	const uint64 CurveHash = Decoded.CurveHash;

	const uint64* ExistingSkeletonHash = SubjectSkeletonHashes.Find(SubjectFName);
	const uint64* ExistingCurveHash = SubjectCurveHashes.Find(SubjectFName);
	const bool bNeedStaticUpdate = (!ExistingSkeletonHash || *ExistingSkeletonHash != SkeletonHash) || (!ExistingCurveHash || *ExistingCurveHash != CurveHash);
	const bool bWasSkeletonKnown = ExistingSkeletonHash != nullptr;

	if (!InitializedSubjects.Contains(SubjectFName) || bNeedStaticUpdate)
	{
		// Timed on its own to tell which push blocks when LiveLink stalls.
		const double StaticStartTime = FPlatformTime::Seconds();
		PushStaticData(MakeKey(SubjectFName), *Decoded.BoneNames, *Decoded.BoneParents, *Decoded.CurveNames, !InitializedSubjects.Contains(SubjectFName));
		const double StaticTimeMs = (FPlatformTime::Seconds() - StaticStartTime) * 1000.0;
		if (StaticTimeMs > O3DLiveLinkPublisherPrivate::SlowPushWarningMs)
		{
			UE_LOG(LogO3DReceiverSource, Warning,
				TEXT("PushSubjectStaticData took %.2f ms (subject='%s', may indicate LiveLink client blocking)"),
				StaticTimeMs, *SubjectFName.ToString());
		}

		InitializedSubjects.Add(SubjectFName);
		SubjectSkeletonHashes.Add(SubjectFName, SkeletonHash);
		SubjectCurveHashes.Add(SubjectFName, CurveHash);

		if (!bWasSkeletonKnown)
		{
			UE_LOG(LogO3DReceiverSource, Log, TEXT("Created subject '%s'"), *SubjectFName.ToString());
		}
		else if (bNeedStaticUpdate)
		{
			UE_LOG(LogO3DReceiverSource, Log, TEXT("Static data updated for subject '%s'"), *SubjectFName.ToString());
		}
	}
	else
	{
		SubjectCurveHashes[SubjectFName] = CurveHash;
	}
	return bNeedStaticUpdate;
}

void FO3DLiveLinkPublisher::PublishFrame(FName Subject, const TArray<FTransform>& BoneTransforms, const TArray<FName>& CurveNames, const TArray<float>& CurveValues,
	double WorldTimeSecondsOverride, const O3DS::SceneTime* SenderSceneTime)
{
	(void)CurveNames; // the static data carries the names; a frame carries values only

	const double FrameStartTime = FPlatformTime::Seconds();
	PushFrameData(MakeKey(Subject), BoneTransforms, CurveValues, WorldTimeSecondsOverride, SenderSceneTime);
	const double FrameTimeMs = (FPlatformTime::Seconds() - FrameStartTime) * 1000.0;
	if (FrameTimeMs > O3DLiveLinkPublisherPrivate::SlowPushWarningMs)
	{
		NoteSlowFramePush(Subject, FrameTimeMs, FPlatformTime::Seconds());
	}

	SubjectLastUpdateTime.Add(Subject, FPlatformTime::Seconds());
}

void FO3DLiveLinkPublisher::PublishSyntheticFrame(FName Subject, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double Time)
{
	// No static data: a synthesized frame never changes the topology, so the registered bone and
	// curve names apply.
	PushFrameData(MakeKey(Subject), BoneTransforms, CurveValues, Time, nullptr);
}

void FO3DLiveLinkPublisher::RemoveInactiveSubjects(double NowSeconds, double ThresholdSeconds, TFunctionRef<void(FName)> OnRemoved)
{
	for (auto It = SubjectLastUpdateTime.CreateIterator(); It; ++It)
	{
		if ((NowSeconds - It.Value()) > ThresholdSeconds)
		{
			if (Client)
			{
				Client->RemoveSubject_AnyThread(MakeKey(It.Key()));
			}
			UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Removed inactive subject %s"), *It.Key().ToString());
			OnRemoved(It.Key());
			SceneTimeMapper.ForgetSubject(It.Key());
			SubjectSkeletonHashes.Remove(It.Key());
			SubjectCurveHashes.Remove(It.Key());
			InitializedSubjects.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
}

void FO3DLiveLinkPublisher::Reset()
{
	InitializedSubjects.Empty();
	SubjectSkeletonHashes.Empty();
	SubjectCurveHashes.Empty();
	SubjectLastUpdateTime.Empty();
	SceneTimeMapper.Reset();
	FrameCounter = 0;
	LastSlowPushWarningTime = -1.0e9;
	SlowPushesNotLogged = 0;
}

void FO3DLiveLinkPublisher::NoteSlowFramePush(FName Subject, double PushMs, double NowSeconds)
{
	if (NowSeconds - LastSlowPushWarningTime < O3DLiveLinkPublisherPrivate::SlowPushWarningIntervalSeconds)
	{
		++SlowPushesNotLogged;
		return;
	}
	UE_LOG(LogO3DReceiverSource, Warning,
		TEXT("PushSubjectFrameData took %.2f ms (subject='%s'): the LiveLink client may be blocking. %d more slow push(es) since the last warning."),
		PushMs, *Subject.ToString(), SlowPushesNotLogged);
	LastSlowPushWarningTime = NowSeconds;
	SlowPushesNotLogged = 0;
}

void FO3DLiveLinkPublisher::PushStaticData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPushThisSession)
{
	if (TestStaticPushHook)
	{
		TestStaticPushHook(SubjectKey, BoneNames, BoneParents, CurveNames, bFirstPushThisSession);
		return;
	}

	if (!Client)
	{
		return;
	}

	// RCV-7: create the LiveLink subject only on the first push of this session, and only if
	// LiveLink doesn't already have it (for example from a previous transport session or a preset
	// the user loaded). Calling CreateSubject again for an existing subject either fails with a
	// warning or replaces the user's per-subject settings (preprocessors, interpolation,
	// translators). Later hierarchy or curve changes re-push static data only.
	if (bFirstPushThisSession && Client->GetSubjectSettings(SubjectKey) == nullptr)
	{
		// A settings object allows subject-level configuration of preprocessors, interpolation and translators.
		ULiveLinkSubjectSettings* SubjectSettings = NewObject<ULiveLinkSubjectSettings>();
		if (SubjectSettings)
		{
			SubjectSettings->Initialize(SubjectKey);
			// The role on the settings object keeps ValidateProcessors() from clearing preprocessors, interpolation and translators.
			SubjectSettings->Role = ULiveLinkAnimationRole::StaticClass();
		}

		FLiveLinkSubjectPreset Preset;
		Preset.Key = SubjectKey;
		Preset.Role = ULiveLinkAnimationRole::StaticClass();
		Preset.Settings = SubjectSettings;
		Preset.bEnabled = true;
		Client->CreateSubject(Preset);
	}

	FLiveLinkStaticDataStruct StaticDataStruct;
	StaticDataStruct.InitializeWith(FLiveLinkSkeletonStaticData::StaticStruct(), nullptr);
	FLiveLinkSkeletonStaticData* SkeletonData = StaticDataStruct.Cast<FLiveLinkSkeletonStaticData>();

	SkeletonData->SetBoneNames(BoneNames);
	SkeletonData->SetBoneParents(BoneParents);
	SkeletonData->PropertyNames = CurveNames;

	Client->PushSubjectStaticData_AnyThread(SubjectKey, ULiveLinkAnimationRole::StaticClass(), MoveTemp(StaticDataStruct));
}

void FO3DLiveLinkPublisher::PushFrameData(const FLiveLinkSubjectKey& SubjectKey, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues,
	double WorldTimeSecondsOverride, const O3DS::SceneTime* SenderSceneTime)
{
	// A2.c: the sender-clock-mapped presentation time when available (gated path), otherwise the
	// apply time (legacy, ungated frames).
	const double NowSeconds = FPlatformTime::Seconds();
	const double WorldTime = (WorldTimeSecondsOverride >= 0.0) ? WorldTimeSecondsOverride : NowSeconds;
	// RCV-8 (ADR 0013): the sender's timecode, the sender's timeline continued, or WorldTime on
	// this engine's timecode (FO3DSceneTimeMapper). Unset leaves LiveLink's default.
	const TOptional<FQualifiedFrameTime> SceneTime = SceneTimeMapper.Map(SubjectKey.SubjectName.Name, SenderSceneTime, WorldTime, NowSeconds,
		FApp::GetCurrentFrameTime());

	if (TestFramePushHook)
	{
		TestFramePushHook(SubjectKey, BoneTransforms, CurveValues, WorldTime, SceneTime);
		return;
	}

	if (!Client)
	{
		return;
	}

	FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
	FLiveLinkAnimationFrameData& FrameData = *FrameDataStruct.Cast<FLiveLinkAnimationFrameData>();
	FLiveLinkBaseFrameData& BaseFrameData = FrameData;

	FrameData.Transforms = BoneTransforms;
	BaseFrameData.PropertyValues = CurveValues;
	BaseFrameData.WorldTime = WorldTime;
	// No per-frame string metadata (RCV-11): the curve hash stays internal.
	BaseFrameData.MetaData.SceneTime = SceneTime.Get(FQualifiedFrameTime());

	FrameData.FrameId = FrameCounter++;

	Client->PushSubjectFrameData_AnyThread(SubjectKey, MoveTemp(FrameDataStruct));
}
