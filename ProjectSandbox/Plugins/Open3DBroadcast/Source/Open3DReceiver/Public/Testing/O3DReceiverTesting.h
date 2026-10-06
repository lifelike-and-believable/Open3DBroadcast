// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

// Test-only white-box access to the receiver module (ADR 0006, WP-T2). Each accessor below is
// befriended by the class it opens up. They are header-only: FO3DReceiverSource and
// UO3DRemoteAudioComponent are exported, so these inline functions link from any module that
// depends on Open3DReceiver. Compiled out without dev automation tests.

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Misc/QualifiedFrameTime.h"
#include "Containers/ArrayView.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DReceiverSource.h"
#include "O3DRemoteAudioComponent.h"
#include "O3DRemoteControlComponent.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Sound/SoundWaveProcedural.h"

// Test-only header: O3DS::Control::AlignerStats below needs the core type (Open3DStreamCore, which
// the test module depends on). No other public header of this module includes the core.
THIRD_PARTY_INCLUDES_START
#include "o3ds/control.h"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

struct FO3DRemoteAudioComponentTestAccessor
{
	static void BindBus(UO3DRemoteAudioComponent& Component) { Component.BindBus(); }
	static void UnbindBus(UO3DRemoteAudioComponent& Component) { Component.UnbindBus(); }

	static void CallEnsureSoundWave(UO3DRemoteAudioComponent* Component, int32 NumChannels, int32 SampleRate)
	{
		Component->EnsureSoundWave(NumChannels, SampleRate);
	}

	static void CallOnAudioPcm16(UO3DRemoteAudioComponent* Component, const O3DS::FAudioFrameMeta& Meta, TConstArrayView<uint8> PCM16Bytes)
	{
		Component->OnAudioPcm16(Meta, PCM16Bytes);
	}

	static bool CallMatchesFilter(const UO3DRemoteAudioComponent* Component, const FString& Subject, const FString& Stream)
	{
		return Component->MatchesFilter(Subject, Stream);
	}

	static USoundWaveProcedural* GetSoundWave(const UO3DRemoteAudioComponent* Component)
	{
		return Component->SoundWave;
	}

	static int32 GetCurrentChannels(const UO3DRemoteAudioComponent* Component)
	{
		return Component->CurrentChannels;
	}

	static int32 GetCurrentSampleRate(const UO3DRemoteAudioComponent* Component)
	{
		return Component->CurrentSampleRate;
	}

	static UAudioComponent* GetAudioComponent(const UO3DRemoteAudioComponent* Component)
	{
		return Component->AudioComp;
	}

	static bool IsPlaybackWanted(const UO3DRemoteAudioComponent* Component)
	{
		return Component->bPlaybackWanted;
	}
};

struct FO3DReceiverSourceTestAccessor
{
	/** The sender time of the pose LiveLink presents for the stream carrying these subjects (control alignment, ADR 0011 item 9; RCV-8). */
	static bool GetPresentedSenderTimeUs(FO3DReceiverSource& Source, const std::vector<std::string>& MocapSubjects, uint64_t& OutUs)
	{
		return Source.GetPresentedSenderTimeUs(MocapSubjects, OutUs);
	}

	static void SetActiveConfig(FO3DReceiverSource& Source, const FO3DTransportConfig& Config)
	{
		Source.ActiveConfig = Config;
	}

	static void CallFinalizeAudioMeta(const FO3DReceiverSource& Source, O3DS::FAudioFrameMeta& Meta)
	{
		Source.FinalizeAudioMeta(Meta);
	}

	static void SetSourceGuid(FO3DReceiverSource& Source, const FGuid& Guid)
	{
		Source.SetSourceGuid(Guid);
	}

	static TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> MakeAudioSink(const FO3DReceiverSource& Source)
	{
		return Source.MakeAudioSink();
	}

	// Control channel (ADR 0011, CTL-4).
	static bool StartTransport(FO3DReceiverSource& Source) { return Source.StartTransport(); }
	static void StopTransport(FO3DReceiverSource& Source) { Source.StopTransport(); }
	static uint64 GetControlPayloadsDroppedDisabled(const FO3DReceiverSource& Source) { return Source.GetControlPayloadsDroppedDisabled(); }
	static const O3DS::Control::AlignerStats& GetAlignerStats(const FO3DReceiverSource& Source) { return Source.GetControlAlignerStats(); }
	static size_t GetHeldControlChanges(const FO3DReceiverSource& Source) { return Source.GetNumHeldControlChanges(); }

