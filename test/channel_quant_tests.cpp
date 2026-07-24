// Acceptance matrix for src/o3ds/quant/channel_quant.* (roadmap doc,
// Workstream D/D1). Pure math round-trip tests, independent of the
// FlatBuffers wire integration (see model.cpp/o3ds.fbs for that).
#include "test_framework.h"

#include "o3ds/quant/channel_quant.h"

#include <cmath>
#include <limits>

using namespace O3DS;

namespace
{
	bool NearlyEqual(double a, double b, double tol)
	{
		return std::abs(a - b) < tol;
	}

	// Angle (radians) between two unit quaternions, taking the shortest
	// path (q and -q represent the same rotation).
	double QuatAngleDelta(const Quat& a, const Quat& b)
	{
		double dot = a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3];
		dot = std::max(-1.0, std::min(1.0, std::fabs(dot)));
		return 2.0 * std::acos(dot);
	}
}

O3DS_TEST(ChooseScalarTier_PicksByteForSmallDelta)
{
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	O3DS_CHECK(ChooseScalarTier(0.001, ranges) == QuantTier::Byte);
	O3DS_CHECK(ChooseScalarTier(0.01, ranges) == QuantTier::Byte);
}

O3DS_TEST(ChooseScalarTier_PicksHalfForMediumDelta)
{
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	O3DS_CHECK(ChooseScalarTier(0.5, ranges) == QuantTier::Half);
	O3DS_CHECK(ChooseScalarTier(1.0, ranges) == QuantTier::Half);
}

O3DS_TEST(ChooseScalarTier_FallsBackToFullBeyondHalfRange)
{
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	O3DS_CHECK(ChooseScalarTier(1.0001, ranges) == QuantTier::Full);
	O3DS_CHECK(ChooseScalarTier(1000.0, ranges) == QuantTier::Full);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_MatchesPlainTierWhenFactorIsZero)
{
	// Regression guard for jitter reported from a live PIE test (idle
	// animations flapping between quantization tiers frame-to-frame):
	// with hysteresisFactor == 0, the hysteresis-aware selector must be
	// byte-for-byte identical to the stateless ChooseScalarTier, regardless
	// of previousTier.
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.0;

	double samples[] = { 0.001, 0.01, 0.5, 1.0, 1.0001, 1000.0 };
	QuantTier previousTiers[] = { QuantTier::Full, QuantTier::Half, QuantTier::Byte };
	for (double sample : samples)
	{
		for (QuantTier previous : previousTiers)
		{
			O3DS_CHECK(ChooseScalarTierWithHysteresis(sample, ranges, previous) == ChooseScalarTier(sample, ranges));
		}
	}
}

