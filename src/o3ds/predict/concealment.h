/*
Open 3D Stream

Copyright 2026 Alastair Macleod

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef O3DS_PREDICT_CONCEALMENT_H
#define O3DS_PREDICT_CONCEALMENT_H

#include <cstdint>
#include <memory>

#include "pose_predictor.h"

namespace O3DS
{
	//! Blends every channel of `from` toward `to` at `alpha` (0 = from, 1 =
	//! to): translations/scales/curves linearly, rotations via
	//! QuatSlerpShortestPath. Channel counts truncate to the common count
	//! across the two samples (see predictor implementations' doc comments
	//! on the topology-stability contract). `outTime`/`outSeq` are stamped
	//! directly onto the result rather than interpolated.
	PoseSample BlendPoseSample(const PoseSample& from, const PoseSample& to, double alpha, double outTime, uint64_t outSeq);

	//! Tunables for ConcealmentEngine (roadmap doc, §5/C1). All defaults are
	//! conservative starting points per C1's "Open decisions" - expose as
	//! cvars in the UE glue layer so they can be tuned per-deployment
	//! without a core rebuild.
	struct ConcealmentConfig
	{
		//! Gap (seconds) beyond the last real frame's time before this
		//! engine starts synthesizing. Below this, TryConceal() reports
		//! "no action needed" and the caller just lets its normal
		//! presentation path (e.g. LiveLink's own interpolation/hold) cover
		//! the small gap - the "gap-triggered" trigger model (C1.b, Option
		//! 1: recommended start, since LiveLink already handles 1-2 frame
		//! gaps gracefully).
		double starvationThresholdSeconds = 0.05;

		//! After this much continuous concealment with no real frame,
		//! stop extrapolating and hold the last output instead - classical
		//! predictors diverge/overshoot on long gaps; a frozen pose beats a
		//! flung skeleton (C1.a).
		double maxConcealHorizonSeconds = 0.15;

		//! When a real frame resumes after a concealed span, blend from the
		//! last synthesized output toward the resumed real trajectory over
		//! this many seconds instead of snapping, to avoid a visible "pop"
		//! (C1.b). Does not rewrite any already-applied frame - it only
		//! shapes what TryConceal() returns for the next little while.
		double correctionWindowSeconds = 0.10;

		//! C1.c: latency-hiding/render-ahead horizon (seconds) beyond the
		//! newest real frame's own timestamp. 0 (the default) disables
		//! render-ahead entirely - TryRenderAhead() always returns false.
		//! Opt-in and separate from the gap/horizon/correction tunables
		//! above: see TryRenderAhead()'s own doc comment for why it doesn't
		//! reuse that machinery.
		double renderAheadSeconds = 0.0;
	};

	//! Running counters/aggregates for the C1.d HUD metrics. "Prediction
	//! error" and "pop" are each the mean, over recoveries that produced a
	//! sample, of a per-node distance/angle magnitude (translation in scene
	//! units, rotation in radians) - a mean of magnitudes, not a true RMS.
	//! Reported as separate translation/rotation numbers rather than one
	//! combined value, since the two use different units.
	struct ConcealmentMetrics
	{
		uint64_t concealedFrameCount = 0;  //!< TryConceal() returned true (predicted or held)
		uint64_t fallbackHoldCount = 0;    //!< ...of which, held rather than freshly predicted (horizon exceeded or predictor lacks history)
		uint64_t correctionFrameCount = 0; //!< frames output during a post-recovery correction blend
		uint64_t recoveryCount = 0;        //!< concealment spans that ended in a real-frame recovery

		//! Of recoveryCount, how many actually contributed a sample to the
		//! corresponding Mean*() below - kept separate from recoveryCount
		//! (rather than reusing it as the divisor) so a recovery where
		//! Predict() has no history yet, or there's no prior output to
		//! diff against, doesn't dilute the mean toward 0.
		uint64_t predictionSampleCount = 0;
		uint64_t popSampleCount = 0;

		double concealedTimeSecondsTotal = 0.0;
		double concealedTimeSecondsMax = 0.0; //!< longest single concealed span - the horizon-distribution signal from C1.d

		double predictionTranslationErrorSum = 0.0; //!< sum over predictionSampleCount, for MeanPredictionTranslationError()
		double predictionTranslationErrorMax = 0.0;
		double predictionRotationErrorRadiansSum = 0.0;
		double predictionRotationErrorRadiansMax = 0.0;

		double popTranslationSum = 0.0; //!< raw (pre-correction) discontinuity at recovery - the "pop" C1 concealment is meant to reduce vs Hold
		double popTranslationMax = 0.0;
		double popRotationRadiansSum = 0.0;
		double popRotationRadiansMax = 0.0;

		uint64_t renderAheadFrameCount = 0; //!< TryRenderAhead() produced a frame (C1.c, opt-in) - a simple counter, not error-scored like the loss-recovery metrics above

		double MeanPredictionTranslationError() const { return predictionSampleCount ? predictionTranslationErrorSum / (double)predictionSampleCount : 0.0; }
		double MeanPredictionRotationErrorRadians() const { return predictionSampleCount ? predictionRotationErrorRadiansSum / (double)predictionSampleCount : 0.0; }
		double MeanPopTranslation() const { return popSampleCount ? popTranslationSum / (double)popSampleCount : 0.0; }
		double MeanPopRotationRadians() const { return popSampleCount ? popRotationRadiansSum / (double)popSampleCount : 0.0; }
	};

	//! Per-subject receiver-side concealment state machine (roadmap doc,
	//! §5/C1.a-C1.d). Wraps an IPosePredictor with the gap-detection,
	//! horizon-bounding, and post-recovery correction-blend logic needed to
	//! turn "predict on request" into "what should I show right now."
	//!
	//! Usage: call ObserveRealFrame() on every applied real frame (from the
	//! A1/A2 reorder-gate apply path), and call TryConceal() once per
	//! presentation instant with the current target time. If it returns
	//! true, push outPose as a synthetic frame; if false, the caller's
	//! normal real-frame path already covers this instant (either a real
	//! frame just arrived, or there isn't enough history to do anything -
	//! in the latter case the caller should hold whatever it last had, same
	//! as today).
	//!
	//! Not thread-safe; matches IPosePredictor's own single-thread contract.
	class ConcealmentEngine
	{
	public:
		explicit ConcealmentEngine(std::unique_ptr<IPosePredictor> predictor, const ConcealmentConfig& config = ConcealmentConfig());

		//! Feed one confirmed real frame. Scores any in-flight
		//! concealment/correction span against this arrival (prediction
		//! error + pop metrics), then ends it and starts a correction blend
		//! for the next `correctionWindowSeconds`.
		void ObserveRealFrame(const PoseSample& sample);

		//! Evaluate presentation time `tNow` - same clock domain as
		//! PoseSample::t (the sender clock, per A2.b's mapping into local
		//! time), not the receiver's wall clock; mixing domains silently
		//! breaks every gap/horizon/window comparison below. Returns false
		//! (outPose left untouched) when real data already covers this
		//! instant closely enough, or when there's no history at all yet.
		//! Returns true and populates outPose (predicted, held, or
		//! correction-blended) otherwise.
		bool TryConceal(double tNow, PoseSample& outPose);

		//! Clears history (predictor + internal state): call on keyframe /
		//! topology change / version mismatch / large sequence gap, same
		//! triggers as IPosePredictor::Reset() (ties to A1.c re-baseline).
		void Reset();

		//! C1.c: latency-hiding/render-ahead (opt-in, disabled by default via
		//! ConcealmentConfig::renderAheadSeconds == 0). Proactively predicts
		//! renderAheadSeconds beyond the newest real frame's own timestamp,
		//! regardless of whether an actual gap has opened - unlike
		//! TryConceal(), which only engages past starvationThresholdSeconds.
		//!
		//! Call this only when TryConceal() returned false for the same
		//! presentation instant (i.e. real data is flowing close to
		//! on-time): a genuine loss/lateness event is already better served
		//! by TryConceal()'s horizon-bounded, correction-blended handling.
		//! This method deliberately never touches mConcealing/mCorrecting or
		//! the loss-recovery metrics above - folding routine render-ahead
		//! operation into that bookkeeping would misreport nearly every real
		//! frame arrival as a "recovery" (render-ahead's gap, measured
		//! against mLastReal, is by design almost always larger than
		//! starvationThresholdSeconds) and corrupt the prediction-error/pop
		//! metrics C1.d relies on to measure actual gap-recovery quality.
		//!
		//! Deliberately takes no `tNow`/presentation-clock argument: the
		//! target is always relative to the newest real frame's own
		//! timestamp, never to the receiver's mapped presentation time -
		//! this is what keeps it from double-counting A2.c's LiveLink
		//! buffer offset (the roadmap's C1.c warning): there is no
		//! presentation-clock offset here to double-count in the first
		//! place.
		//!
		//! Returns false (outPose untouched) when render-ahead is disabled
		//! (renderAheadSeconds <= 0) or there's no real-frame history yet.
		bool TryRenderAhead(PoseSample& outPose);

		const ConcealmentMetrics& Metrics() const { return mMetrics; }

	private:
		std::unique_ptr<IPosePredictor> mPredictor;
		ConcealmentConfig mConfig;
		ConcealmentMetrics mMetrics;

		bool mHasHistory = false;
		PoseSample mLastReal;

		bool mConcealing = false;
		double mConcealStartTime = 0.0;

		bool mCorrecting = false;
		double mCorrectionStartTime = 0.0;
		PoseSample mCorrectionBase;

		PoseSample mLastOutput;
		bool mHasLastOutput = false;

		void RecordRecovery(const PoseSample& real);
	};
}

#endif