	// Typed config (WP-A1 PR 5a): the config the source would start its transport with.
	static FO3DTransportConfig BuildTransportConfig(const FO3DReceiverSource& Source) { return Source.BuildTransportConfig(); }

	// Schema validation (WP-A1 PR 5c): what the last StartTransport() refused, and the status line.
	static FO3DTransportResult GetLastTransportResult(const FO3DReceiverSource& Source) { return Source.LastTransportResult; }
	static FText GetSourceStatus(const FO3DReceiverSource& Source) { return Source.SourceStatus; }

	// Source status (WP-U3, RCV-16): what Tick does for the status, with a chosen "now".
	static void DrainConnectionState(FO3DReceiverSource& Source) { Source.DrainConnectionState(); }
	static void UpdateStalledStatus(FO3DReceiverSource& Source, double NowSeconds) { Source.UpdateStalledStatus(NowSeconds); }
	static bool HasStartFailed(const FO3DReceiverSource& Source) { return Source.bStartFailed.load(); }
	static void Poll(FO3DReceiverSource& Source) { if (Source.ActiveReceiver.IsValid()) { Source.ActiveReceiver->Poll(); } }
};

/** Binds a remote control component to the bus without a world (BeginPlay needs one). */
struct FO3DRemoteControlComponentTestAccessor
{
	static void Bind(UO3DRemoteControlComponent& Component) { Component.Bind(); }
	static void Unbind(UO3DRemoteControlComponent& Component) { Component.Unbind(); }
};

/** Records the LiveLink static and frame pushes of one FO3DReceiverSource (WP-S4 correctness tests). */
struct FO3DReceiverCorrectnessTestAccessor
{
	struct FStaticPush
	{
		FName Subject;
		TArray<FName> BoneNames;
		TArray<int32> BoneParents;
		TArray<FName> CurveNames;
		bool bFirstPushThisSession = false;
	};

	struct FFramePush
	{
		FName Subject;
		TArray<FTransform> Transforms;
		TArray<float> Curves;
		double WorldTime = 0.0;
		/** The frame's LiveLink SceneTime; unset when it was left at LiveLink's default (RCV-8). */
		TOptional<FQualifiedFrameTime> SceneTime;
	};

	struct FRecorder
	{
		TArray<FStaticPush> Statics;
		TArray<FFramePush> Frames;

		int32 CountFrames(FName Subject) const
		{
			int32 Count = 0;
			for (const FFramePush& Frame : Frames)
			{
				Count += (Frame.Subject == Subject) ? 1 : 0;
			}
			return Count;
		}
	};

	static void BindRecorder(FO3DReceiverSource& Source, const TSharedRef<FRecorder>& Recorder)
	{
		auto StaticHook = [Recorder](const FLiveLinkSubjectKey& Key, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPush)
		{
			FStaticPush Push;
			Push.Subject = Key.SubjectName.Name;
			Push.BoneNames = BoneNames;
			Push.BoneParents = BoneParents;
			Push.CurveNames = CurveNames;
			Push.bFirstPushThisSession = bFirstPush;
			Recorder->Statics.Add(MoveTemp(Push));
		};
		auto FrameHook = [Recorder](const FLiveLinkSubjectKey& Key, const TArray<FTransform>& Transforms, const TArray<float>& Curves, double WorldTime,
			const TOptional<FQualifiedFrameTime>& SceneTime)
		{
			FFramePush Push;
			Push.Subject = Key.SubjectName.Name;
			Push.Transforms = Transforms;
			Push.Curves = Curves;
			Push.WorldTime = WorldTime;
			Push.SceneTime = SceneTime;
			Recorder->Frames.Add(MoveTemp(Push));
		};
		Source.SetTestPushHooks(MoveTemp(StaticHook), MoveTemp(FrameHook));
	}

	/** The same consumer object StartTransport() hands to a real transport. */
	static TSharedRef<ISerializedFrameConsumer> MakeConsumer(const TSharedRef<FO3DReceiverSource>& Source)
	{
		return FO3DReceiverSource::MakeSerializedConsumer(TWeakPtr<FO3DReceiverSource>(Source));
	}
};

class FO3DReceiverFrameDecoder;
namespace O3DS
{
	class SubjectList;
}

/**
 * Owns one FO3DReceiverFrameDecoder (a private class of this module, WP-A3) for its unit tests,
 * and copies out what one Decode produced.
 */
class OPEN3DRECEIVER_API FO3DReceiverFrameDecoderProbe
{
public:
	FO3DReceiverFrameDecoderProbe();
	~FO3DReceiverFrameDecoderProbe();

