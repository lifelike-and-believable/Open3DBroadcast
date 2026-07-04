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

#ifndef O3DS_PREDICT_RESIDUAL_CODEC_H
#define O3DS_PREDICT_RESIDUAL_CODEC_H

#include <cstdint>
#include <memory>

#include "pose_predictor.h"

namespace O3DS
{
	//! Wire tag for which predictor (if any) produced a residual-coded
	//! update (roadmap doc, §5/C2). Deliberately NOT the same enumeration
	//! as IPosePredictor::Version() (Hold=0/Linear=1/Quadratic=2): 0 here
	//! must unambiguously mean "no residual coding on this update" (the
	//! legacy last-pose-delta wire format, i.e. SubjectUpdate's
	//! translations/rotation/scale/curves are absolute values). If Hold's
	//! residual mode shared IPosePredictor::Version()'s 0, a Hold-based
	//! residual stream would be silently misread as legacy raw-absolute
	//! mode by anything that doesn't understand residual coding, corrupting
	//! playback instead of just failing to compress.
	enum class ResidualPredictorId : uint32_t
	{
		None = 0, // legacy - no residual coding, values are absolute (today's behavior)
		Hold = 1,
		Linear = 2,
		Quadratic = 3,
	};

	//! Constructs the predictor matching a wire tag, or nullptr for None
	//! (the caller should not construct a codec at all in that case - see
	//! ResidualEncoder's own constructor comment).
	std::unique_ptr<IPosePredictor> MakePredictorForId(ResidualPredictorId id);

	//! Sender-side residual coding on top of any IPosePredictor (roadmap
	//! doc §5/C2). One instance per subject, mirroring the per-subject
	//! convention already used for C1's ConcealmentEngine. With a
	//! HoldPredictor, this reduces *exactly* to today's last-pose delta
	//! scheme (Predict() == the last Observe()'d sample), which is what
	//! makes C2 a strict generalization rather than a parallel scheme -
	//! shipping Hold-based residual coding changes nothing observable on
	//! the wire vs today's plain delta path.
	//!
	//! Usage: call BeginFrame(actual) once per frame (not once per
	//! channel), then read IsKeyframe()/Reference() to decide, per
	//! channel, what to encode - reference channel value if !IsKeyframe(),
	//! or the zero/identity-relative absolute value if IsKeyframe() (the
	//! caller diffs `actual` against Reference() the same way it already
	//! diffs against a "last sent" value today, using the same
	//! deltaThreshold-gated convention - this class does not do that
	//! per-channel diffing itself, since the caller already owns that loop
	//! and its wire structures).
	class ResidualEncoder
	{
	public:
		//! `id` must not be ResidualPredictorId::None - legacy mode means
		//! "don't use this class at all", not "construct one that does
		//! nothing" (there is no meaningful predictor to back it).
		explicit ResidualEncoder(ResidualPredictorId id, uint32_t keyframeIntervalFrames = 0);

		//! Advances one frame: decides keyframe-vs-residual for the whole
		//! frame (a single decision/reference shared by every channel -
		//! never mixed per-channel within one frame) and observes `actual`
		//! into the underlying predictor's history. Call exactly once per
		//! frame, in non-decreasing `actual.t` order (same precondition as
		//! IPosePredictor::Observe()).
		void BeginFrame(const PoseSample& actual);

		//! True when this frame's reference is zero/identity (i.e. every
		//! channel's wire value is its absolute value, not a residual) -
		//! either because the predictor doesn't have enough history yet,
		//! or because keyframeIntervalFrames elapsed and a re-anchor is
		//! due (roadmap's "periodic keyframes bound drift, enable
		//! join-in-progress, and re-anchor after loss").
		bool IsKeyframe() const { return mIsKeyframe; }

		//! Valid only between BeginFrame() calls. Empty (default
		//! PoseSample, no channels) when IsKeyframe() - the caller must
		//! not index into it in that case; treat every channel's
		//! reference as zero/identity instead (see class comment).
		const PoseSample& Reference() const { return mReference; }

		ResidualPredictorId Id() const { return mId; }

	private:
		ResidualPredictorId mId;
		std::unique_ptr<IPosePredictor> mPredictor;
		uint32_t mKeyframeIntervalFrames;
		uint32_t mFramesSinceKeyframe = 0;
		bool mIsKeyframe = true;
		PoseSample mReference;
	};

	//! Receiver-side mirror of ResidualEncoder. One instance per subject.
	//! Must run the same predictor (same ResidualPredictorId) as the
	//! sender's ResidualEncoder for reconstruction to converge - the
	//! caller is responsible for (re)constructing a matching decoder
	//! whenever the wire's predictor_id changes (see model.cpp's
	//! ParseUpdateResidual for the concrete policy).
	class ResidualDecoder
	{
	public:
		explicit ResidualDecoder(ResidualPredictorId id);

		//! Call once per incoming update, before applying any of its
		//! channels (mirrors ResidualEncoder::BeginFrame(), frame-level
		//! not channel-level). `incomingIsKeyframe`/`t` come straight off
		//! the wire (SubjectUpdate::is_keyframe(), SubjectList::time()).
		//! Falls back to a zero/identity reference (IsKeyframe() forced
		//! true) when this decoder's own predictor lacks history yet
		//! (e.g. just constructed, or Reset() since) even if the wire
		//! said otherwise - same "insufficient history -> keyframe-like"
		//! fallback contract as the encoder side, so a receiver that
		//! joins mid-stream or just resynced degrades to one visibly
		//! "off" frame rather than reading garbage.
		void BeginFrame(bool incomingIsKeyframe, double t);

		bool IsKeyframe() const { return mIsKeyframe; }
		const PoseSample& Reference() const { return mReference; }

		//! Call once, after every channel in this update has been applied
		//! onto the subject's own state (i.e. the subject's full current
		//! pose - changed channels and carried-over unchanged ones alike
		//! - is now known), to advance the predictor's history for the
		//! next frame's Reference(). `fullyReconstructedPose` must be the
		//! complete pose, not just this frame's changed channels - same
		//! full-state contract IPosePredictor::Observe() already has.
		void EndFrame(const PoseSample& fullyReconstructedPose);

		ResidualPredictorId Id() const { return mId; }

		//! Clears the underlying predictor's history: call on a topology
		//! change (ties to the same triggers as IPosePredictor::Reset()
		//! and ConcealmentEngine::Reset()). The next BeginFrame() call
		//! naturally reports IsKeyframe() until fresh history accumulates.
		void Reset();

	private:
		ResidualPredictorId mId;
		std::unique_ptr<IPosePredictor> mPredictor;
		bool mIsKeyframe = true;
		PoseSample mReference;
	};
}

#endif
