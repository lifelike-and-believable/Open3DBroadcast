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

#include "concealment.h"

#include <algorithm>

namespace O3DS
{
	namespace
	{
		double AggregateTranslationDistance(const std::vector<Vector3d>& a, const std::vector<Vector3d>& b)
		{
			const size_t n = std::min(a.size(), b.size());
			if (n == 0) return 0.0;

			double sum = 0.0;
			for (size_t i = 0; i < n; ++i) sum += dist(a[i], b[i]);
			return sum / (double)n;
		}

		double AggregateRotationAngleRadians(const std::vector<Quat>& a, const std::vector<Quat>& b)
		{
			const size_t n = std::min(a.size(), b.size());
			if (n == 0) return 0.0;

			double sum = 0.0;
			for (size_t i = 0; i < n; ++i)
			{
				const Quat dq = QuatMultiply(b[i], QuatConjugate(a[i]));
				Vector3d axis;
				double angle = 0.0;
				if (QuatToAxisAngle(dq, axis, angle)) sum += angle;
			}
			return sum / (double)n;
		}
	}

	PoseSample BlendPoseSample(const PoseSample& from, const PoseSample& to, double alpha, double outTime, uint64_t outSeq)
	{
		PoseSample result;
		result.t = outTime;
		result.seq = outSeq;

		const size_t nTrans = std::min(from.translations.size(), to.translations.size());
		result.translations.resize(nTrans);
		for (size_t i = 0; i < nTrans; ++i)
		{
			for (int c = 0; c < 3; ++c)
			{
				result.translations[i].v[c] = from.translations[i].v[c] + (to.translations[i].v[c] - from.translations[i].v[c]) * alpha;
			}
		}

		const size_t nScale = std::min(from.scales.size(), to.scales.size());
		result.scales.resize(nScale);
		for (size_t i = 0; i < nScale; ++i)
		{
			for (int c = 0; c < 3; ++c)
			{
				result.scales[i].v[c] = from.scales[i].v[c] + (to.scales[i].v[c] - from.scales[i].v[c]) * alpha;
			}
		}

		const size_t nCurves = std::min(from.curves.size(), to.curves.size());
		result.curves.resize(nCurves);
		for (size_t i = 0; i < nCurves; ++i)
		{
			result.curves[i] = static_cast<float>(from.curves[i] + (to.curves[i] - from.curves[i]) * alpha);
		}

		const size_t nRot = std::min(from.rotations.size(), to.rotations.size());
		result.rotations.resize(nRot);
		for (size_t i = 0; i < nRot; ++i)
		{
			result.rotations[i] = QuatSlerpShortestPath(from.rotations[i], to.rotations[i], alpha);
		}

		return result;
	}

	ConcealmentEngine::ConcealmentEngine(std::unique_ptr<IPosePredictor> predictor, const ConcealmentConfig& config)
		: mPredictor(std::move(predictor))
		, mConfig(config)
	{
	}

	void ConcealmentEngine::RecordRecovery(const PoseSample& real)
	{
		++mMetrics.recoveryCount;

		// Compare what the predictor (built from history strictly before this
		// arrival) would have said for this exact time against what actually
		// arrived - the direct "predicted vs actual" comparison C1.d asks
		// for. Uses the predictor directly rather than TryConceal() so this
		// doesn't perturb concealment/correction bookkeeping.
		PoseSample predicted;
		if (mPredictor->Predict(real.t, predicted))
		{
			const double transErr = AggregateTranslationDistance(predicted.translations, real.translations);
			const double rotErr = AggregateRotationAngleRadians(predicted.rotations, real.rotations);
			mMetrics.predictionTranslationErrorSum += transErr;
			mMetrics.predictionTranslationErrorMax = std::max(mMetrics.predictionTranslationErrorMax, transErr);
			mMetrics.predictionRotationErrorRadiansSum += rotErr;
			mMetrics.predictionRotationErrorRadiansMax = std::max(mMetrics.predictionRotationErrorRadiansMax, rotErr);
		}

		// Raw (pre-correction) discontinuity between what was last shown and
		// the newly-arrived real pose - what concealment is meant to reduce
		// vs Hold; comparable across predictors on the same dropped-frame set.
		if (mHasLastOutput)
		{
			const double popTrans = AggregateTranslationDistance(mLastOutput.translations, real.translations);
			const double popRot = AggregateRotationAngleRadians(mLastOutput.rotations, real.rotations);
			mMetrics.popTranslationSum += popTrans;
			mMetrics.popTranslationMax = std::max(mMetrics.popTranslationMax, popTrans);
			mMetrics.popRotationRadiansSum += popRot;
			mMetrics.popRotationRadiansMax = std::max(mMetrics.popRotationRadiansMax, popRot);
		}

		if (mConcealing)
		{
			const double duration = real.t - mConcealStartTime;
			mMetrics.concealedTimeSecondsTotal += duration;
			mMetrics.concealedTimeSecondsMax = std::max(mMetrics.concealedTimeSecondsMax, duration);
		}
	}