	FO3DReceiverFrameDecoderProbe(const FO3DReceiverFrameDecoderProbe&) = delete;
	FO3DReceiverFrameDecoderProbe& operator=(const FO3DReceiverFrameDecoderProbe&) = delete;

	/** Decodes the first subject of an O3DS buffer (parsed into a SubjectList kept by the probe). */
	bool Decode(TConstArrayView<uint8> Buffer, bool bFullDescriptor);
	/** Decodes a subject built in memory, so values the wire's floats cannot carry can be tested. */
	bool DecodeSubject(const O3DS::Subject& Subject, bool bFullDescriptor);
	void ForgetSubject(FName SubjectName);
	void Reset();
	uint64 GetSkeletonBuilds() const;
	uint64 GetCurveNameBuilds() const;

	// The last successful Decode, copied.
	FName SubjectName;
	TArray<FName> BoneNames;
	TArray<int32> BoneParents;
	TArray<FTransform> BoneTransforms;
	TArray<FName> CurveNames;
	TArray<float> CurveValues;
	uint64 SkeletonHash = 0;
	uint64 CurveHash = 0;

private:
	TUniquePtr<FO3DReceiverFrameDecoder> Decoder;
	/** Kept so parsed subjects persist across Decode calls, like a receiver stream's list. */
	TUniquePtr<O3DS::SubjectList> List;
};

class FO3DLiveLinkPublisher;

/**
 * Owns one FO3DLiveLinkPublisher (a private class of this module, WP-A3) with recording test hooks
 * bound, for its unit tests. Bone and curve hashes are computed here as the frame decoder does.
 */
class OPEN3DRECEIVER_API FO3DLiveLinkPublisherProbe
{
public:
	struct FStaticPush
	{
		FName Subject;
		TArray<FName> CurveNames;
		bool bFirstPushThisSession = false;
	};

	struct FFramePush
	{
		FName Subject;
		int32 NumTransforms = 0;
		double WorldTime = 0.0;
		TOptional<FQualifiedFrameTime> SceneTime;
	};

	explicit FO3DLiveLinkPublisherProbe(bool bBindHooks = true);
	~FO3DLiveLinkPublisherProbe();

	FO3DLiveLinkPublisherProbe(const FO3DLiveLinkPublisherProbe&) = delete;
	FO3DLiveLinkPublisherProbe& operator=(const FO3DLiveLinkPublisherProbe&) = delete;

	bool CanPublish() const;
	/** PublishStatic for a subject with these names; returns whether the topology changed. */
	bool PublishStatic(FName Subject, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames);
	void PublishFrame(FName Subject, int32 NumTransforms, double WorldTime);
	/** PublishFrame with a sender timecode (SubjectList.scene_time; RCV-8). */
	void PublishFrameWithSenderTime(FName Subject, int32 NumTransforms, double WorldTime, int32 Frame, float SubFrame, int32 RateNumerator, int32 RateDenominator);
	void PublishSyntheticFrame(FName Subject, int32 NumTransforms, double Time);
	/** Subjects cleared, in the order the publisher reported them. */
	TArray<FName> ClearInactiveSubjects(double NowSeconds, double ThresholdSeconds);
	int32 GetActiveSubjectCount() const;
	/** What PublishFrame does after a slow push (RCV-26). */
	void NoteSlowFramePush(FName Subject, double PushMs, double NowSeconds);
	int32 GetSlowPushesNotLogged() const;
	void Reset();

	TArray<FStaticPush> Statics;
	TArray<FFramePush> Frames;

private:
	TUniquePtr<FO3DLiveLinkPublisher> Publisher;
};

class FO3DSceneTimeMapper;

/** Owns one FO3DSceneTimeMapper (a private class of this module, RCV-8) for its unit tests. */
class OPEN3DRECEIVER_API FO3DSceneTimeMapperProbe
{
public:
	FO3DSceneTimeMapperProbe();
	~FO3DSceneTimeMapperProbe();

	FO3DSceneTimeMapperProbe(const FO3DSceneTimeMapperProbe&) = delete;
	FO3DSceneTimeMapperProbe& operator=(const FO3DSceneTimeMapperProbe&) = delete;

