// Acceptance tests for src/o3ds/channel_model.h, per the roadmap doc's B2
// spec (docs/roadmap/resilient-streaming-and-motion-prediction.md, section
// "Phase B2"). Includes the interlock tests with A1's ReorderGate, which is
// the entire reason ChannelModel exists as a test fixture.
#include "test_framework.h"

#include "o3ds/channel_model.h"
#include "o3ds/reorder_gate.h"

using namespace O3DS;

namespace
{
	Frame MakeFrame(uint64_t seq)
	{
		Frame f;
		f.seq = seq;
		f.bytes = std::vector<char>(4, (char)(seq & 0xFF));
		return f;
	}

	struct Collected
	{
		std::vector<Frame> frames;
		std::function<void(Frame&&)> Sink()
		{
			return [this](Frame&& f) { frames.push_back(std::move(f)); };
		}
	};
}

O3DS_TEST(ChannelModel_ZeroParams_IsPassThrough)
{
	ChannelConfig config; // all zero/default
	ChannelModel channel(config);
	Collected out;

	const int kCount = 20;
	for (int i = 1; i <= kCount; i++)
		channel.Push(MakeFrame((uint64_t)i), (double)i, out.Sink());
	channel.Flush(out.Sink());

	O3DS_CHECK_EQ(out.frames.size(), (size_t)kCount);
	for (int i = 0; i < kCount; i++)
		O3DS_CHECK_EQ(out.frames[(size_t)i].seq, (uint64_t)(i + 1));

	O3DS_CHECK_EQ(channel.Stats().dropped, (uint64_t)0);
	O3DS_CHECK_EQ(channel.Stats().duplicated, (uint64_t)0);
	O3DS_CHECK_EQ(channel.Stats().reordered, (uint64_t)0);
	O3DS_CHECK_EQ(channel.Stats().passed, (uint64_t)kCount);
}

O3DS_TEST(ChannelModel_Determinism_SameSeedSameOutput)
{
	ChannelConfig config;
	config.seed = 12345;
	config.loss_prob = 0.3;
	config.max_jitter_s = 0.05;
	config.dup_prob = 0.1;

	auto run = [&]() {
		ChannelModel channel(config);
		Collected out;
		for (int i = 1; i <= 100; i++)
			channel.Push(MakeFrame((uint64_t)i), (double)i * 0.01, out.Sink());
		channel.Flush(out.Sink());

		std::vector<uint64_t> seqLog;
		for (auto& f : out.frames)
			seqLog.push_back(f.seq);
		return std::make_pair(seqLog, channel.Stats());
	};

	auto r1 = run();
	auto r2 = run();

	O3DS_CHECK(r1.first == r2.first); // identical event log, same seed
	O3DS_CHECK_EQ(r1.second.dropped, r2.second.dropped);
	O3DS_CHECK_EQ(r1.second.duplicated, r2.second.duplicated);
	O3DS_CHECK_EQ(r1.second.reordered, r2.second.reordered);
	O3DS_CHECK_EQ(r1.second.passed, r2.second.passed);
}

O3DS_TEST(ChannelModel_DifferentSeeds_TypicallyDiffer)
{
	// Not a strict guarantee for every possible pair of seeds, but with
	// loss_prob=0.5 over 200 frames, two distinct seeds producing the exact
	// same drop pattern is astronomically unlikely - this guards against a
	// broken RNG that ignores the seed entirely.
	auto run = [](uint64_t seed) {
		ChannelConfig config;
		config.seed = seed;
		config.loss_prob = 0.5;
		ChannelModel channel(config);
		Collected out;
		for (int i = 1; i <= 200; i++)
			channel.Push(MakeFrame((uint64_t)i), (double)i, out.Sink());
		channel.Flush(out.Sink());
		return channel.Stats().dropped;
	};

	O3DS_CHECK(run(1) != run(2));
}

O3DS_TEST(ChannelModel_LossOnly_FedIntoReorderGate_LostMatchesDropped)
{
	// The crisp B2/A1 interlock test: with jitter=0, loss alone produces
	// gaps the gate can only classify as `lost` (never `reordered`, since
	// nothing arrives out of order when there's no jitter).
	ChannelConfig config;
	config.seed = 42;
	config.loss_prob = 0.05;
	config.max_jitter_s = 0.0;

	ChannelModel channel(config);
	ReorderGate gate;

	const int kCount = 500;
	double t = 0.0;
	for (int i = 1; i <= kCount; i++)
	{
		t += 1.0 / 60.0; // ~60fps spacing
		channel.Push(MakeFrame((uint64_t)i), t, [&](Frame&& f) {
			gate.Push(std::move(f), t, [](Frame&&) {});
		});
	}
	channel.Flush([&](Frame&& f) {
		gate.Push(std::move(f), t + 1.0, [](Frame&&) {});
	});
	gate.Flush(t + 10.0, [](Frame&&) {}); // release any still-pending gap

	O3DS_CHECK(channel.Stats().dropped > (uint64_t)0); // sanity: the test actually exercised loss
	O3DS_CHECK_EQ(gate.Stats().lost, channel.Stats().dropped);
	O3DS_CHECK_EQ(gate.Stats().reordered, (uint64_t)0);
}