O3DS_TEST(ChooseScalarTierWithHysteresis_UpgradesToHalfImmediatelyAtByteRange)
{
	// Regression guard (Copilot review, PR #247): upgrades must happen at
	// the plain byteRange boundary, NOT a hysteresis-expanded one - a value
	// that already exceeds byteRange must never stay in Byte, since
	// QuantizeByte would then silently clamp it (byteRange is the max
	// |delta| Byte can represent WITHOUT clamping). Only downgrades get a
	// hysteresis margin - see QuantRanges::hysteresisFactor's doc comment.
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.011, ranges, QuantTier::Byte) == QuantTier::Half);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_UpgradesToHalfImmediatelyFurtherPastByteRange)
{
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.012, ranges, QuantTier::Byte) == QuantTier::Half);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_StaysInHalfWithinLowerBand)
{
	// contractedByteRange=0.0085. A delta just below byteRange but still
	// within the band should NOT flip Half -> Byte yet.
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.009, ranges, QuantTier::Half) == QuantTier::Half);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_DowngradesToBytePastLowerBand)
{
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.008, ranges, QuantTier::Half) == QuantTier::Byte);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_UpgradesToFullImmediatelyAtHalfRange)
{
	// Same regression guard as UpgradesToHalfImmediatelyAtByteRange, one
	// tier up: a value past halfRange must upgrade to Full immediately,
	// never stay in Half and risk QuantizeHalf clamping it.
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(1.1, ranges, QuantTier::Half) == QuantTier::Full);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_UpgradesToFullImmediatelyFurtherPastHalfRange)
{
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(1.2, ranges, QuantTier::Half) == QuantTier::Full);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_StaysInFullWithinLowerBand)
{
	// contractedHalfRange=0.85. A delta just below halfRange but still
	// within the band should NOT flip Full -> Half yet.
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.9, ranges, QuantTier::Full) == QuantTier::Full);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_DowngradesFromFullToHalfPastLowerBand)
{
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.8, ranges, QuantTier::Full) == QuantTier::Half);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_FreshChannelFromFullNeedsToClearContractedThreshold)
{
	// A "fresh" channel (no prior tier - see Transform::mLastTranslationTier's
	// default of QuantTier::Full) is slightly more conservative on its very
	// first evaluation than the stateless ChooseScalarTier: entering Byte
	// specifically (not just some smaller-range tier) needs to clear the
	// tighter contractedByteRange (0.0085), not just byteRange (0.01)
	// itself. A delta in between - comfortably "Byte territory" for the
	// plain function, but not quite past Full's Byte-entry margin - settles
	// into Half instead of Byte on this first call (it easily clears the
	// more lenient contractedHalfRange), and only reaches Byte once the
	// delta itself drops below contractedByteRange. This converges quickly
	// for any genuinely continuous signal (the next frame's previousTier
	// reflects whichever tier was just settled into) so it isn't a
	// user-visible regression - see StaysInFullWithinLowerBand above for
	// the "doesn't enter any smaller-range tier at all yet" case.
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.009, ranges, QuantTier::Full) == QuantTier::Half);
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.008, ranges, QuantTier::Full) == QuantTier::Byte);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_HugeJumpFromByteSkipsHalfGoesStraightToFull)
{
	// A genuinely large single-frame movement (e.g. a snap/teleport) must
	// still reach Full in one call, not get stuck needing two calls to pass
	// through Half first.
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(5.0, ranges, QuantTier::Byte) == QuantTier::Full);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_HugeDropFromFullSkipsHalfGoesStraightToByte)
{
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 0.15;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.0001, ranges, QuantTier::Full) == QuantTier::Byte);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_NonFiniteOrNegativeFactorDisablesHysteresis)
{
	// Mirrors QuantizeByte/QuantizeHalf's own non-finite-range guard - a
	// caller-supplied hysteresisFactor (UE UPROPERTY) could be NaN or
	// negative; both should behave exactly like plain ChooseScalarTier
	// rather than propagating garbage into the expanded/contracted math.
	const double nan = std::numeric_limits<double>::quiet_NaN();
	QuantRanges rangesNan;
	rangesNan.byteRange = 0.01;
	rangesNan.halfRange = 1.0;
	rangesNan.hysteresisFactor = nan;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.011, rangesNan, QuantTier::Byte) == QuantTier::Half);

	QuantRanges rangesNegative;
	rangesNegative.byteRange = 0.01;
	rangesNegative.halfRange = 1.0;
	rangesNegative.hysteresisFactor = -0.5;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.011, rangesNegative, QuantTier::Byte) == QuantTier::Half);
}

O3DS_TEST(ChooseScalarTierWithHysteresis_FactorAboveOneIsClampedNotDegenerate)
{
	// hysteresisFactor >= 1.0 would otherwise make contractedByteRange <= 0
	// (or negative), which could make every delta - even zero - unable to
	// clear the "enter Byte" threshold. Confirm it's clamped to something
	// still usable instead.
	QuantRanges ranges;
	ranges.byteRange = 0.01;
	ranges.halfRange = 1.0;
	ranges.hysteresisFactor = 5.0;
	O3DS_CHECK(ChooseScalarTierWithHysteresis(0.0, ranges, QuantTier::Full) == QuantTier::Byte);
}

O3DS_TEST(QuantizeByte_RoundTripsWithinExpectedResolution)
{
	const double range = 0.01;
	for (double v = -range; v <= range; v += range / 20.0)
	{
		int8_t code = QuantizeByte(v, range);
		double back = DequantizeByte(code, range);
		// Resolution at this range is range/127 per step.
		O3DS_CHECK(NearlyEqual(back, v, (range / 127.0) + 1.0e-12));
	}
}

O3DS_TEST(QuantizeByte_ClampsOutOfRangeInputInsteadOfOverflowing)
{
	const double range = 0.01;
	int8_t codeHigh = QuantizeByte(range * 100.0, range);
	int8_t codeLow = QuantizeByte(-range * 100.0, range);
	O3DS_CHECK(codeHigh == 127);
	O3DS_CHECK(codeLow == -127);
}

