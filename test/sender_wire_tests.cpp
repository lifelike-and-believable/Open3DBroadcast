// Sender wire-correctness tests (WP-S3; findings SND-1/2/3/4/5/13/14 in
// docs/review/2026-09-plugin-review/sender.md; ADR 0005).
//
// Two layers:
//   - the pure policy helpers in o3ds/sender_sync.h that the UE sender calls
//     (capture rate limiter, curve send filter, full-sync tracker);
//   - end-to-end round trips through the real Subject serializers and
//     SubjectList::Parse, driven the way FO3DSenderSerializer drives them:
//     a persistent sender Subject, a FullSyncTracker deciding full versus
//     update, and a receiver parsing every packet.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/sender_sync.h"
#include "o3ds/predict/residual_codec.h"
#include "o3ds_generated.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace O3DS;

namespace
{
	bool NearlyEqual(double a, double b, double tol)
	{
		return std::fabs(a - b) <= tol;
	}

	// Deterministic jitter source (no <random> distribution, whose output is
	// implementation-defined).
	struct Lcg
	{
		uint32_t state;
		explicit Lcg(uint32_t seed) : state(seed) {}
		// Uniform in [-1, 1].
		double Next()
		{
			state = state * 1664525u + 1013904223u;
			return ((double)(state >> 8) / (double)(1u << 24)) * 2.0 - 1.0;
		}
	};

	int CountAcceptedTicks(double tickHz, double jitterSeconds, double captureHz, double durationSeconds, uint32_t seed)
	{
		Lcg rng(seed);
		double lastSlot = 0.0;
		int accepted = 0;
		const double start = 100.0; // any positive clock origin
		const int ticks = (int)std::lround(durationSeconds * tickHz);
		for (int i = 0; i < ticks; ++i)
		{
			const double now = start + (double)i / tickHz + jitterSeconds * rng.Next();
			if (ConsumeCaptureBudget(now, lastSlot, captureHz))
				++accepted;
		}
		return accepted;
	}

	uint64_t HashNames(const std::vector<std::string>& names)
	{
		// FNV-1a over the ordered list, with a separator per name.
		uint64_t h = 1469598103934665603ull;
		for (const std::string& n : names)
		{
			for (unsigned char c : n)
			{
				h ^= c;
				h *= 1099511628211ull;
			}
			h ^= 0xFF;
			h *= 1099511628211ull;
		}
		return h;
	}

	struct Bone
	{
		std::string name;
		int parent;
	};

	enum class Mode { Quantized, Residual };

	// Minimal mirror of FO3DSenderSerializer's persistent path: one
	// persistent Subject, a FullSyncTracker, and optionally a rebuild of every
	// Transform object at each full sync (the pre-WP-S3 UE behaviour, which
	// SND-2 showed corrupted quantized deltas).
	struct SenderSim
	{
		Mode mode;
		bool rebuildTransformsOnFullSync;
		std::vector<Bone> bones;
		SubjectList list;
		Subject* subject = nullptr;
		FullSyncTracker tracker;
		QuantRanges ranges;
		double deltaThreshold = 1.0e-6;
		double interval = 1.0;
		uint64_t descriptorHash = 0x1234;

		SenderSim(Mode inMode, bool inRebuild, std::vector<Bone> inBones)
			: mode(inMode), rebuildTransformsOnFullSync(inRebuild), bones(std::move(inBones))
		{
			subject = list.addSubject("Actor");
			ranges.byteRange = 0.01;
			ranges.halfRange = 1.0;
		}

		void BuildTransforms()
		{
			subject->clear();
			for (const Bone& b : bones)
			{
				Transform* t = subject->addTransform(b.name, b.parent);
				t->transformOrder = { TTranslation, TRotation, TScale };
			}
		}