	/** FO3DSceneTimeMapper::Map with a sender timecode (frame, sub-frame, rate). */
	TOptional<FQualifiedFrameTime> MapSender(FName Subject, int32 Frame, float SubFrame, int32 RateNumerator, int32 RateDenominator,
		double WorldTime, double NowSeconds, const TOptional<FQualifiedFrameTime>& EngineTime);
	/** FO3DSceneTimeMapper::Map without one. */
	TOptional<FQualifiedFrameTime> MapWithout(FName Subject, double WorldTime, double NowSeconds, const TOptional<FQualifiedFrameTime>& EngineTime);
	void ForgetSubject(FName Subject);
	void Reset();

private:
	TUniquePtr<FO3DSceneTimeMapper> Mapper;
};

class FO3DReceiverStreamScheduler;
class FO3DReceiverConcealment;
class UO3DReceiverSourceSettings;

/**
 * Owns one FO3DReceiverStreamScheduler (a private class of this module, WP-A3) whose apply callback
 * records the packets it releases.
 */
class OPEN3DRECEIVER_API FO3DReceiverStreamSchedulerProbe
{
public:
	struct FReleased
	{
		FString Label;
		int32 NumBytes = 0;
		bool bGated = false;
		uint64 Seq = 0;
		double LegacyTimestampSeconds = 0.0;
	};

	FO3DReceiverStreamSchedulerProbe();
	~FO3DReceiverStreamSchedulerProbe();

	FO3DReceiverStreamSchedulerProbe(const FO3DReceiverStreamSchedulerProbe&) = delete;
	FO3DReceiverStreamSchedulerProbe& operator=(const FO3DReceiverStreamSchedulerProbe&) = delete;

	/** Peeks the packet's metadata and pushes it; false when the packet does not verify. */
	bool Push(const FString& Subject, TConstArrayView<uint8> Buffer, double TimestampSeconds, double NowSeconds);
	void Flush(double NowSeconds);
	void PruneIdle(double NowSeconds, double IdleSeconds);
	void Reset();
	int32 GetNumStreams() const;

	TArray<FReleased> Released;

private:
	TUniquePtr<FO3DReceiverStreamScheduler> Scheduler;
};

/** Owns one FO3DReceiverConcealment (a private class of this module, WP-A3) and records its synthesized frames. */
class OPEN3DRECEIVER_API FO3DReceiverConcealmentProbe
{
public:
	struct FSynthetic
	{
		FName Subject;
		TArray<FTransform> Transforms;
		double Time = 0.0;
	};

	FO3DReceiverConcealmentProbe();
	~FO3DReceiverConcealmentProbe();

	FO3DReceiverConcealmentProbe(const FO3DReceiverConcealmentProbe&) = delete;
	FO3DReceiverConcealmentProbe& operator=(const FO3DReceiverConcealmentProbe&) = delete;

	void ObserveRealFrame(const UO3DReceiverSourceSettings* Settings, FName Subject, double PresentationTimeSeconds, const TArray<FTransform>& Transforms, bool bTopologyChanged);
	void NoteClockOffset(int64 OffsetEstimateUs);
	void Tick(const UO3DReceiverSourceSettings* Settings, bool bCanPublish, double NowSeconds);
	void ForgetSubject(FName Subject);
	void Reset();
	int32 GetNumEngines() const;
	bool HasClockOffsetEstimate() const;

	TArray<FSynthetic> Synthetic;

private:
	TUniquePtr<FO3DReceiverConcealment> Concealment;
};

class FO3DReceiverControlRouter;

/** Owns one FO3DReceiverControlRouter (a private class of this module, WP-A3). */
class OPEN3DRECEIVER_API FO3DReceiverControlRouterProbe
{
public:
	FO3DReceiverControlRouterProbe();
	~FO3DReceiverControlRouterProbe();

	FO3DReceiverControlRouterProbe(const FO3DReceiverControlRouterProbe&) = delete;
	FO3DReceiverControlRouterProbe& operator=(const FO3DReceiverControlRouterProbe&) = delete;

	void ApplyConfig();
	void HandlePayload(bool bEnabled, TConstArrayView<uint8> Payload, double NowSeconds, const FString& StreamId);
	/** PresentedSenderTimeUs < 0: no live mocap stream for the source. Records the subjects asked about. */
	void Tick(bool bEnabled, double NowSeconds, const FString& StreamId, int64 PresentedSenderTimeUs);
	void FlushHeld(const FString& StreamId);
	uint64 GetPayloadsDroppedDisabled() const;
	int32 GetNumHeld() const;

	TArray<FString> LastAskedSubjects;

private:
	TUniquePtr<FO3DReceiverControlRouter> Router;
};

#endif // WITH_DEV_AUTOMATION_TESTS