O3DS_TEST(QuantizeByte_NonFiniteRangeDisablesQuantizationInsteadOfProducingGarbage)
{
	// Regression (Copilot review): range can come from caller-supplied
	// config (e.g. a UE UPROPERTY) - only checking range<=0 lets a NaN
	// range slip through (NaN<=0 is false in IEEE 754), producing a
	// garbage saturated code from the division instead of safely
	// disabling quantization.
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();
	O3DS_CHECK(QuantizeByte(0.005, nan) == 0);
	O3DS_CHECK(QuantizeByte(0.005, inf) == 0);
	O3DS_CHECK(QuantizeByte(0.005, -inf) == 0);
}

O3DS_TEST(QuantizeByte_ZeroRoundTripsExactly)
{
	const double range = 0.01;
	O3DS_CHECK(QuantizeByte(0.0, range) == 0);
	O3DS_CHECK(NearlyEqual(DequantizeByte(0, range), 0.0, 1.0e-12));
}

O3DS_TEST(QuantizeHalf_RoundTripsWithinExpectedResolution)
{
	const double range = 1.0;
	for (double v = -range; v <= range; v += range / 50.0)
	{
		int16_t code = QuantizeHalf(v, range);
		double back = DequantizeHalf(code, range);
		O3DS_CHECK(NearlyEqual(back, v, (range / 32767.0) + 1.0e-12));
	}
}

O3DS_TEST(QuantizeHalf_ClampsOutOfRangeInputInsteadOfOverflowing)
{
	const double range = 1.0;
	int16_t codeHigh = QuantizeHalf(range * 100.0, range);
	int16_t codeLow = QuantizeHalf(-range * 100.0, range);
	O3DS_CHECK(codeHigh == 32767);
	O3DS_CHECK(codeLow == -32767);
}

O3DS_TEST(QuantizeHalf_NonFiniteRangeDisablesQuantizationInsteadOfProducingGarbage)
{
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();
	O3DS_CHECK(QuantizeHalf(0.5, nan) == 0);
	O3DS_CHECK(QuantizeHalf(0.5, inf) == 0);
	O3DS_CHECK(QuantizeHalf(0.5, -inf) == 0);
}

O3DS_TEST(QuantizeHalf_IsMeaningfullyMorePreciseThanByte)
{
	// D1's whole premise: Half must resolve finer detail than Byte at the
	// same range - otherwise there'd be no reason for a Half tier to exist.
	const double range = 1.0;
	int8_t byteCode = QuantizeByte(0.123456, range);
	int16_t halfCode = QuantizeHalf(0.123456, range);
	double byteBack = DequantizeByte(byteCode, range);
	double halfBack = DequantizeHalf(halfCode, range);
	double byteError = std::abs(byteBack - 0.123456);
	double halfError = std::abs(halfBack - 0.123456);
	O3DS_CHECK(halfError < byteError);
}

O3DS_TEST(RotationByte_RoundTripsIdentity)
{
	Quat identity = QuatIdentity();
	SmallestThreeQ8 encoded = QuantizeRotationByte(identity);
	Quat decoded = DequantizeRotationByte(encoded);
	O3DS_CHECK(QuatAngleDelta(identity, decoded) < 0.05);
}

O3DS_TEST(RotationByte_RoundTripsAxisAlignedRotations)
{
	// One rotation per principal axis, each landing a different component
	// as "largest" (dropped) to exercise all four droppedIndex cases.
	Quat rotations[] = {
		QuatFromAxisAngle(Vector3d(1.0, 0.0, 0.0), rad(37.0)),
		QuatFromAxisAngle(Vector3d(0.0, 1.0, 0.0), rad(75.0)),
		QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), rad(112.0)),
		QuatFromAxisAngle(Vector3d(1.0, 1.0, 1.0), rad(15.0)),
	};
	for (const Quat& q : rotations)
	{
		SmallestThreeQ8 encoded = QuantizeRotationByte(q);
		Quat decoded = DequantizeRotationByte(encoded);
		// Byte tier is coarse (~1/127 per component) - allow a generous
		// angular tolerance, just confirming it's in the right neighborhood
		// and not catastrophically wrong (e.g. wrong dropped-axis bug).
		O3DS_CHECK(QuatAngleDelta(q, decoded) < rad(5.0));
	}
}