		// Returns the packet and whether it was a full sync.
		std::vector<char> Frame(double now,
			const std::vector<Vector3d>& translations,
			const std::vector<std::string>& curveNames,
			const std::vector<float>& curveValues,
			bool& outFull)
		{
			FullSyncInputs in;
			in.descriptorHash = descriptorHash;
			in.curveNamesHash = HashNames(curveNames);
			in.encodingFingerprint = (uint64_t)mode;
			in.nowSeconds = now;
			in.intervalSeconds = interval;

			outFull = tracker.Evaluate(in) != FullSyncTracker::None;
			if (outFull && (rebuildTransformsOnFullSync || subject->size() != bones.size()))
			{
				BuildTransforms();
			}
			for (size_t i = 0; i < bones.size(); ++i)
			{
				subject->mTransforms[i]->translation.value = translations[i];
			}

			std::vector<char> buf;
			if (outFull)
			{
				subject->mCurveNames = curveNames;
				subject->mCurveValues = curveValues;
				if (mode == Mode::Residual)
				{
					subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear, 0));
				}
				O3DS_CHECK(subject->Serialize(buf, now) > 0);
				tracker.MarkFullSent(in);
			}
			else
			{
				O3DS_CHECK(subject->mCurveValues.size() == curveValues.size());
				subject->mCurveValues = curveValues;
				size_t count = 0;
				if (mode == Mode::Residual)
				{
					O3DS_CHECK(subject->SerializeUpdateResidual(buf, count, deltaThreshold, now) > 0);
				}
				else
				{
					O3DS_CHECK(subject->SerializeUpdate(buf, count, deltaThreshold, now, &ranges) > 0);
				}
			}
			return buf;
		}
	};

	bool IsFullSyncPacket(const std::vector<char>& buf)
	{
		auto root = O3DS::Data::GetSubjectList(buf.data() + 8);
		return root->subjects() != nullptr && root->subjects()->size() > 0;
	}

	void CheckCurves(SubjectList& receiver, const std::vector<std::string>& names, const std::vector<float>& values, double tol)
	{
		Subject* r = receiver.findSubject("Actor");
		O3DS_CHECK(r != nullptr);
		O3DS_CHECK_EQ(r->mCurveNames.size(), names.size());
		O3DS_CHECK_EQ(r->mCurveValues.size(), values.size());
		for (size_t i = 0; i < names.size(); ++i)
		{
			O3DS_CHECK(r->mCurveNames[i] == names[i]);
			O3DS_CHECK(NearlyEqual(r->mCurveValues[i], values[i], tol));
		}
	}
}

// ---------------------------------------------------------------------------
// SND-5: capture rate limiter
// ---------------------------------------------------------------------------

O3DS_TEST(RateLimiter_FirstCallCapturesAndAnchors)
{
	double last = 0.0;
	O3DS_CHECK(ConsumeCaptureBudget(10.0, last, 60.0));
	O3DS_CHECK(last == 10.0);
	O3DS_CHECK(!ConsumeCaptureBudget(10.001, last, 60.0));
	O3DS_CHECK(ConsumeCaptureBudget(10.0 + 1.0 / 60.0 + 0.001, last, 60.0));
}

O3DS_TEST(RateLimiter_DisabledRateAlwaysCaptures)
{
	double last = -1.0;
	O3DS_CHECK(ConsumeCaptureBudget(5.0, last, 0.0));
	O3DS_CHECK(ConsumeCaptureBudget(5.0001, last, 0.0));
	O3DS_CHECK(ConsumeCaptureBudget(5.0002, last, -5.0));
	O3DS_CHECK(ConsumeCaptureBudget(5.0003, last, std::nan("")));
	O3DS_CHECK(last == 5.0003);
}

O3DS_TEST(RateLimiter_Jittered60HzTick_AcceptsAbout60PerSecond)
{
	// The WP-S3 acceptance case: a 60 Hz tick with +/-1 ms jitter and a
	// 60 Hz capture rate. The old "now - last < 1/rate" gate dropped roughly
	// every other frame here.
	const double duration = 10.0;
	const int accepted = CountAcceptedTicks(60.0, 0.001, 60.0, duration, 12345u);
	O3DS_CHECK(accepted >= 590);
	O3DS_CHECK(accepted <= 601);

	// Same with larger (+/-3 ms) jitter.
	const int acceptedWide = CountAcceptedTicks(60.0, 0.003, 60.0, duration, 777u);
	O3DS_CHECK(acceptedWide >= 580);
	O3DS_CHECK(acceptedWide <= 601);
}

