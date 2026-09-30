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

#ifndef O3DS_SENDER_SYNC_H
#define O3DS_SENDER_SYNC_H

// Sender-side wire policy helpers (WP-S3, ADR 0005). Pure logic with no
// FlatBuffers, UE or clock dependency, so the sender plugin and CTest share
// one implementation:
//   - ConsumeCaptureBudget: accumulator-based capture rate limiter (SND-5).
//   - FilterCurveValue: per-curve epsilon/delta send filter that still sends
//     a curve's return to zero once (SND-4).
//   - FullSyncTracker: per-subject "is a full Subject due?" state for the
//     persistent (residual/quantized) encodings (SND-1/2/3/13/14,
//     ADR 0005 (ii)).

#include "o3ds_export.h"
#include <cstdint>

namespace O3DS
{
	//! Default tolerance for ConsumeCaptureBudget: a tick up to this much
	//! earlier than the next capture slot still captures. Absorbs normal
	//! frame-time jitter at a tick rate equal to the capture rate.
	constexpr double kCaptureRateToleranceSeconds = 0.0005;

	//! Accumulator-based capture rate limiter (SND-5).
	//!
	//! `inOutLastSlot` is the scheduled time of the last accepted capture, not
	//! the tick time it happened on. Each accepted capture advances it by one
	//! interval (1 / rateHz), so the long-run capture rate equals rateHz even
	//! when individual ticks arrive slightly early or late. A tick is accepted
	//! when it is no more than `toleranceSeconds` (capped at half an interval)
	//! before the next slot. When the schedule falls one full interval or more
	//! behind (hitch, tick rate below the capture rate) or the clock jumps
	//! backwards by more than an interval, the slot re-anchors to `nowSeconds`
	//! so no burst of catch-up captures follows.
	//!
	//! `inOutLastSlot <= 0` means "no capture yet": the call captures and
	//! anchors to `nowSeconds`. `rateHz <= 0` (or NaN) disables limiting: every
	//! call captures and `inOutLastSlot` tracks `nowSeconds`.
	O3DS_API bool ConsumeCaptureBudget(double nowSeconds, double& inOutLastSlot, double rateHz,
		double toleranceSeconds = kCaptureRateToleranceSeconds);

	//! Per-curve send filter for the legacy (full-snapshot) encoding (SND-4).
	//!
	//! Returns true when the curve should be sent this frame. A value whose
	//! magnitude is below `epsilon` counts as zero: it is suppressed only if
	//! the last sent value was also zero (or nothing was sent yet). Otherwise
	//! it is sent once, snapped to exactly 0.0f through `inOutValue`, so a
	//! receiver never holds a stale non-zero value after the curve returns to
	//! rest. A non-zero value is suppressed when it differs from the last sent
	//! value by less than `deltaThreshold`.
	//!
	//! The caller records `inOutValue` as the new last-sent value whenever this
	//! returns true.
	O3DS_API bool FilterCurveValue(float& inOutValue, bool hasLastSent, float lastSent,
		float epsilon, float deltaThreshold);

	//! Inputs describing one frame of one subject, for FullSyncTracker.
	struct FullSyncInputs
	{
		//! Hash of the skeleton descriptor (bone names and parents).
		uint64_t descriptorHash = 0;
		//! Hash of the ordered curve NAME list (identity, not values or count).
		uint64_t curveNamesHash = 0;
		//! Fingerprint of the encoding settings whose change needs a fresh
		//! full sync (mode, predictor, keyframe interval, quantization ranges).
		uint64_t encodingFingerprint = 0;
		//! Monotonic sampling time of the frame, in seconds.
		double nowSeconds = 0.0;
		//! Periodic full-sync interval in seconds; <= 0 disables the periodic
		//! trigger.
		double intervalSeconds = 1.0;
	};

	//! Per-subject full-sync state for the persistent encodings (ADR 0005 (ii)).
	//!
	//! A full Subject is due when any of these holds: nothing has been sent
	//! yet (first frame after start, rename or Reset()), the descriptor hash
	//! changed, the curve name list changed, the encoding fingerprint changed,
	//! the periodic interval elapsed since the last full sync (or the clock
	//! went backwards), or RequestFullSync() was called (for example a
	//! transport reporting a new peer).
	class O3DS_API FullSyncTracker
	{
	public:
		enum Reason : uint32_t
		{
			None         = 0,
			First        = 1u << 0,
			Descriptor   = 1u << 1,
			CurveNames   = 1u << 2,
			Encoding     = 1u << 3,
			Interval     = 1u << 4,
			Requested    = 1u << 5,
		};

		//! Returns a bitmask of Reason values; 0 means an update may be sent.
		uint32_t Evaluate(const FullSyncInputs& in) const;

		//! Record that a full Subject was sent for `in`.
		void MarkFullSent(const FullSyncInputs& in);

		//! Force the next Evaluate() to report a full sync.
		void RequestFullSync() { mRequested = true; }

		//! Forget everything: the next Evaluate() reports First.
		void Reset();

		bool HasSentFull() const { return mHasSent; }
		double LastFullSyncSeconds() const { return mLastFullSeconds; }

	private:
		bool     mHasSent = false;
		bool     mRequested = false;
		uint64_t mDescriptorHash = 0;
		uint64_t mCurveNamesHash = 0;
		uint64_t mEncodingFingerprint = 0;
		double   mLastFullSeconds = 0.0;
	};
}

#endif
