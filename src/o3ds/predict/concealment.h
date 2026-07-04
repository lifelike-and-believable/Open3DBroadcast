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
	};

	//! Running counters/aggregates for the C1.d HUD metrics. "Prediction
	//! error" and "pop" are reported as separate translation (scene units)
	//! and rotation (radians) RMS-style aggregates rather than one combined
	//! number, since the two use different units; both are recorded once
	//! per concealment-span recovery (when a real frame ends a gap this
	//! engine was concealing/correcting for).
	struct ConcealmentMetrics
	{
		uint64_t concealedFrameCount = 0;  //!< TryConceal() returned true (predicted or held)
		uint64_t fallbackHoldCount = 0;    //!< ...of which, held rather than freshly predicted (horizon exceeded or predictor lacks history)
		uint64_t correctionFrameCount = 0; //!< frames output during a post-recovery correction blend
		uint64_t recoveryCount = 0;        //!< concealment spans that ended in a real-frame recovery (each contributes one prediction-error + pop sample)

		double concealedTimeSecondsTotal = 0.0;
		double concealedTimeSecondsMax = 0.0; //!< longest single concealed span - the horizon-distribution signal from C1.d

		double predictionTranslationErrorSum = 0.0; //!< sum over recoveries, for MeanPredictionTranslationError()
		double predictionTranslationErrorMax = 0.0;
		double predictionRotationErrorRadiansSum = 0.0;
		double predictionRotationErrorRadiansMax = 0.0;

		double popTranslationSum = 0.0; //!< raw (pre-correction) discontinuity at recovery - the "pop" C1 concealment is meant to reduce vs Hold
		double popTranslationMax = 0.0;
		double popRotationRadiansSum = 0.0;
		double popRotationRadiansMax = 0.0;

		double MeanPredictionTranslationError() const { return recoveryCount ? predictionTranslationErrorSum / (double)recoveryCount : 0.0; }
		double MeanPredictionRotationErrorRadians() const { return recoveryCount ? predictionRotationErrorRadiansSum / (double)recoveryCount : 0.0; }
		double MeanPopTranslation() const { return recoveryCount ? popTranslationSum / (double)recoveryCount : 0.0; }
		double MeanPopRotationRadians() const { return recoveryCount ? popRotationRadiansSum / (double)recoveryCount : 0.0; }
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
