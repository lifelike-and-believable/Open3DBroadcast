// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DReceiverConcealment.h"

#include "O3DPerformanceMetrics.h"
#include "O3DReceiverSourceSettings.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/predict/linear_predictor.h"
THIRD_PARTY_INCLUDES_END

namespace O3DReceiverConcealmentPrivate
{
	// PoseSample <-> LiveLink transform/curve conversion. PoseSample stores rotations as (x,y,z,w)
	// doubles (O3DS::Quat = Vector4d), matching FQuat's own component order, so no component
	// reshuffling is needed.
	O3DS::PoseSample BuildPoseSampleFromLiveLink(double PresentationTimeSeconds, const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues)
	{
		O3DS::PoseSample Sample;
		Sample.t = PresentationTimeSeconds;

		Sample.translations.reserve(BoneTransforms.Num());
		Sample.rotations.reserve(BoneTransforms.Num());
		Sample.scales.reserve(BoneTransforms.Num());
		for (const FTransform& Xform : BoneTransforms)
		{
			const FVector Loc = Xform.GetLocation();
			const FQuat Rot = Xform.GetRotation();
			const FVector Scale = Xform.GetScale3D();
			Sample.translations.emplace_back((double)Loc.X, (double)Loc.Y, (double)Loc.Z);
			Sample.rotations.emplace_back((double)Rot.X, (double)Rot.Y, (double)Rot.Z, (double)Rot.W);
			Sample.scales.emplace_back((double)Scale.X, (double)Scale.Y, (double)Scale.Z);
		}

		Sample.curves.reserve(CurveValues.Num());
		for (float Value : CurveValues)
		{
			Sample.curves.push_back(Value);
		}

		return Sample;
	}

	// Inverse of BuildPoseSampleFromLiveLink, for pushing a synthesized (predicted, held or
	// corrected) pose back through the same LiveLink path a real frame takes. Scales default to 1
	// (as for a real frame without a scale channel) if the sample has fewer scale entries than
	// translations and rotations - PoseSample channel counts are stable within an epoch, but this
	// avoids reading out of bounds if they ever are not.
	void ApplyPoseSampleToLiveLink(const O3DS::PoseSample& Sample, TArray<FTransform>& OutBoneTransforms, TArray<float>& OutCurveValues)
	{
		const int32 Count = static_cast<int32>(Sample.translations.size());
		OutBoneTransforms.Reset(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const O3DS::Vector3d& Translation = Sample.translations[(size_t)Index];
			const O3DS::Vector3d Scale = (Index < (int32)Sample.scales.size()) ? Sample.scales[(size_t)Index] : O3DS::Vector3d(1.0, 1.0, 1.0);

			FQuat Rot = FQuat::Identity;
			if (Index < (int32)Sample.rotations.size())
			{
				const O3DS::Quat& Q = Sample.rotations[(size_t)Index];
				Rot = FQuat(Q.v[0], Q.v[1], Q.v[2], Q.v[3]);
				Rot.Normalize();
			}

			const FVector Location(Translation.v[0], Translation.v[1], Translation.v[2]);
			const FVector ScaleVec(Scale.v[0], Scale.v[1], Scale.v[2]);
			OutBoneTransforms.Emplace(Rot, Location, ScaleVec);
		}

		OutCurveValues.Reset(static_cast<int32>(Sample.curves.size()));
		for (float Value : Sample.curves)
		{
			OutCurveValues.Add(Value);
		}
	}

	bool IsDisabled(const UO3DReceiverSourceSettings* Settings)
	{
		return Settings != nullptr && !Settings->bEnableConcealment;
	}
}

FO3DReceiverConcealment::FO3DReceiverConcealment(FO3DReceiverMetricsHandleRef InMetrics)
	: Metrics(MoveTemp(InMetrics))
{
}

/** Lazily creates a per-subject ConcealmentEngine on first use (roadmap doc §5/C1.a).
 *  LinearPredictor is the roadmap's recommended C1 default ("almost certainly" - see
 *  §5/C1's "Open decisions"); Quadratic may overshoot on longer horizons. */
O3DS::ConcealmentEngine& FO3DReceiverConcealment::GetOrCreateEngine(const UO3DReceiverSourceSettings* Settings, FName Subject)
{
	if (TUniquePtr<O3DS::ConcealmentEngine>* Existing = Engines.Find(Subject))
	{
		return **Existing;
	}

	O3DS::ConcealmentConfig Config;
	Config.starvationThresholdSeconds = FMath::Max(0.0, (double)(Settings ? Settings->StarvationThresholdMs : 50.0f) / 1000.0);
	Config.maxConcealHorizonSeconds = FMath::Max(0.0, (double)(Settings ? Settings->MaxHorizonMs : 150.0f) / 1000.0);
	Config.correctionWindowSeconds = FMath::Max(0.0, (double)(Settings ? Settings->CorrectionWindowMs : 100.0f) / 1000.0);
	Config.renderAheadSeconds = FMath::Max(0.0, (double)(Settings ? Settings->RenderAheadMs : 0.0f) / 1000.0);

	TUniquePtr<O3DS::ConcealmentEngine> NewEngine = MakeUnique<O3DS::ConcealmentEngine>(std::make_unique<O3DS::LinearPredictor>(), Config);
	O3DS::ConcealmentEngine& Ref = *NewEngine;
	Engines.Add(Subject, MoveTemp(NewEngine));
	return Ref;
}