O3DS_TEST(RotationHalf_RoundTripsAxisAlignedRotationsMorePrecisely)
{
	Quat rotations[] = {
		QuatFromAxisAngle(Vector3d(1.0, 0.0, 0.0), rad(37.0)),
		QuatFromAxisAngle(Vector3d(0.0, 1.0, 0.0), rad(75.0)),
		QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), rad(112.0)),
		QuatFromAxisAngle(Vector3d(1.0, 1.0, 1.0), rad(15.0)),
	};
	for (const Quat& q : rotations)
	{
		SmallestThreeQ16 encoded = QuantizeRotationHalf(q);
		Quat decoded = DequantizeRotationHalf(encoded);
		O3DS_CHECK(QuatAngleDelta(q, decoded) < rad(0.05));
	}
}

O3DS_TEST(RotationHalf_DecodedQuaternionStaysUnitLength)
{
	// The roadmap's explicit requirement: unlike naive per-component
	// quantization, smallest-three must always renormalize cleanly.
	Quat q = QuatFromAxisAngle(Vector3d(0.3, 0.7, -0.2), rad(83.0));
	SmallestThreeQ16 encoded = QuantizeRotationHalf(q);
	Quat decoded = DequantizeRotationHalf(encoded);
	double lenSq = decoded.v[0] * decoded.v[0] + decoded.v[1] * decoded.v[1]
		+ decoded.v[2] * decoded.v[2] + decoded.v[3] * decoded.v[3];
	O3DS_CHECK(NearlyEqual(lenSq, 1.0, 1.0e-9));
}

O3DS_TEST(RotationByte_NegativeLargestComponentStillRoundTrips)
{
	// Forces the sign-normalization branch: construct a quaternion whose
	// largest-magnitude component is negative before encoding.
	Quat q = QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), rad(179.0));
	Quat negated(-q.v[0], -q.v[1], -q.v[2], -q.v[3]); // same rotation, w now negative
	SmallestThreeQ8 encoded = QuantizeRotationByte(negated);
	Quat decoded = DequantizeRotationByte(encoded);
	O3DS_CHECK(QuatAngleDelta(q, decoded) < rad(5.0));
}

O3DS_TEST(RotationByte_OutOfRangeDroppedIndexDoesNotReadOutOfBounds)
{
	// Regression: droppedIndex is untrusted wire input (RotationUpdateQ8's
	// `dropped` byte) - a value outside [0,3] previously made
	// ReconstructSmallestThree's loop read one element past the end of its
	// 3-element `others` array on the 4th iteration (ASan catches this).
	// Just confirms it doesn't crash/misbehave; the resulting quaternion's
	// exact value for garbage input isn't otherwise meaningful.
	SmallestThreeQ8 garbage;
	garbage.droppedIndex = 200;
	garbage.a = 10;
	garbage.b = -20;
	garbage.c = 30;
	Quat decoded = DequantizeRotationByte(garbage);
	double lenSq = decoded.v[0] * decoded.v[0] + decoded.v[1] * decoded.v[1]
		+ decoded.v[2] * decoded.v[2] + decoded.v[3] * decoded.v[3];
	O3DS_CHECK(NearlyEqual(lenSq, 1.0, 1.0e-9));
}

O3DS_TEST(RotationHalf_ManyRandomLikeRotationsRoundTripWithinTolerance)
{
	// Deterministic pseudo-random sweep (no <random> - keep the test
	// itself trivially reproducible) across axes and angles.
	for (int i = 0; i < 50; ++i)
	{
		double ax = std::sin(static_cast<double>(i) * 0.7);
		double ay = std::cos(static_cast<double>(i) * 1.3);
		double az = std::sin(static_cast<double>(i) * 2.1 + 0.5);
		double angleDeg = static_cast<double>((i * 37) % 360);
		Quat q = QuatFromAxisAngle(Vector3d(ax, ay, az), rad(angleDeg));

		SmallestThreeQ16 encoded = QuantizeRotationHalf(q);
		Quat decoded = DequantizeRotationHalf(encoded);
		O3DS_CHECK(QuatAngleDelta(q, decoded) < rad(0.1));
	}
}
