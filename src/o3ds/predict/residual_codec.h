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
	//! Usage per frame: BeginFrame(actual) - decides keyframe-vs-residual
	//! and exposes Reference(); the caller diffs `actual` against
	//! Reference() per channel (same deltaThreshold-gated convention it
	//! already uses today) to decide what to put on the wire - THEN calls
	//! Commit(reconstructedPose), where reconstructedPose is the exact
	//! pose a receiver will end up with: Reference()'s value for every
	//! channel the caller decided NOT to send, `actual`'s value for every
	//! channel it did send. This "local decode" (observe what the
	//! receiver will reconstruct, not the true actual value) is required,
	//! not an optimization detail: if the encoder instead observed
	//! `actual` directly, its predictor's history would silently diverge
	//! from the receiver's the very first time a channel's residual fell
	//! under deltaThreshold and got omitted (the receiver only ever knows
	//! about the channels it was actually told about, plus its own
	//! Reference() for the rest) - and once the two sides' histories
	//! differ, EVERY later Reference() differs too, so even a
	//! subsequently-*sent* channel reconstructs wrong (predicted+residual
	//! only equals actual when both sides computed the identical
	//! prediction). Observing reconstructedPose instead keeps the two
	//! sides' predictor state bit-for-bit in lockstep by construction.
	class ResidualEncoder
	{
	public:
		//! `id` must not be ResidualPredictorId::None - legacy mode means
		//! "don't use this class at all", not "construct one that does
		//! nothing" (there is no meaningful predictor to back it).
		explicit ResidualEncoder(ResidualPredictorId id, uint32_t keyframeIntervalFrames = 0);

		//! Phase 1 of the frame: decides keyframe-vs-residual using
		//! history committed so far (this frame's own values must not
		//! affect that decision - Predict() is evaluated before this
		//! frame is Commit()'d, never after). `actual`'s channel counts
		//! are also compared against the last Commit()'d pose - a
		//! mismatch (a bone/curve added or removed) would otherwise let a
		//! stale Reference() get indexed against the wrong channel under
		//! the new topology, so it forces a fresh start (predictor
		//! Reset(), this frame reported as a keyframe) instead.
		void BeginFrame(const PoseSample& actual);

		//! True when this frame's reference is zero/identity (i.e. every
		//! channel's wire value is its absolute value, not a residual) -
		//! because the predictor doesn't have enough history yet, a
		//! topology change was just detected, or keyframeIntervalFrames
		//! elapsed and a re-anchor is due (roadmap's "periodic keyframes
		//! bound drift, enable join-in-progress, and re-anchor after
		//! loss"). The caller must send EVERY channel unconditionally on
		//! a keyframe (bypassing its own deltaThreshold gate) - a keyframe
		//! exists to fully resync state, and a channel omitted from one
		//! (because it happened to be near the zero/identity reference)
		//! would defeat that for a receiver joining mid-stream or
		//! recovering from loss.
		bool IsKeyframe() const { return mIsKeyframe; }

		//! Valid only between BeginFrame() and Commit() calls. Empty
		//! (default PoseSample, no channels) when IsKeyframe() - the
		//! caller must not index into it in that case; treat every
		//! channel's reference as zero/identity instead (see class
		//! comment), and must send every channel (see IsKeyframe()).
		const PoseSample& Reference() const { return mReference; }

		//! Phase 2: advances the predictor's history with
		//! `reconstructedPose` (see class comment for why this must be
		//! the reconstructed pose, not the true actual one) and remembers
		//! its channel counts for the next BeginFrame()'s topology check.
		//! Call exactly once per frame, after BeginFrame(), in
		//! non-decreasing `reconstructedPose.t` order (same precondition
		//! as IPosePredictor::Observe()).
		void Commit(const PoseSample& reconstructedPose);

		ResidualPredictorId Id() const { return mId; }

		//! Clears the underlying predictor's history and topology
		//! tracking: call on a topology change detected by the caller
		//! ahead of BeginFrame() (ties to the same triggers as
		//! IPosePredictor::Reset()) - BeginFrame() also detects and
		//! handles this automatically via its own channel-count check, so
		//! calling this explicitly is only needed if the caller wants to
		//! force a reset the frame it happens rather than the frame after.
		void Reset();

	private:
		ResidualPredictorId mId;
		std::unique_ptr<IPosePredictor> mPredictor;
		uint32_t mKeyframeIntervalFrames;
		uint32_t mFramesSinceKeyframe = 0;
		bool mIsKeyframe = true;
		PoseSample mReference;
		bool mHasCommitted = false;
		size_t mLastTranslationCount = 0;
		size_t mLastRotationCount = 0;
		size_t mLastCurveCount = 0;
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