O3DS_TEST(RateLimiter_FasterTickDecimatesToCaptureRate)
{
	const int at144 = CountAcceptedTicks(144.0, 0.0005, 60.0, 10.0, 99u);
	O3DS_CHECK(at144 >= 595);
	O3DS_CHECK(at144 <= 605);

	const int at120 = CountAcceptedTicks(120.0, 0.0005, 30.0, 10.0, 5u);
	O3DS_CHECK(at120 >= 297);
	O3DS_CHECK(at120 <= 303);
}

O3DS_TEST(RateLimiter_SlowerTickCapturesEveryTickWithoutBurst)
{
	// Tick rate below the capture rate: every tick captures, and after a
	// long hitch there is no catch-up burst.
	double last = 0.0;
	int accepted = 0;
	for (int i = 0; i < 300; ++i)
	{
		if (ConsumeCaptureBudget(1.0 + i / 30.0, last, 60.0))
			++accepted;
	}
	O3DS_CHECK_EQ(accepted, 300);

	double hitchLast = 0.0;
	O3DS_CHECK(ConsumeCaptureBudget(1.0, hitchLast, 60.0));
	O3DS_CHECK(ConsumeCaptureBudget(1.5, hitchLast, 60.0)); // 500 ms hitch
	// Back to a 60 Hz tick: the next capture is one interval after the hitch
	// frame, not immediately.
	O3DS_CHECK(!ConsumeCaptureBudget(1.5 + 0.005, hitchLast, 60.0));
	O3DS_CHECK(ConsumeCaptureBudget(1.5 + 1.0 / 60.0, hitchLast, 60.0));
}

O3DS_TEST(RateLimiter_ClockStepBackwardsReanchors)
{
	double last = 0.0;
	O3DS_CHECK(ConsumeCaptureBudget(50.0, last, 60.0));
	O3DS_CHECK(ConsumeCaptureBudget(10.0, last, 60.0));
	O3DS_CHECK(last == 10.0);
	O3DS_CHECK(ConsumeCaptureBudget(10.0 + 1.0 / 60.0, last, 60.0));
}

// ---------------------------------------------------------------------------
// SND-4: curve epsilon/delta filter
// ---------------------------------------------------------------------------

O3DS_TEST(CurveFilter_ReturnToZeroIsSentOnce)
{
	// Acceptance case: a curve going 0.8, 0, 0.
	const float epsilon = 0.0005f;
	const float delta = 0.001f;
	bool hasLast = false;
	float last = 0.0f;
	const float sequence[3] = { 0.8f, 0.0f, 0.0f };
	bool sent[3] = {};
	float wire[3] = {};
	for (int i = 0; i < 3; ++i)
	{
		float value = sequence[i];
		sent[i] = FilterCurveValue(value, hasLast, last, epsilon, delta);
		if (sent[i])
		{
			wire[i] = value;
			last = value;
			hasLast = true;
		}
	}
	O3DS_CHECK(sent[0]);
	O3DS_CHECK(wire[0] == 0.8f);
	O3DS_CHECK(sent[1]);
	O3DS_CHECK(wire[1] == 0.0f);
	O3DS_CHECK(!sent[2]);
	O3DS_CHECK(last == 0.0f);
}

O3DS_TEST(CurveFilter_NearZeroSnapsToExactZero)
{
	float value = 0.0003f;
	O3DS_CHECK(FilterCurveValue(value, true, 0.5f, 0.0005f, 0.001f));
	O3DS_CHECK(value == 0.0f);
}

O3DS_TEST(CurveFilter_NeverSentZeroAndSmallDeltasAreSuppressed)
{
	float zero = 0.0f;
	O3DS_CHECK(!FilterCurveValue(zero, false, 0.0f, 0.0005f, 0.001f));

	float small = 0.5004f;
	O3DS_CHECK(!FilterCurveValue(small, true, 0.5f, 0.0005f, 0.001f));

	float moved = 0.502f;
	O3DS_CHECK(FilterCurveValue(moved, true, 0.5f, 0.0005f, 0.001f));
	O3DS_CHECK(moved == 0.502f);

	float first = 0.3f;
	O3DS_CHECK(FilterCurveValue(first, false, 0.0f, 0.0005f, 0.001f));
}

// ---------------------------------------------------------------------------
// SND-1/3/13/14: full-sync tracker
// ---------------------------------------------------------------------------