	void ConcealmentEngine::ObserveRealFrame(const PoseSample& sample)
	{
		// Gate on mConcealing specifically (not mCorrecting too): mCorrecting
		// stays true across every ordinary real frame that arrives during
		// the post-recovery blend window (it's only cleared by a TryConceal()
		// poll once the window elapses), so using it here would re-record a
		// "recovery" and restart the correction window on every subsequent
		// normal frame instead of just the one that actually ended the gap.
		const bool endingConcealment = mConcealing;
		if (endingConcealment)
		{
			RecordRecovery(sample);
		}

		mPredictor->Observe(sample);
		mLastReal = sample;
		mHasHistory = true;
		mConcealing = false;

		if (endingConcealment)
		{
			mCorrecting = true;
			mCorrectionStartTime = sample.t;
			mCorrectionBase = mHasLastOutput ? mLastOutput : sample;
		}
	}

	bool ConcealmentEngine::TryConceal(double tNow, PoseSample& outPose)
	{
		if (!mHasHistory) return false;

		const double gap = tNow - mLastReal.t;

		if (mCorrecting)
		{
			const double elapsed = tNow - mCorrectionStartTime;
			if (elapsed >= mConfig.correctionWindowSeconds || gap > mConfig.starvationThresholdSeconds)
			{
				mCorrecting = false;
			}
			else
			{
				const double alpha = mConfig.correctionWindowSeconds > 0.0
					? std::min(1.0, elapsed / mConfig.correctionWindowSeconds)
					: 1.0;
				outPose = BlendPoseSample(mCorrectionBase, mLastReal, alpha, tNow, mLastReal.seq);
				mLastOutput = outPose;
				mHasLastOutput = true;
				++mMetrics.correctionFrameCount;
				return true;
			}
		}

		if (gap <= mConfig.starvationThresholdSeconds)
		{
			// Real data covers this instant closely enough - let the
			// caller's normal presentation path (e.g. LiveLink's own
			// interpolation) handle it (gap-triggered trigger model, C1.b).
			return false;
		}

		if (!mConcealing)
		{
			mConcealing = true;
			mConcealStartTime = tNow;
		}

		const double concealedFor = tNow - mConcealStartTime;
		PoseSample predicted;
		bool held = false;
		if (concealedFor > mConfig.maxConcealHorizonSeconds || !mPredictor->Predict(tNow, predicted))
		{
			// Bound the horizon, or insufficient predictor history: hold
			// rather than extrapolate further (C1.a).
			predicted = mHasLastOutput ? mLastOutput : mLastReal;
			predicted.t = tNow;
			held = true;
		}

		outPose = predicted;
		mLastOutput = outPose;
		mHasLastOutput = true;

		++mMetrics.concealedFrameCount;
		if (held) ++mMetrics.fallbackHoldCount;

		return true;
	}

	void ConcealmentEngine::Reset()
	{
		mPredictor->Reset();
		mHasHistory = false;
		mLastReal = PoseSample();
		mConcealing = false;
		mConcealStartTime = 0.0;
		mCorrecting = false;
		mCorrectionStartTime = 0.0;
		mCorrectionBase = PoseSample();
		mLastOutput = PoseSample();
		mHasLastOutput = false;
		// Metrics deliberately persist across Reset() - they're a
		// session-level HUD signal (C1.d), not per-topology-epoch state.
	}
}
