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

#ifndef O3DS_QUANT_CHANNEL_QUANT_H
#define O3DS_QUANT_CHANNEL_QUANT_H

#include <cstdint>
#include "../predict/quat_math.h"

// Adaptive/variable-precision channel quantization (roadmap doc, Workstream
// D/D1). Pure math, FlatBuffers- and wire-format-agnostic - deliberately
// mirrors predict/residual_codec.h's separation of "the algorithm" from "how
// it's put on the wire" (see model.cpp/o3ds.fbs for the wire integration).
//
// Two independent quantization schemes live here:
//  - Scalar delta quantization (translation/scale components, curves): the
//    VALUE BEING QUANTIZED must already be a bounded delta (e.g. actual minus
//    a reference both sides already know - the legacy last-sent value, or
//    C2's ResidualEncoder::Reference()) - world-space absolutes are not
//    bounded and are not what this quantizes.
//  - Smallest-three quaternion quantization (rotation): quantizes the
//    ABSOLUTE rotation directly, since unit-quaternion components are
//    inherently bounded to [-1, 1] - no caller-supplied range needed.
namespace O3DS
{
	//! Precision tier chosen per channel, self-describing on the wire
	//! (SubjectUpdate's new quantized vectors, one per tier). 0 == Full
	//! deliberately matches the "0 == legacy/unquantized" convention already
	//! used for predictor_id (see O3DS::ResidualPredictorId) - never encode
	//! a real quantized tier as 0.
	enum class QuantTier : uint8_t
	{
		Full = 0, //!< No quantization; today's full float32, unconditionally lossless.
		Half = 1, //!< 16-bit fixed-point.
		Byte = 2, //!< 8-bit fixed-point.
	};

	//! Configurable bounds (in the value's own units) for the Byte/Half
	//! tiers of scalar delta quantization. A delta whose magnitude exceeds
	//! halfRange always falls back to QuantTier::Full - the "large/fast
	//! movement -> full float ceiling" safety net the roadmap calls for, so
	//! there is never a case where a channel's precision is *worse* than
	//! today's unconditional float32.
	struct QuantRanges
	{
		double byteRange = 0.01; //!< Max |delta| representable at Byte tier.
		double halfRange = 1.0;  //!< Max |delta| representable at Half tier.

		//! Fractional hysteresis margin (e.g. 0.15 == 15%) applied around
		//! each tier boundary by ChooseScalarTierWithHysteresis. Without it,
		//! a value that naturally hovers near byteRange/halfRange (e.g.
		//! idle-animation sway oscillating around a roughly constant
		//! amplitude) flips tiers every time it crosses back and forth -
		//! and since each tier reconstructs on a different rounding grid,
		//! every flip is a visible jump on the receiver, not just added
		//! noise. 0 (or a non-finite value) disables hysteresis, matching
		//! plain ChooseScalarTier exactly.
		double hysteresisFactor = 0.15;
	};

	//! Chooses the smallest tier that can represent absDelta (already
	//! std::abs'd by the caller) within ranges, without clamping/losing
	//! range - Full if absDelta exceeds halfRange. Stateless: the same
	//! absDelta always yields the same tier, regardless of what was chosen
	//! last time - see ChooseScalarTierWithHysteresis below for a
	//! flap-resistant alternative driven by a per-channel previous tier.
	QuantTier ChooseScalarTier(double absDelta, const QuantRanges& ranges);

	//! Hysteresis-aware tier selection for a channel whose previously-chosen
	//! tier is known. Only moves away from previousTier once absDelta clears
	//! the boundary on the *opposite* side by ranges.hysteresisFactor,
	//! instead of the instant the plain boundary is crossed - see
	//! QuantRanges::hysteresisFactor's doc comment for the flapping problem
	//! this avoids. A channel with no meaningful prior choice (e.g. a
	//! freshly-created Transform) should pass QuantTier::Full, the always-
	//! safe/correct starting point already used elsewhere in this scheme
	//! (e.g. "no anchor yet" falls back to Full too) - hysteresis then
	//! applies normally from there and converges within one call.
	QuantTier ChooseScalarTierWithHysteresis(double absDelta, const QuantRanges& ranges, QuantTier previousTier);

	//! Quantizes delta (already known to satisfy |delta| <= range - callers
	//! choose range via ChooseScalarTier first) to a signed 8-bit code.
	//! Clamps defensively if called with an out-of-range delta rather than
	//! wrapping/overflowing.
	int8_t QuantizeByte(double delta, double range);
	double DequantizeByte(int8_t code, double range);

	//! 16-bit counterpart of QuantizeByte/DequantizeByte.
	int16_t QuantizeHalf(double delta, double range);
	double DequantizeHalf(int16_t code, double range);

	//! Smallest-three quaternion quantization: drops the largest-magnitude
	//! component (reconstructed on decode from the unit-length constraint),
	//! storing which axis was dropped (0=x,1=y,2=z,3=w) plus the other three
	//! components quantized against the standard 1/sqrt(2) bound (see
	//! channel_quant.cpp for the derivation). If the dropped component's
	//! original sign was negative, the whole quaternion is negated first
	//! (q and -q represent the same rotation) so the dropped component's
	//! reconstructed sign (always positive) matches.
	struct SmallestThreeQ8
	{
		uint8_t droppedIndex = 3;
		int8_t a = 0, b = 0, c = 0; //!< The other three components, in ascending index order.
	};

	struct SmallestThreeQ16
	{
		uint8_t droppedIndex = 3;
		int16_t a = 0, b = 0, c = 0;
	};

	SmallestThreeQ8 QuantizeRotationByte(const Quat& q);
	Quat DequantizeRotationByte(const SmallestThreeQ8& q);

	SmallestThreeQ16 QuantizeRotationHalf(const Quat& q);
	Quat DequantizeRotationHalf(const SmallestThreeQ16& q);
}

#endif