void FO3DReceiverConcealment::ObserveRealFrame(const UO3DReceiverSourceSettings* Settings, FName Subject, double PresentationTimeSeconds,
	const TArray<FTransform>& BoneTransforms, const TArray<float>& CurveValues, bool bTopologyChanged)
{
	if (O3DReceiverConcealmentPrivate::IsDisabled(Settings))
	{
		return;
	}

	O3DS::ConcealmentEngine& Engine = GetOrCreateEngine(Settings, Subject);
	if (bTopologyChanged)
	{
		Engine.Reset();
	}

	Engine.ObserveRealFrame(O3DReceiverConcealmentPrivate::BuildPoseSampleFromLiveLink(PresentationTimeSeconds, BoneTransforms, CurveValues));
}

void FO3DReceiverConcealment::NoteClockOffset(int64 OffsetEstimateUs)
{
	LastClockOffsetEstimateUs = OffsetEstimateUs;
	bHasClockOffsetEstimate = true;
}

void FO3DReceiverConcealment::Tick(const UO3DReceiverSourceSettings* Settings, bool bCanPublish, double NowSeconds, FPushSyntheticFrame PushSyntheticFrame)
{
	if (!bCanPublish || !bHasClockOffsetEstimate || Engines.Num() == 0)
	{
		return;
	}
	if (O3DReceiverConcealmentPrivate::IsDisabled(Settings))
	{
		return;
	}

	for (TPair<FName, TUniquePtr<O3DS::ConcealmentEngine>>& Pair : Engines)
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}

		O3DS::PoseSample Predicted;
		if (!Pair.Value->TryConceal(NowSeconds, Predicted))
		{
			// No actual gap right now (real data is flowing on schedule) - C1.c latency-hiding only
			// applies on top of that healthy case (see TryRenderAhead()'s own doc comment on why it
			// must not run during a genuine TryConceal()-handled gap or correction). Opt-in, default
			// off: returns false immediately when disabled.
			if (!Pair.Value->TryRenderAhead(Predicted))
			{
				continue;
			}
		}

		O3DReceiverConcealmentPrivate::ApplyPoseSampleToLiveLink(Predicted, SyntheticTransforms, SyntheticCurveValues);
		if (SyntheticTransforms.Num() == 0)
		{
			continue;
		}

		PushSyntheticFrame(Pair.Key, SyntheticTransforms, SyntheticCurveValues, Predicted.t);
	}

	ReportMetricsDelta();
}

/** Report each subject's ConcealmentEngine stats as a delta against its last-reported
 *  snapshot (same convention as the gate's metric deltas), summed across subjects into the
 *  shared metrics singleton. The error/pop averages are a last-writer-wins gauge across
 *  subjects and sources, the same simplification as AvgClockOffsetMs. */
void FO3DReceiverConcealment::ReportMetricsDelta()
{
	auto Delta = [](uint64 NewVal, uint64 OldVal) -> uint64 { return NewVal >= OldVal ? (NewVal - OldVal) : 0; };

	uint64 DeltaConcealed = 0, DeltaFallback = 0, DeltaCorrection = 0, DeltaRecovery = 0, DeltaRenderAhead = 0;
	bool bHasAnyRecovery = false;
	double LastTransErr = 0.0, LastRotErrDeg = 0.0, LastPopTrans = 0.0, LastPopRotDeg = 0.0;

	for (TPair<FName, TUniquePtr<O3DS::ConcealmentEngine>>& Pair : Engines)
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}

		const O3DS::ConcealmentMetrics& Current = Pair.Value->Metrics();
		O3DS::ConcealmentMetrics& Prev = PrevMetricsBySubject.FindOrAdd(Pair.Key);

		DeltaConcealed += Delta(Current.concealedFrameCount, Prev.concealedFrameCount);
		DeltaFallback += Delta(Current.fallbackHoldCount, Prev.fallbackHoldCount);
		DeltaCorrection += Delta(Current.correctionFrameCount, Prev.correctionFrameCount);
		DeltaRecovery += Delta(Current.recoveryCount, Prev.recoveryCount);
		DeltaRenderAhead += Delta(Current.renderAheadFrameCount, Prev.renderAheadFrameCount);

		if (Current.recoveryCount > 0)
		{
			bHasAnyRecovery = true;
			LastTransErr = Current.MeanPredictionTranslationError();
			LastRotErrDeg = FMath::RadiansToDegrees(Current.MeanPredictionRotationErrorRadians());
			LastPopTrans = Current.MeanPopTranslation();
			LastPopRotDeg = FMath::RadiansToDegrees(Current.MeanPopRotationRadians());
		}

		Prev = Current;
	}

	if (DeltaConcealed) Metrics->RecordConcealedFrames(DeltaConcealed);
	if (DeltaFallback) Metrics->RecordConcealmentFallbackHolds(DeltaFallback);
	if (DeltaCorrection) Metrics->RecordConcealmentCorrectionFrames(DeltaCorrection);
	if (DeltaRecovery) Metrics->RecordConcealmentRecoveries(DeltaRecovery);
	if (DeltaRenderAhead) Metrics->RecordConcealmentRenderAheadFrames(DeltaRenderAhead);
	if (bHasAnyRecovery)
	{
		Metrics->SetConcealmentPredictionError(LastTransErr, LastRotErrDeg);
		Metrics->SetConcealmentPop(LastPopTrans, LastPopRotDeg);
	}
}

void FO3DReceiverConcealment::ForgetSubject(FName Subject)
{
	Engines.Remove(Subject);
	PrevMetricsBySubject.Remove(Subject);
}

void FO3DReceiverConcealment::Reset()
{
	// A publisher restart invalidates every subject's prediction history equally (C1.a's "Reset
	// the predictor on... publisher restart"), and the cached clock-offset estimate is stale too.
	Engines.Empty();
	PrevMetricsBySubject.Empty();
	bHasClockOffsetEstimate = false;
	LastClockOffsetEstimateUs = 0;
}
