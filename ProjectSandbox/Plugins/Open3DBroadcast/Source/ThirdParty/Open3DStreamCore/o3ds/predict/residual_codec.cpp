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

#include "residual_codec.h"

#include "hold_predictor.h"
#include "linear_predictor.h"
#include "quadratic_predictor.h"

namespace O3DS
{
	std::unique_ptr<IPosePredictor> MakePredictorForId(ResidualPredictorId id)
	{
		switch (id)
		{
		case ResidualPredictorId::Hold:      return std::make_unique<HoldPredictor>();
		case ResidualPredictorId::Linear:    return std::make_unique<LinearPredictor>();
		case ResidualPredictorId::Quadratic: return std::make_unique<QuadraticPredictor>();
		case ResidualPredictorId::None:
		default:
			return nullptr;
		}
	}

	ResidualEncoder::ResidualEncoder(ResidualPredictorId id, uint32_t keyframeIntervalFrames)
		: mId(id)
		, mPredictor(MakePredictorForId(id))
		, mKeyframeIntervalFrames(keyframeIntervalFrames)
	{
	}

	void ResidualEncoder::BeginFrame(const PoseSample& actual)
	{
		// A channel-count mismatch vs the last Commit()'d pose means the
		// topology changed (a bone/curve was added or removed) - the
		// predictor's history is indexed by the OLD topology, so a stale
		// Reference() would silently get applied to the wrong channel
		// under the new one. Treat it exactly like fresh construction:
		// drop history and force this frame to a keyframe.
		const bool topologyChanged = mHasCommitted &&
			(actual.translations.size() != mLastTranslationCount ||
			 actual.rotations.size() != mLastRotationCount ||
			 actual.curves.size() != mLastCurveCount);
		if (topologyChanged)
		{
			mPredictor->Reset();
			mHasCommitted = false;
		}

		PoseSample predicted;
		const bool hasPrediction = !topologyChanged && mPredictor->Predict(actual.t, predicted);
		const bool cadenceElapsed = (mKeyframeIntervalFrames > 0) && (mFramesSinceKeyframe >= mKeyframeIntervalFrames);

		mIsKeyframe = !hasPrediction || cadenceElapsed;
		mReference = mIsKeyframe ? PoseSample() : predicted;
		mFramesSinceKeyframe = mIsKeyframe ? 0 : (mFramesSinceKeyframe + 1);
	}

	void ResidualEncoder::Commit(const PoseSample& reconstructedPose)
	{
		mPredictor->Observe(reconstructedPose);
		mHasCommitted = true;
		mLastTranslationCount = reconstructedPose.translations.size();
		mLastRotationCount = reconstructedPose.rotations.size();
		mLastCurveCount = reconstructedPose.curves.size();
	}

	void ResidualEncoder::Reset()
	{
		mPredictor->Reset();
		mIsKeyframe = true;
		mReference = PoseSample();
		mFramesSinceKeyframe = 0;
		mHasCommitted = false;
	}

	ResidualDecoder::ResidualDecoder(ResidualPredictorId id)
		: mId(id)
		, mPredictor(MakePredictorForId(id))
	{
	}

	void ResidualDecoder::BeginFrame(bool incomingIsKeyframe, double t)
	{
		if (incomingIsKeyframe)
		{
			mIsKeyframe = true;
			mReference = PoseSample();
			return;
		}

		PoseSample predicted;
		if (!mPredictor->Predict(t, predicted))
		{
			// Sender thought a residual was safe, but this decoder's own
			// history doesn't support a prediction yet (e.g. just Reset(),
			// or joined mid-stream) - fall back to a zero/identity
			// reference for this one frame rather than reconstructing
			// against garbage. The next real keyframe re-anchors it.
			mIsKeyframe = true;
			mReference = PoseSample();
			return;
		}

		mIsKeyframe = false;
		mReference = predicted;
	}

	void ResidualDecoder::EndFrame(const PoseSample& fullyReconstructedPose)
	{
		mPredictor->Observe(fullyReconstructedPose);
	}

	void ResidualDecoder::Reset()
	{
		mPredictor->Reset();
		mIsKeyframe = true;
		mReference = PoseSample();
	}
}