O3DS_TEST(FullSyncTracker_Triggers)
{
	FullSyncTracker tracker;
	FullSyncInputs in;
	in.descriptorHash = 1;
	in.curveNamesHash = 2;
	in.encodingFingerprint = 3;
	in.nowSeconds = 10.0;
	in.intervalSeconds = 1.0;

	O3DS_CHECK(tracker.Evaluate(in) == FullSyncTracker::First);
	tracker.MarkFullSent(in);
	O3DS_CHECK(tracker.HasSentFull());
	O3DS_CHECK(tracker.Evaluate(in) == FullSyncTracker::None);

	FullSyncInputs later = in;
	later.nowSeconds = 10.99;
	O3DS_CHECK(tracker.Evaluate(later) == FullSyncTracker::None);
	later.nowSeconds = 11.0;
	O3DS_CHECK(tracker.Evaluate(later) == FullSyncTracker::Interval);

	FullSyncInputs changed = in;
	changed.descriptorHash = 9;
	O3DS_CHECK(tracker.Evaluate(changed) == FullSyncTracker::Descriptor);
	changed = in;
	changed.curveNamesHash = 9;
	O3DS_CHECK(tracker.Evaluate(changed) == FullSyncTracker::CurveNames);
	changed = in;
	changed.encodingFingerprint = 9;
	O3DS_CHECK(tracker.Evaluate(changed) == FullSyncTracker::Encoding);

	FullSyncInputs backwards = in;
	backwards.nowSeconds = 5.0;
	O3DS_CHECK(tracker.Evaluate(backwards) == FullSyncTracker::Interval);

	FullSyncInputs noInterval = in;
	noInterval.intervalSeconds = 0.0;
	noInterval.nowSeconds = 1000.0;
	O3DS_CHECK(tracker.Evaluate(noInterval) == FullSyncTracker::None);

	tracker.RequestFullSync();
	O3DS_CHECK(tracker.Evaluate(in) == FullSyncTracker::Requested);
	tracker.MarkFullSent(in);
	O3DS_CHECK(tracker.Evaluate(in) == FullSyncTracker::None);

	tracker.Reset();
	O3DS_CHECK(!tracker.HasSentFull());
	O3DS_CHECK(tracker.Evaluate(in) == FullSyncTracker::First);
}

// ---------------------------------------------------------------------------
// SND-2/SND-13: quantized full, delta, resync, delta
// ---------------------------------------------------------------------------

namespace
{
	void RunQuantizedResyncScenario(bool rebuildTransformsOnFullSync, bool clearInactive)
	{
		// Root far from the origin so float32 rounding of the anchor matters
		// (ADR 0005 (vii)), and a child with small local motion.
		SenderSim sender(Mode::Quantized, rebuildTransformsOnFullSync, { { "Root", -1 }, { "Hip", 0 } });
		sender.interval = 1.0;
		SubjectList receiver;
		SubjectList lateReceiver; // joins at the second full sync
		bool lateJoined = false;

		const double byteTol = 0.01 / 127.0 + 1.0e-5;
		const double halfTol = 1.0 / 32767.0 + 1.0e-5;
		int fullSyncs = 0;
		int quantizedUpdates = 0;

		// 2.5 s at 60 Hz: full at t=0, periodic resyncs at t=1 and t=2.
		for (int i = 0; i < 150; ++i)
		{
			const double t = i / 60.0;
			std::vector<Vector3d> tr = {
				Vector3d(1234.567 + 0.3 * t, -250.125 + 0.2 * t, 98.3),
				Vector3d(10.0 + 0.004 * std::sin(6.0 * t), 0.002 * std::cos(4.0 * t), 5.0),
			};
			bool full = false;
			std::vector<char> buf = sender.Frame(100.0 + t, tr, {}, {}, full);
			O3DS_CHECK(full == IsFullSyncPacket(buf));
			if (full)
			{
				++fullSyncs;
			}

			O3DS_CHECK(receiver.Parse(buf.data(), buf.size(), nullptr, clearInactive));
			if (full && fullSyncs == 2)
			{
				lateJoined = true;
			}
			if (lateJoined)
			{
				O3DS_CHECK(lateReceiver.Parse(buf.data(), buf.size(), nullptr, clearInactive));
			}

			if (!full)
			{
				auto root = O3DS::Data::GetSubjectList(buf.data() + 8);
				auto update = root->updates()->Get(0);
				if (update->translations_q8() || update->translations_q16())
					++quantizedUpdates;
			}

			for (SubjectList* rx : { &receiver, lateJoined ? &lateReceiver : nullptr })
			{
				if (rx == nullptr)
					continue;
				Subject* r = rx->findSubject("Actor");
				O3DS_CHECK(r != nullptr);
				O3DS_CHECK_EQ(r->mTransforms.size(), (size_t)2);
				for (size_t b = 0; b < 2; ++b)
				{
					for (int axis = 0; axis < 3; ++axis)
					{
						// Full-precision channels round-trip through float32:
						// allow float rounding at ~1234 (about 1.2e-4), plus
						// the half tier's step.
						const double tol = (b == 0) ? (halfTol + 2.0e-4) : byteTol;
						O3DS_CHECK(NearlyEqual(r->mTransforms[b]->translation.value.v[axis], tr[b].v[axis], tol));
					}
				}
			}
		}
		O3DS_CHECK_EQ(fullSyncs, 3);
		O3DS_CHECK(quantizedUpdates > 100);
	}
}