O3DS_TEST(ChannelModel_JitterOnly_FedIntoReorderGate_FullyRecoveredNoLoss)
{
	// Jitter bounded well below the gate's default window/delay budget
	// guarantees every reordering the channel introduces is recoverable -
	// gate.lost stays 0 (see the CHECK below; this is the guarantee that
	// actually matters, not exact count equality - see the comment further
	// down for why gate.reordered and channel.reordered are related but
	// not required to match exactly). max_jitter_s is deliberately set to
	// ~1.8x the ~16.7ms frame spacing (not smaller) so reordering actually
	// happens with high probability - jitter meaningfully below one
	// frame-spacing can never produce a swap between adjacent frames,
	// which would make the reordered>0 sanity check below vacuously fail
	// to exercise anything. 30ms is still comfortably under the gate's
	// default 50ms/16-frame recovery budget.
	ChannelConfig config;
	config.seed = 7;
	config.loss_prob = 0.0;
	config.max_jitter_s = 0.03; // 30ms

	ChannelModel channel(config);
	ReorderGate gate;

	const int kCount = 300;
	double t = 0.0;
	for (int i = 1; i <= kCount; i++)
	{
		t += 1.0 / 60.0;
		channel.Push(MakeFrame((uint64_t)i), t, [&](Frame&& f) {
			gate.Push(std::move(f), t, [](Frame&&) {});
		});
	}
	channel.Flush([&](Frame&& f) {
		gate.Push(std::move(f), t + 1.0, [](Frame&&) {});
	});
	gate.Flush(t + 10.0, [](Frame&&) {});

	O3DS_CHECK(channel.Stats().reordered > (uint64_t)0); // sanity: the test actually exercised reordering
	O3DS_CHECK_EQ(gate.Stats().lost, (uint64_t)0); // the guarantee that actually matters: full recovery

	// NOT exact equality - the two "reordered" counters measure genuinely
	// different things, and they provably diverge for a specific shape of
	// input. ReorderGate::reordered only counts frames delivered via the
	// internal drain loop (buffered, then released because a predecessor
	// filled the gap). ChannelModel::reordered counts any frame released
	// with a lower seq than one already released. These coincide for a
	// simple two-frame swap (e.g. 1,3,2 - both count exactly 1), but
	// diverge when a gap is filled by MULTIPLE separately-arriving frames:
	// each one individually satisfies seq==last_applied+1 at the moment
	// *it* arrives (the gate's normal path, not the drain loop), even
	// though from the channel's release-order perspective every one of
	// them arrived after a higher seq already went out. Concretely, if the
	// channel releases in order 1, 4, 2, 3: the channel counts 2 reordered
	// (frames 2 and 3, each lower than the already-released 4); the gate
	// delivers 2 and 3 via its normal path (each fills the gap it's
	// currently waiting on) and only drains 4 afterward - gate.reordered
	// is 1, not 2. gate.reordered <= channel.reordered always holds as
	// long as gate.lost == 0 (confirmed empirically here, not just
	// asserted): every frame the channel calls "reordered" still gets
	// delivered, just not always through the specific code path the gate
	// happens to tag with that stat.
	O3DS_CHECK(gate.Stats().reordered <= channel.Stats().reordered);
}

O3DS_TEST(ChannelModel_CombinedRealism_DoesNotCrashOrHang)
{
	// Not an exact-count assertion (per the roadmap's own acceptance
	// criteria for this case) - this combination is for A2/C1-style
	// smoke/quality checks, not a precise interlock proof. Just prove it
	// runs to completion cleanly under ASan/UBSan with both effects active.
	ChannelConfig config;
	config.seed = 99;
	config.loss_prob = 0.05;
	config.max_jitter_s = 0.12; // 120ms
	config.dup_prob = 0.02;

	ChannelModel channel(config);
	ReorderGate gate;

	const int kCount = 1000;
	double t = 0.0;
	for (int i = 1; i <= kCount; i++)
	{
		t += 1.0 / 60.0;
		channel.Push(MakeFrame((uint64_t)i), t, [&](Frame&& f) {
			gate.Push(std::move(f), t, [](Frame&&) {});
		});
	}
	channel.Flush([&](Frame&& f) {
		gate.Push(std::move(f), t + 1.0, [](Frame&&) {});
	});
	gate.Flush(t + 10.0, [](Frame&&) {});

	// Just needs to have actually run without crashing; loose sanity bounds.
	O3DS_CHECK(channel.Stats().passed <= (uint64_t)(kCount * 2)); // dup_prob bounds this
	O3DS_CHECK(gate.Stats().delivered <= channel.Stats().passed);
}

O3DS_TEST(ChannelModel_DuplicateProbabilityOne_DoublesDelivery)
{
	ChannelConfig config;
	config.seed = 5;
	config.dup_prob = 1.0; // always duplicate, never drop, no jitter
	ChannelModel channel(config);
	Collected out;

	const int kCount = 10;
	for (int i = 1; i <= kCount; i++)
		channel.Push(MakeFrame((uint64_t)i), (double)i, out.Sink());
	channel.Flush(out.Sink());

	O3DS_CHECK_EQ(channel.Stats().duplicated, (uint64_t)kCount);
	O3DS_CHECK_EQ(out.frames.size(), (size_t)(kCount * 2));
}

O3DS_TEST(ChannelModel_LegacySeqZero_ExcludedFromReorderedTracking)
{
	ChannelConfig config; // no loss/jitter/dup
	ChannelModel channel(config);
	Collected out;

	channel.Push(MakeFrame(100), 1.0, out.Sink());
	channel.Push(MakeFrame(0), 2.0, out.Sink());   // legacy/unset - must not count as "reordered" against seq 100
	channel.Push(MakeFrame(101), 3.0, out.Sink());

	O3DS_CHECK_EQ(channel.Stats().reordered, (uint64_t)0);
}
