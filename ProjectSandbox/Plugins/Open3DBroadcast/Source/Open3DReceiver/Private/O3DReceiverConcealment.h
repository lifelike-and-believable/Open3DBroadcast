// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/predict/concealment.h"
THIRD_PARTY_INCLUDES_END

class UO3DReceiverSourceSettings;

/**
 * Receiver-side concealment (roadmap doc §5/C1; WP-A3 split it out of FO3DReceiverSource): one
 * O3DS::ConcealmentEngine per subject, fed with the gated path's real frames, and a per-tick poll
 * that synthesizes a frame for a subject whose real data has starved beyond LiveLink's own
 * interpolation. Only the gated (A2) path feeds it, because only that path has a sender-clock-mapped
 * presentation time to reason about gaps in. Game thread.
 */
class FO3DReceiverConcealment
{
public:
	/** Pushes one synthesized frame for a subject (no static data: the topology did not change). */
	using FPushSyntheticFrame = TFunctionRef<void(FName Subject, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, double Time)>;

	/**
	 * Feeds one real gated frame to the subject's engine, created on first use from Settings (null:
	 * defaults). A topology change resets the engine, so history from another skeleton never mixes
	 * into a prediction. Does nothing while concealment is disabled.
	 */
	void ObserveRealFrame(const UO3DReceiverSourceSettings* Settings, FName Subject, double PresentationTimeSeconds,
		const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, bool bTopologyChanged);

	/** Records the gated path's latest clock offset estimate; from then on Tick may synthesize frames. */
	void NoteClockOffset(int64 OffsetEstimateUs);

	/**
	 * Per tick: for every engine, asks whether NowSeconds (FPlatformTime::Seconds(), the domain the
	 * real frames' presentation times are mapped into) needs a synthesized frame, pushes it, then
	 * reports the metric deltas. Does nothing unless frames can be published, a gated frame has been
	 * observed, an engine exists and concealment is enabled.
	 */
	void Tick(const UO3DReceiverSourceSettings* Settings, bool bCanPublish, double NowSeconds, FPushSyntheticFrame PushSyntheticFrame);

	/** Drops one subject's engine and metric snapshot (the subject was removed). */
	void ForgetSubject(FName Subject);

	/** Drops every engine and forgets the clock offset (a transport start or stop). */
	void Reset();

	int32 GetNumEngines() const { return Engines.Num(); }
	bool HasClockOffsetEstimate() const { return bHasClockOffsetEstimate; }

private:
	O3DS::ConcealmentEngine& GetOrCreateEngine(const UO3DReceiverSourceSettings* Settings, FName Subject);
	void ReportMetricsDelta();

	TMap<FName, TUniquePtr<O3DS::ConcealmentEngine>> Engines;
	/** Last-reported metrics per subject, for delta reporting into the shared metrics. */
	TMap<FName, O3DS::ConcealmentMetrics> PrevMetricsBySubject;

	/**
	 * Cached from the gated path's latest clock Observe(). A real frame's presentation time is
	 * already in the FPlatformTime::Seconds() domain, so Tick's "now" needs no offset: the value is
	 * kept only as the payload of bHasClockOffsetEstimate ("the gated path has observed a frame").
	 */
	int64 LastClockOffsetEstimateUs = 0;
	bool bHasClockOffsetEstimate = false;

	// Scratch for a synthesized frame, reused across subjects and ticks (RCV-11).
	TArray<FTransform> SyntheticTransforms;
	TArray<float> SyntheticCurveValues;
};