O3DS_TEST(QuantizedResync_TransformsKept_DecodesWithinTolerance)
{
	// clearInactive=true is what the UE receiver passes; false keeps the
	// receiver's Subject objects across full syncs, which is where the old
	// preserve-anchor-by-name logic ran.
	RunQuantizedResyncScenario(false, true);
	RunQuantizedResyncScenario(false, false);
}

O3DS_TEST(QuantizedResync_TransformsRebuilt_DecodesWithinTolerance)
{
	// The pre-WP-S3 UE serializer deleted and recreated every Transform at a
	// resync (SND-2). With re-anchoring at every full sync this is now only
	// an allocation cost, not a correctness problem.
	RunQuantizedResyncScenario(true, true);
	RunQuantizedResyncScenario(true, false);
}

// ---------------------------------------------------------------------------
// SND-3: curve add/remove with residual and quantized coding
// ---------------------------------------------------------------------------

namespace
{
	void RunCurveMembershipScenario(Mode mode)
	{
		SenderSim sender(mode, false, { { "Root", -1 }, { "Head", 0 } });
		sender.interval = 1.0;
		SubjectList receiver;

		std::vector<std::string> names = { "Blink", "Smile" };
		std::vector<float> values = { 0.1f, 0.2f };
		int fullSyncs = 0;

		for (int i = 0; i < 40; ++i)
		{
			const double t = i / 60.0;
			if (i == 10)
			{
				// Curve added in the middle of the list (same count change
				// the old count-only check caught).
				names = { "Blink", "JawOpen", "Smile" };
				values = { values[0], 0.0f, values[1] };
			}
			if (i == 20)
			{
				// Swap membership at the SAME count: "Blink" leaves and
				// "Frown" joins. The old serializer wrote these values by
				// index under the previous names (SND-3).
				names = { "Frown", "JawOpen", "Smile" };
				values = { 0.9f, values[1], values[2] };
			}
			if (i == 30)
			{
				// Curve removed.
				names = { "Frown", "Smile" };
				values = { values[0], values[2] };
			}
			for (size_t c = 0; c < values.size(); ++c)
			{
				values[c] = (float)(0.5 + 0.4 * std::sin(0.3 * i + (double)c));
			}

			std::vector<Vector3d> tr = { Vector3d(0.01 * t, 0.0, 0.0), Vector3d(0.0, 15.0, 0.0) };
			bool full = false;
			std::vector<char> buf = sender.Frame(100.0 + t, tr, names, values, full);
			if (full)
				++fullSyncs;
			O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));
			CheckCurves(receiver, names, values, 1.0e-5);
		}

		// First frame plus exactly one resync per membership change; no
		// resync while the curve list is stable (the interval is 1 s and the
		// run lasts 0.65 s).
		O3DS_CHECK_EQ(fullSyncs, 4);
	}
}

O3DS_TEST(CurveMembership_Quantized_ResyncsAndStaysAligned)
{
	RunCurveMembershipScenario(Mode::Quantized);
}

O3DS_TEST(CurveMembership_Residual_ResyncsAndStaysAligned)
{
	RunCurveMembershipScenario(Mode::Residual);
}
