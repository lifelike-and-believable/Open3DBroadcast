// Tests for the control channel state machines (src/o3ds/control.h:
// ControlPublisher, ControlReceiver, ControlAligner).
// docs/adr/0011-control-channel.md, Verification: "State", "Events",
// "Limits", "Coalescing", "Timing" and "Load".
//
// Publisher and receiver are joined by Link, a seeded, time-driven lossy
// link defined below. ChannelModel (channel_model.h) is not used here: it
// releases delayed frames only when the next frame is pushed, which would
// hold control messages back on ticks with no traffic.
#include "test_framework.h"

#include "o3ds/control.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace O3DS::Control;
namespace ControlLimits = O3DS::ControlLimits;

namespace
{
	const std::string kSource = "0a1b2c3d4e5f60718293a4b5c6d7e8f9";
	constexpr double kDt = 1.0 / 60.0;

	uint64_t SenderUs(double nowS) { return 1000000ull + static_cast<uint64_t>(nowS * 1.0e6); }

	struct Link
	{
		double loss = 0.0;
		double latency_s = 0.0;
		double jitter_s = 0.0;
		bool connected = true;
		std::function<bool(const Message&)> drop; //!< drop matching messages (after parsing)
		std::mt19937_64 rng{ 12345 };
		std::multimap<double, std::vector<uint8_t>> inflight;

		//! Same sequence on every standard library (unlike
		//! std::uniform_real_distribution), so a seed means the same losses
		//! on MSVC and on CI's GCC.
		double Uniform() { return static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0); }

		void Send(const OutgoingMessage& m, double nowS)
		{
			const double lossDraw = Uniform();
			const double jitterDraw = Uniform();
			if (!connected || lossDraw < loss)
				return;
			if (drop)
			{
				Message parsed;
				if (ParseMessage(m.bytes.data(), m.bytes.size(), parsed) == ParseError::None && drop(parsed))
					return;
			}
			inflight.emplace(nowS + latency_s + jitterDraw * jitter_s, m.bytes);
		}

		void Deliver(double nowS, ControlReceiver& rx, std::vector<Change>& out)
		{
			while (!inflight.empty() && inflight.begin()->first <= nowS)
			{
				const std::vector<uint8_t> bytes = std::move(inflight.begin()->second);
				inflight.erase(inflight.begin());
				rx.Submit(bytes.data(), bytes.size(), nowS, out);
			}
		}
	};

	struct Sim
	{
		ControlPublisher pub;
		ControlReceiver rx;
		Link link;
		double now = 0.0;
		std::vector<Change> changes;
		std::vector<OutgoingMessage> sent; //!< everything the publisher produced

		explicit Sim(const PublisherConfig& pc = PublisherConfig(), const ReceiverConfig& rc = ReceiverConfig())
			: pub(kSource, "TestSender", pc), rx(rc)
		{
		}

		void Step()
		{
			std::vector<OutgoingMessage> out;
			pub.Tick(now, SenderUs(now), 0, out);
			for (const OutgoingMessage& m : out)
			{
				link.Send(m, now);
				sent.push_back(m);
			}
			link.Deliver(now, rx, changes);
			rx.Tick(now, changes);
			now += kDt;
		}

		void Run(double seconds)
		{
			const double end = now + seconds;
			while (now < end)
				Step();
		}

		//! Receiver's table equals the publisher's for `keys`.
		bool Converged(const std::vector<std::string>& keys) const
		{
			for (const std::string& k : keys)
			{
				const Value* want = pub.FindValue(k, "");
				const Value* have = rx.FindValue(kSource, k, "");
				if ((want == nullptr) != (have == nullptr))
					return false;
				if (want != nullptr && *want != *have)
					return false;
			}
			return rx.GetValues(kSource).size() == pub.NumValues();
		}
	};

	size_t Count(const std::vector<Change>& changes, Change::Kind kind)
	{
		return static_cast<size_t>(std::count_if(changes.begin(), changes.end(), [kind](const Change& c) { return c.kind == kind; }));
	}

	//! Every value change and clear for a key carries a strictly higher
	//! version than the one before it: nothing stale was ever applied.
	bool VersionsStrictlyIncrease(const std::vector<Change>& changes)
	{
		std::map<std::pair<uint32_t, std::string>, uint64_t> last;
		for (const Change& c : changes)
		{
			if (c.kind == Change::Kind::Event)
				continue;
			const auto key = std::make_pair(c.epoch, c.name + "|" + c.target);
			const auto it = last.find(key);
			if (it != last.end() && c.version <= it->second)
				return false;
			last[key] = c.version;
		}
		return true;
	}

	std::vector<Message> Parsed(const std::vector<OutgoingMessage>& out)
	{
		std::vector<Message> parsed;
		for (const OutgoingMessage& m : out)
		{
			Message p;
			if (ParseMessage(m.bytes.data(), m.bytes.size(), p) == ParseError::None)
				parsed.push_back(std::move(p));
		}
		return parsed;
	}

	Message Live(uint32_t epoch, uint64_t seq)
	{
		Message m;
		m.source_id = kSource;
		m.epoch = epoch;
		m.seq = seq;
		m.sender_time_us = SenderUs(0.0) + seq * 1000;
		return m;
	}

	Entry SetEntry(const std::string& key, double v, uint64_t version)
	{
		Entry e;
		e.key = key;
		e.value = Value::MakeDouble(v);
		e.version = version;
		return e;
	}

	void ApplyOne(ControlReceiver& rx, const Message& m, double nowS, std::vector<Change>& out)
	{
		O3DS_CHECK(Validate(m) == ParseError::None);
		rx.Apply(m, 100, nowS, out);
	}
}

// ---------------------------------------------------------------------------
// Publisher
// ---------------------------------------------------------------------------

O3DS_TEST(ControlPub_NoTrafficWhenUnused)
{
	Sim sim;
	sim.pub.Start(1000);
	sim.Run(5.0);
	O3DS_CHECK(sim.sent.empty());
}

O3DS_TEST(ControlPub_ValuesSetBeforeStartGoOutAtStartWithASnapshot)
{
	Sim sim;
	O3DS_CHECK(sim.pub.SetValue("env.fog_density", "", Value::MakeDouble(0.4)) == PublishResult::Ok);
	sim.Step();
	O3DS_CHECK(sim.sent.empty()); // stopped: nothing goes out

	sim.pub.Start(1000);
	sim.Step();
	const std::vector<Message> first = Parsed(sim.sent);
	O3DS_CHECK_EQ(first.size(), (size_t)2);
	O3DS_CHECK(!first[0].IsSnapshot());
	O3DS_CHECK_EQ(first[0].set.size(), (size_t)1);
	O3DS_CHECK(first[1].IsSnapshot());
	O3DS_CHECK_EQ(first[1].set.size(), (size_t)1);
	O3DS_CHECK_EQ(first[1].set[0].version, first[0].seq);
	O3DS_CHECK(sim.rx.FindValue(kSource, "env.fog_density", "") != nullptr);
}

O3DS_TEST(ControlPub_CoalescesManySetsInOneTick)
{
	Sim sim;
	sim.pub.Start(1000);
	for (int k = 0; k < 1000; ++k)
		O3DS_CHECK(sim.pub.SetValue("light.intensity", "", Value::MakeDouble(k)) == PublishResult::Ok);
	sim.Step();

	size_t entries = 0;
	for (const Message& m : Parsed(sim.sent))
	{
		if (m.IsSnapshot())
			continue;
		for (const Entry& e : m.set)
		{
			++entries;
			O3DS_CHECK(e.value == Value::MakeDouble(999));
		}
	}
	O3DS_CHECK_EQ(entries, (size_t)1);
}

O3DS_TEST(ControlPub_SettingTheSameValueIsANoOp)
{
	Sim sim;
	sim.pub.Start(1000);
	sim.pub.SetValue("char.wetness", "Hero", Value::MakeDouble(0.5));
	sim.Step();
	const size_t before = sim.sent.size();
	sim.pub.SetValue("char.wetness", "Hero", Value::MakeDouble(0.5));
	sim.Step();
	O3DS_CHECK_EQ(sim.sent.size(), before);
}

O3DS_TEST(ControlPub_RateLimitsOneKeyAndLatestWins)
{
	PublisherConfig pc;
	pc.max_value_rate_hz = 30.0;
	pc.snapshot_interval_s = 10.0; // keep snapshots out of the count
	Sim sim(pc);
	sim.pub.Start(1000);
	double last = 0.0;
	for (int tick = 0; tick < 180; ++tick) // 3 s at 60 Hz
	{
		last = tick * 0.01;
		sim.pub.SetValue("env.time_of_day", "", Value::MakeDouble(last));
		sim.Step();
	}
	sim.Run(0.5); // let the final value out

	size_t entries = 0;
	for (const Message& m : Parsed(sim.sent))
	{
		if (!m.IsSnapshot())
			entries += m.set.size();
	}
	O3DS_CHECK(entries <= 30 * 3 + 2);
	O3DS_CHECK(entries >= 30 * 3 - 5);
	const Value* final = sim.rx.FindValue(kSource, "env.time_of_day", "");
	O3DS_CHECK(final != nullptr && *final == Value::MakeDouble(last));
}

O3DS_TEST(ControlPub_RefusesInvalidOversizeAndOverCap)
{
	PublisherConfig pc;
	pc.max_keys = 2;
	ControlPublisher pub(kSource, "TestSender", pc);
	O3DS_CHECK(pub.SetValue("", "", Value::MakeBool(true)) == PublishResult::Invalid);
	O3DS_CHECK(pub.SetValue("bad\xFF", "", Value::MakeBool(true)) == PublishResult::Invalid);
	O3DS_CHECK(pub.SetValue("nan", "", Value::MakeDouble(std::numeric_limits<double>::quiet_NaN())) == PublishResult::Invalid);
	O3DS_CHECK(pub.FireEvent("cue", "", Value(), 0) == PublishResult::NotRunning);

	O3DS_CHECK(pub.SetValue("a", "", Value::MakeBool(true)) == PublishResult::Ok);
	O3DS_CHECK(pub.SetValue("b", "", Value::MakeBool(true)) == PublishResult::Ok);
	O3DS_CHECK(pub.SetValue("c", "", Value::MakeBool(true)) == PublishResult::TooManyKeys);
	O3DS_CHECK(pub.SetValue("a", "", Value::MakeBool(false)) == PublishResult::Ok); // existing key is fine

	// A key, target and value at their limits together exceed one message.
	const std::string key(ControlLimits::kMaxKeyBytes, 'k');
	const std::string target(ControlLimits::kMaxTargetBytes, 't');
	ControlPublisher big(kSource, "TestSender");
	O3DS_CHECK(big.SetValue(key, target, Value::MakeString(std::string(ControlLimits::kMaxStringValueBytes, 's'))) == PublishResult::Ok);
	O3DS_CHECK(big.SetValue(key, target, Value::MakeBytes(std::vector<uint8_t>(ControlLimits::kMaxBytesValueBytes))) == PublishResult::Ok);

	ControlPublisher invalidSource("", "TestSender");
	O3DS_CHECK(invalidSource.SetValue("a", "", Value::MakeBool(true)) == PublishResult::Invalid);
}

O3DS_TEST(ControlPub_RefusedMessagesAreRetriedWithinTheWindow)
{
	PublisherConfig pc;
	pc.retry_window_s = 0.5;
	ControlPublisher pub(kSource, "TestSender", pc);
	pub.Start(1000);
	O3DS_CHECK(pub.FireEvent("light.cue", "", Value::MakeInt(12), SenderUs(0.0)) == PublishResult::Ok);

	std::vector<OutgoingMessage> out;
	pub.Tick(0.0, SenderUs(0.0), 0, out);
	O3DS_CHECK_EQ(out.size(), (size_t)1);
	pub.OnSendRefused(out[0], 0.0);

	std::vector<OutgoingMessage> retry;
	pub.Tick(kDt, SenderUs(kDt), 0, retry);
	O3DS_CHECK_EQ(retry.size(), (size_t)1);
	O3DS_CHECK_EQ(retry[0].seq, out[0].seq);

	// Refused again, past the window: dropped.
	pub.OnSendRefused(retry[0], kDt);
	std::vector<OutgoingMessage> late;
	pub.Tick(1.0, SenderUs(1.0), 0, late);
	O3DS_CHECK(late.empty());
}

// ---------------------------------------------------------------------------
// Values: loss, reordering, late join, snapshots
// ---------------------------------------------------------------------------

namespace
{
	//! Why `sim` has not converged for `keys`, or an empty string.
	std::string Divergence(const Sim& sim, const std::vector<std::string>& keys)
	{
		std::ostringstream oss;
		for (const std::string& k : keys)
		{
			const Value* want = sim.pub.FindValue(k, "");
			const Value* have = sim.rx.FindValue(kSource, k, "");
			if ((want == nullptr) != (have == nullptr) || (want != nullptr && *want != *have))
				oss << k << ": sender " << (want ? "has" : "lacks") << ", receiver " << (have ? "has" : "lacks")
					<< (want && have ? " (different values)" : "") << "; ";
		}
		if (!VersionsStrictlyIncrease(sim.changes))
			oss << "a stale version was applied; ";
		const ReceiverStats& st = sim.rx.GetStats();
		if (!oss.str().empty())
			oss << "snapshots completed " << st.snapshots_completed << ", discarded " << st.snapshots_discarded
				<< ", stale " << st.items_stale << ", rate drops " << st.dropped_rate;
		return oss.str();
	}

	//! 100 keys of random sets and clears for 5 s over a link with 20 % loss
	//! and heavy reordering, then six snapshot intervals of quiet. Returns
	//! why the receiver does not match the sender (or applied something
	//! stale), or an empty string.
	//!
	//! Why six: once changes stop, a key whose last change was lost is
	//! repaired by the next snapshot part covering it that arrives. Each
	//! interval that happens with probability 1 - p (p = part loss), so a
	//! key stays wrong for k intervals with probability p^k: 4 % after two
	//! intervals at p = 0.2 (some of 100 keys will), 6e-5 after six.
	std::string ConvergesUnderLoss(uint64_t seed)
	{
		PublisherConfig pc;
		Sim sim(pc);
		sim.link.rng.seed(seed);
		sim.link.loss = 0.20;
		sim.link.latency_s = 0.02;
		sim.link.jitter_s = 0.15; // ~9 ticks of jitter: plenty of reordering
		sim.pub.Start(1000);

		std::vector<std::string> keys;
		for (int k = 0; k < 100; ++k)
			keys.push_back("param." + std::to_string(k));

		std::mt19937_64 rng(seed * 7919 + 1);
		for (int tick = 0; tick < 300; ++tick)
		{
			for (int n = 0; n < 4; ++n)
			{
				const std::string& key = keys[rng() % keys.size()];
				if (rng() % 5 == 0)
					sim.pub.ClearValue(key, "");
				else
					sim.pub.SetValue(key, "", Value::MakeDouble(static_cast<double>(rng() % 1000)));
			}
			sim.Step();
		}
		sim.Run(6.0 * pc.snapshot_interval_s + 0.5);
		return Divergence(sim, keys);
	}
}

O3DS_TEST(Control_ValuesConvergeUnderLossAndReorderingWithNoStaleApply)
{
	// Every seed, not most. The link's draws are the same on every standard
	// library, so a seed means the same losses on MSVC and on CI's GCC.
	for (uint64_t seed = 1; seed <= 20; ++seed)
	{
		const std::string why = ConvergesUnderLoss(seed);
		if (!why.empty())
			throw o3ds_test::TestFailure{ "seed " + std::to_string(seed) + ": " + why };
	}
}

O3DS_TEST(Control_LostClearIsRepairedByAnyPartThatCoversItsKey)
{
	// A three-part snapshot of {a..f}; the clear of "b" and the part holding
	// "e" are lost. The part covering "b" alone removes it, without the
	// snapshot ever completing.
	ControlReceiver rx;
	std::vector<Change> out;
	Message live = Live(1, 5);
	for (const char* k : { "a", "b", "c", "d", "e", "f" })
		live.set.push_back(SetEntry(k, 1.0, 5));
	ApplyOne(rx, live, 0.0, out);

	auto part = [](uint16_t index, std::vector<const char*> keys)
	{
		Message m = Live(1, 20 + index);
		m.snapshot_id = 1;
		m.snapshot_part = index;
		m.snapshot_parts = 3;
		m.snapshot_seq = 10;
		for (const char* k : keys)
			m.set.push_back(SetEntry(k, 1.0, 5));
		return m;
	};
	out.clear();
	ApplyOne(rx, part(0, { "a", "c" }), 0.1, out); // "b" was cleared at 7, before capture
	O3DS_CHECK(rx.FindValue(kSource, "b", "") == nullptr);
	O3DS_CHECK_EQ(Count(out, Change::Kind::ValueCleared), (size_t)1);
	ApplyOne(rx, part(2, { "f" }), 0.2, out);
	for (const char* k : { "a", "c", "d", "e", "f" })
		O3DS_CHECK(rx.FindValue(kSource, k, "") != nullptr); // "d", "e" sit in the missing part's range
	O3DS_CHECK_EQ(rx.GetStats().snapshots_completed, (uint64_t)0);
}

O3DS_TEST(Control_KeysDroppedAtTheKeyCapRecoverFromLaterSnapshots)
{
	// Publisher and receiver at the same key limit. Clears leave tombstones
	// that fill the receiver's cap, so new keys are dropped at first; a
	// later snapshot must still deliver them (the floor must not reject a
	// snapshot entry whose version predates it).
	PublisherConfig pc;
	pc.max_keys = 64;
	ReceiverConfig rc;
	rc.max_keys_per_source = 64;
	Sim sim(pc, rc);
	sim.pub.Start(1000);
	std::vector<std::string> keys;
	for (int k = 0; k < 64; ++k)
	{
		keys.push_back("k" + std::to_string(100 + k));
		sim.pub.SetValue(keys.back(), "", Value::MakeInt(k));
	}
	sim.Run(1.5);
	O3DS_CHECK(sim.Converged(keys));

	for (int k = 0; k < 10; ++k)
		sim.pub.ClearValue(keys[k], "");
	sim.Step();
	for (int k = 0; k < 10; ++k)
	{
		keys.push_back("n" + std::to_string(100 + k));
		O3DS_CHECK(sim.pub.SetValue(keys.back(), "", Value::MakeInt(k)) == PublishResult::Ok);
	}
	sim.Step();
	O3DS_CHECK(sim.rx.GetStats().items_key_cap > 0);

	sim.Run(4.0);
	O3DS_CHECK(sim.Converged(keys));
}

O3DS_TEST(Control_ReorderedSetAfterClearIsRejectedByTombstone)
{
	ControlReceiver rx;
	std::vector<Change> out;

	Message set5 = Live(1, 5);
	set5.set.push_back(SetEntry("env.fog_density", 0.4, 5));
	Message clear7 = Live(1, 7);
	Clear c;
	c.key = "env.fog_density";
	c.version = 7;
	clear7.clear.push_back(c);

	ApplyOne(rx, clear7, 0.0, out); // the clear overtakes the set
	ApplyOne(rx, set5, 0.1, out);
	O3DS_CHECK(rx.FindValue(kSource, "env.fog_density", "") == nullptr);
	O3DS_CHECK_EQ(rx.GetStats().items_stale, (uint64_t)1);
	O3DS_CHECK(out.empty());

	Message set9 = Live(1, 9);
	set9.set.push_back(SetEntry("env.fog_density", 0.6, 9));
	ApplyOne(rx, set9, 0.2, out);
	O3DS_CHECK(rx.FindValue(kSource, "env.fog_density", "") != nullptr);
	O3DS_CHECK_EQ(Count(out, Change::Kind::ValueChanged), (size_t)1);
}

O3DS_TEST(Control_StaleSetAfterSnapshotPrunedTombstoneIsRejectedByFloor)
{
	ControlReceiver rx;
	std::vector<Change> out;

	Message set5 = Live(1, 5);
	set5.set.push_back(SetEntry("k", 1.0, 5));
	Message clear7 = Live(1, 7);
	Clear c;
	c.key = "k";
	c.version = 7;
	clear7.clear.push_back(c);
	Message snap = Live(1, 11); // empty table captured at 10
	snap.snapshot_id = 1;
	snap.snapshot_parts = 1;
	snap.snapshot_seq = 10;

	ApplyOne(rx, clear7, 0.0, out);
	ApplyOne(rx, snap, 0.1, out);  // prunes the tombstone; the floor takes over
	ApplyOne(rx, set5, 0.2, out);  // a very late retry of the old set
	O3DS_CHECK(rx.FindValue(kSource, "k", "") == nullptr);
	O3DS_CHECK(out.empty());
}

O3DS_TEST(Control_LateJoinerGetsTheTableAndNoOldEvents)
{
	PublisherConfig pc;
	Sim sim(pc);
	sim.pub.Start(1000);
	sim.link.connected = false;

	std::vector<std::string> keys;
	for (int k = 0; k < 40; ++k)
	{
		keys.push_back("scene." + std::to_string(k));
		sim.pub.SetValue(keys.back(), "", Value::MakeInt(k));
	}
	for (int tick = 0; tick < 300; ++tick) // 5 s with cues firing, receiver absent
	{
		if (tick % 10 == 0)
			sim.pub.FireEvent("vfx.spark", "", Value(), SenderUs(sim.now));
		sim.Step();
	}

	const uint64_t joinUs = SenderUs(sim.now);
	sim.link.connected = true;
	sim.Run(pc.snapshot_interval_s + 0.6); // one interval plus pacing

	O3DS_CHECK(sim.Converged(keys));
	for (const Change& c : sim.changes)
	{
		if (c.kind == Change::Kind::Event)
			O3DS_CHECK(c.sender_time_us >= joinUs);
	}
}

O3DS_TEST(Control_LostClearIsRepairedByTheNextSnapshot)
{
	PublisherConfig pc;
	Sim sim(pc);
	sim.pub.Start(1000);
	sim.pub.SetValue("light.on", "", Value::MakeBool(true));
	sim.pub.SetValue("light.color", "", Value::MakeColor({ 1.0f, 0.5f, 0.0f, 1.0f }));
	sim.Run(0.5);
	O3DS_CHECK(sim.rx.FindValue(kSource, "light.on", "") != nullptr);

	sim.link.drop = [](const Message& m) { return !m.clear.empty(); };
	sim.pub.ClearValue("light.on", "");
	sim.Step();
	O3DS_CHECK(sim.rx.FindValue(kSource, "light.on", "") != nullptr); // the clear was lost

	sim.Run(pc.snapshot_interval_s + 0.6);
	O3DS_CHECK(sim.rx.FindValue(kSource, "light.on", "") == nullptr);
	O3DS_CHECK(sim.rx.FindValue(kSource, "light.color", "") != nullptr);
	O3DS_CHECK(VersionsStrictlyIncrease(sim.changes));
}

O3DS_TEST(Control_IncompleteSnapshotRemovesNothing)
{
	ReceiverConfig rc;
	rc.incomplete_snapshot_timeout_s = 1.0;
	ControlReceiver rx(rc);
	std::vector<Change> out;

	Message live = Live(1, 5);
	live.set.push_back(SetEntry("a", 1.0, 5));
	live.set.push_back(SetEntry("b", 2.0, 5));
	ApplyOne(rx, live, 0.0, out);

	// Snapshot of {a, b} in two parts; only the part holding b arrives.
	Message part1 = Live(1, 11);
	part1.snapshot_id = 1;
	part1.snapshot_part = 1;
	part1.snapshot_parts = 2;
	part1.snapshot_seq = 10;
	part1.set.push_back(SetEntry("b", 2.0, 5));
	ApplyOne(rx, part1, 0.1, out);

	out.clear();
	rx.Tick(5.0, out);
	O3DS_CHECK(rx.FindValue(kSource, "a", "") != nullptr);
	O3DS_CHECK(out.empty());
	O3DS_CHECK_EQ(rx.GetStats().snapshots_completed, (uint64_t)0);
	O3DS_CHECK_EQ(rx.GetStats().snapshots_discarded, (uint64_t)1);
}

O3DS_TEST(Control_SenderRestartWithUnchangedTableDoesNotFlicker)
{
	Sim sim;
	sim.pub.Start(1000);
	std::vector<std::string> keys;
	for (int k = 0; k < 50; ++k)
	{
		keys.push_back("env." + std::to_string(k));
		sim.pub.SetValue(keys.back(), "", Value::MakeDouble(k * 0.5));
	}
	sim.Run(2.0);
	O3DS_CHECK(sim.Converged(keys));

	// Stop/Start, as PostEditChangeProperty does: same table, new epoch.
	sim.changes.clear();
	sim.pub.Stop();
	sim.pub.Start(1000);
	O3DS_CHECK(sim.pub.GetEpoch() == 1001);
	sim.Run(2.0);
	O3DS_CHECK(sim.changes.empty());
	O3DS_CHECK(sim.Converged(keys));

	// Restart with three changed values and one cleared: exactly those.
	sim.changes.clear();
	sim.pub.Stop();
	sim.pub.SetValue("env.1", "", Value::MakeDouble(100.0));
	sim.pub.SetValue("env.2", "", Value::MakeDouble(200.0));
	sim.pub.SetValue("env.3", "", Value::MakeDouble(300.0));
	sim.pub.ClearValue("env.4", "");
	sim.pub.Start(1000);
	sim.Run(2.0);
	O3DS_CHECK_EQ(Count(sim.changes, Change::Kind::ValueChanged), (size_t)3);
	O3DS_CHECK_EQ(Count(sim.changes, Change::Kind::ValueCleared), (size_t)1);
	O3DS_CHECK(sim.Converged(keys));
}

O3DS_TEST(Control_LowerEpochIsIgnored)
{
	ControlReceiver rx;
	std::vector<Change> out;
	Message now = Live(5, 10);
	now.set.push_back(SetEntry("k", 1.0, 10));
	ApplyOne(rx, now, 0.0, out);

	Message old = Live(4, 50);
	old.set.push_back(SetEntry("k", 9.0, 50));
	ApplyOne(rx, old, 0.1, out);
	O3DS_CHECK(*rx.FindValue(kSource, "k", "") == Value::MakeDouble(1.0));
	O3DS_CHECK_EQ(rx.GetStats().dropped_old_epoch, (uint64_t)1);
}

O3DS_TEST(Control_TargetsAreSeparateKeys)
{
	Sim sim;
	sim.pub.Start(1000);
	sim.pub.SetValue("char.emotion", "Hero", Value::MakeName("joy"));
	sim.pub.SetValue("char.emotion", "Villain", Value::MakeName("anger"));
	sim.Run(0.2);
	O3DS_CHECK(*sim.rx.FindValue(kSource, "char.emotion", "Hero") == Value::MakeName("joy"));
	O3DS_CHECK(*sim.rx.FindValue(kSource, "char.emotion", "Villain") == Value::MakeName("anger"));
	O3DS_CHECK(sim.rx.FindValue(kSource, "char.emotion", "") == nullptr);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

O3DS_TEST(Control_EventWithRedundancyIsDeliveredExactlyOnceWhenLossless)
{
	PublisherConfig pc;
	pc.event_redundancy = 3;
	Sim sim(pc);
	sim.pub.Start(1000);
	O3DS_CHECK(sim.pub.FireEvent("audio.sting", "", Value::MakeName("hit_01"), SenderUs(sim.now)) == PublishResult::Ok);
	sim.Run(0.5);

	size_t copiesSent = 0;
	for (const Message& m : Parsed(sim.sent))
		copiesSent += m.events.size();
	O3DS_CHECK_EQ(copiesSent, (size_t)3);
	O3DS_CHECK_EQ(Count(sim.changes, Change::Kind::Event), (size_t)1);
	O3DS_CHECK_EQ(sim.rx.GetStats().events_duplicate, (uint64_t)2);
	O3DS_CHECK(sim.changes[0].value == Value::MakeName("hit_01"));
}

O3DS_TEST(Control_EventsAt20PercentLossWithRedundancy3MatchTheory)
{
	// Three copies in three messages, each lost independently with p = 0.2:
	// an event is lost only if all three are, so expected delivery is
	// 1 - 0.2^3 = 99.2 %, sd ~0.09 % over 10,000 events. The bar is theory
	// minus 4 sd, with zero duplicates and nothing expired.
	for (uint64_t seed : { 11ull, 22ull, 33ull })
	{
	PublisherConfig pc;
	pc.event_redundancy = 3;
	Sim sim(pc);
	sim.link.rng.seed(seed);
	sim.link.loss = 0.20;
	sim.link.latency_s = 0.01;
	sim.pub.Start(1000);

	const int kEvents = 10000;
	int fired = 0;
	while (fired < kEvents)
	{
		for (int n = 0; n < 2 && fired < kEvents; ++n, ++fired)
			O3DS_CHECK(sim.pub.FireEvent("cue." + std::to_string(fired % 7), "", Value::MakeInt(fired), SenderUs(sim.now)) == PublishResult::Ok);
		sim.Step();
	}
	sim.Run(1.0);

	std::set<int64_t> seen;
	size_t events = 0;
	for (const Change& c : sim.changes)
	{
		if (c.kind != Change::Kind::Event)
			continue;
		++events;
		seen.insert(c.value.i);
	}
	O3DS_CHECK_EQ(seen.size(), events); // zero duplicates
	if (events < static_cast<size_t>(kEvents * 0.988))
	{
		const ReceiverStats& st = sim.rx.GetStats();
		throw o3ds_test::TestFailure{ "seed " + std::to_string(seed) + ": delivered " + std::to_string(events) + " of "
			+ std::to_string(kEvents) + " (rate drops " + std::to_string(st.dropped_rate) + ", expired "
			+ std::to_string(st.events_expired) + ")" };
	}
	O3DS_CHECK_EQ(sim.rx.GetStats().events_expired, (uint64_t)0);
	}
}

O3DS_TEST(Control_ExpiredEventIsDroppedOnTheSendersClock)
{
	// The TTL never reads the receiver's clock: the same input gives the
	// same result whatever nowS the receiver passes in.
	for (double receiverClockOffset : { 0.0, 3600.0, -3600.0 })
	{
		ControlReceiver rx;
		std::vector<Change> out;

		Message newer = Live(1, 20);
		newer.sender_time_us = 10000000; // t = 10 s
		ApplyOne(rx, newer, 100.0 + receiverClockOffset, out);

		Message stale = Live(1, 21);
		stale.sender_time_us = 10000000;
		Event ev;
		ev.event_id = 1;
		ev.name = "vfx.explosion";
		ev.ttl_ms = 500;
		ev.time_us = 9000000; // fired at 9 s: 1 s older than the newest message
		stale.events.push_back(ev);
		ApplyOne(rx, stale, 100.1 + receiverClockOffset, out);

		Event fresh = ev;
		fresh.event_id = 2;
		fresh.time_us = 9800000; // 0.2 s old: within its TTL
		Message ok = Live(1, 22);
		ok.sender_time_us = 10000000;
		ok.events.push_back(fresh);
		ApplyOne(rx, ok, 100.2 + receiverClockOffset, out);

		O3DS_CHECK_EQ(Count(out, Change::Kind::Event), (size_t)1);
		O3DS_CHECK_EQ(out.back().event_id, (uint64_t)2);
		O3DS_CHECK_EQ(rx.GetStats().events_expired, (uint64_t)1);
	}
}

O3DS_TEST(Control_DedupeWindowSurvivesWrapAround)
{
	ReceiverConfig rc;
	rc.dedupe_window = 4;
	ControlReceiver rx(rc);
	std::vector<Change> out;

	auto eventMsg = [](uint64_t seq, uint64_t id)
	{
		Message m = Live(1, seq);
		Event ev;
		ev.event_id = id;
		ev.name = "cue";
		m.events.push_back(ev);
		return m;
	};
	for (uint64_t id = 1; id <= 10; ++id)
		ApplyOne(rx, eventMsg(id, id), 0.0, out);
	O3DS_CHECK_EQ(Count(out, Change::Kind::Event), (size_t)10);

	for (uint64_t id = 7; id <= 10; ++id) // the last four are remembered
		ApplyOne(rx, eventMsg(20 + id, id), 0.0, out);
	O3DS_CHECK_EQ(Count(out, Change::Kind::Event), (size_t)10);
	O3DS_CHECK_EQ(rx.GetStats().events_duplicate, (uint64_t)4);
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

O3DS_TEST(Control_ByteRateFloodIsCappedAndCounted)
{
	ReceiverConfig rc;
	rc.max_live_bytes_per_s = 10000.0;
	ControlReceiver rx(rc);
	std::vector<Change> out;

	// Ten times the budget over one second, in 100-byte messages.
	uint64_t accepted = 0;
	for (int k = 0; k < 1000; ++k)
	{
		Message m = Live(1, static_cast<uint64_t>(k + 1));
		m.set.push_back(SetEntry("k", k, static_cast<uint64_t>(k + 1)));
		rx.Apply(m, 100, k * 0.001, out);
	}
	accepted = rx.GetStats().messages_accepted;
	O3DS_CHECK(accepted <= 10000 / 100 + 10000 / 100 + 1); // one-second burst plus one second of refill
	O3DS_CHECK(rx.GetStats().dropped_rate >= 1000 - accepted);
}

O3DS_TEST(Control_KeyCapHolds)
{
	ReceiverConfig rc;
	rc.max_keys_per_source = 8;
	ControlReceiver rx(rc);
	std::vector<Change> out;
	for (uint64_t k = 1; k <= 20; ++k)
	{
		Message m = Live(1, k);
		m.set.push_back(SetEntry("k" + std::to_string(k), 1.0, k));
		ApplyOne(rx, m, 0.0, out);
	}
	O3DS_CHECK_EQ(rx.GetValues(kSource).size(), (size_t)8);
	O3DS_CHECK_EQ(rx.GetStats().items_key_cap, (uint64_t)12);
}

O3DS_TEST(Control_AllowlistFiltersByPrefix)
{
	ReceiverConfig rc;
	rc.allow_prefixes = { "env.", "light." };
	Sim sim(PublisherConfig(), rc);
	sim.pub.Start(1000);
	sim.pub.SetValue("env.fog_density", "", Value::MakeDouble(0.2));
	sim.pub.SetValue("admin.shutdown", "", Value::MakeBool(true));
	sim.pub.FireEvent("light.cue", "", Value::MakeInt(4), SenderUs(sim.now));
	sim.pub.FireEvent("debug.crash", "", Value(), SenderUs(sim.now));
	sim.Run(0.2);

	O3DS_CHECK(sim.rx.FindValue(kSource, "env.fog_density", "") != nullptr);
	O3DS_CHECK(sim.rx.FindValue(kSource, "admin.shutdown", "") == nullptr);
	O3DS_CHECK_EQ(Count(sim.changes, Change::Kind::Event), (size_t)1);
	O3DS_CHECK(sim.rx.GetStats().items_not_allowed >= 2);
}

O3DS_TEST(Control_SourceCapAndIdlePruning)
{
	ReceiverConfig rc;
	rc.max_sources = 2;
	rc.source_idle_timeout_s = 5.0;
	ControlReceiver rx(rc);
	std::vector<Change> out;
	for (int s = 0; s < 3; ++s)
	{
		Message m = Live(1, 1);
		m.source_id = "source" + std::to_string(s);
		m.set.push_back(SetEntry("k", 1.0, 1));
		ApplyOne(rx, m, 0.0, out);
	}
	O3DS_CHECK_EQ(rx.NumSources(), (size_t)2);
	O3DS_CHECK_EQ(rx.GetStats().dropped_source_cap, (uint64_t)1);

	out.clear();
	rx.Tick(10.0, out);
	O3DS_CHECK_EQ(rx.NumSources(), (size_t)0);
	O3DS_CHECK_EQ(Count(out, Change::Kind::ValueCleared), (size_t)2);
}

O3DS_TEST(Control_MaxLoadPublisherFitsADefaultReceiver)
{
	// Worst case a publisher can produce: a full table of the largest
	// entries, every key changing every tick, events with redundancy 3.
	// A receiver with default settings must drop nothing for rate and
	// complete every snapshot (ADR 0011, Verification "Load").
	PublisherConfig pc;
	pc.event_redundancy = 3;
	Sim sim(pc);
	sim.pub.Start(1000);

	const std::string bigValue(ControlLimits::kMaxStringValueBytes, 'v');
	std::vector<std::string> keys;
	for (size_t k = 0; k < ControlLimits::kMaxKeysPerSource; ++k)
	{
		std::string key = "load." + std::to_string(k);
		key.resize(ControlLimits::kMaxKeyBytes, '_');
		keys.push_back(key);
		O3DS_CHECK(sim.pub.SetValue(key, "", Value::MakeString(bigValue)) == PublishResult::Ok);
	}

	std::mt19937_64 rng(99);
	for (int tick = 0; tick < 60 * 12; ++tick)
	{
		for (int n = 0; n < 20; ++n)
		{
			std::string value = bigValue;
			value[0] = static_cast<char>('a' + rng() % 26);
			sim.pub.SetValue(keys[rng() % keys.size()], "", Value::MakeString(value));
		}
		if (tick % 6 == 0)
			sim.pub.FireEvent("cue", "", Value(), SenderUs(sim.now));
		sim.Step();
	}

	const ReceiverStats& stats = sim.rx.GetStats();
	O3DS_CHECK_EQ(stats.dropped_rate, (uint64_t)0);
	O3DS_CHECK_EQ(stats.snapshots_discarded, (uint64_t)0);
	O3DS_CHECK(stats.snapshots_completed >= 1);
	O3DS_CHECK_EQ(sim.rx.GetValues(kSource).size(), keys.size());
}

// ---------------------------------------------------------------------------
// Alignment
// ---------------------------------------------------------------------------

namespace
{
	Change TimedChange(const std::string& source, uint64_t senderUs, const std::string& name)
	{
		Change c;
		c.kind = Change::Kind::Event;
		c.source_id = source;
		c.name = name;
		c.sender_time_us = senderUs;
		return c;
	}
}

O3DS_TEST(ControlAligner_HoldsUntilTheMocapStreamReachesTheCue)
{
	ControlAligner aligner(0.5);
	aligner.Push(TimedChange("A", 1000000, "first"), 0.0);
	aligner.Push(TimedChange("A", 1100000, "second"), 0.0);

	uint64_t presenting = 950000; // the pose shown is 50 ms behind the cue
	auto present = [&presenting](const std::string&, uint64_t& outUs) { outUs = presenting; return true; };

	std::vector<Change> out;
	aligner.Release(0.05, present, out);
	O3DS_CHECK(out.empty());

	presenting = 1000000;
	aligner.Release(0.1, present, out);
	O3DS_CHECK_EQ(out.size(), (size_t)1);
	O3DS_CHECK_EQ(out[0].name, std::string("first"));

	presenting = 1200000;
	aligner.Release(0.2, present, out);
	O3DS_CHECK_EQ(out.size(), (size_t)2);
	O3DS_CHECK_EQ(out[1].name, std::string("second"));
	O3DS_CHECK_EQ(aligner.GetStats().released_aligned, (uint64_t)2);
}

O3DS_TEST(ControlAligner_NoMocapStreamMeansNoWait)
{
	ControlAligner aligner(0.5);
	aligner.Push(TimedChange("control-only", 5000000, "cue"), 0.0);
	std::vector<Change> out;
	aligner.Release(0.0, [](const std::string&, uint64_t&) { return false; }, out);
	O3DS_CHECK_EQ(out.size(), (size_t)1);
	O3DS_CHECK_EQ(aligner.GetStats().released_unaligned, (uint64_t)1);
}

O3DS_TEST(ControlAligner_CapReleasesLateNeverDrops)
{
	ControlAligner aligner(0.5);
	aligner.Push(TimedChange("A", 9000000, "cue"), 0.0);
	auto stuck = [](const std::string&, uint64_t& outUs) { outUs = 1000000; return true; };
	std::vector<Change> out;
	aligner.Release(0.49, stuck, out);
	O3DS_CHECK(out.empty());
	aligner.Release(0.5, stuck, out);
	O3DS_CHECK_EQ(out.size(), (size_t)1);
	O3DS_CHECK_EQ(aligner.GetStats().released_late, (uint64_t)1);
	O3DS_CHECK_EQ(aligner.NumHeld(), (size_t)0);
}

O3DS_TEST(ControlAligner_PreservesOrderWithinASourceAndIsolatesSources)
{
	ControlAligner aligner(10.0);
	// An earlier-pushed change with a later time holds back the one after it.
	aligner.Push(TimedChange("A", 2000000, "a1"), 0.0);
	aligner.Push(TimedChange("A", 1000000, "a2"), 0.0);
	aligner.Push(TimedChange("B", 1000000, "b1"), 0.0);

	std::vector<Change> out;
	aligner.Release(0.1, [](const std::string&, uint64_t& outUs) { outUs = 1500000; return true; }, out);
	O3DS_CHECK_EQ(out.size(), (size_t)1);
	O3DS_CHECK_EQ(out[0].name, std::string("b1"));

	aligner.Release(0.2, [](const std::string&, uint64_t& outUs) { outUs = 2000000; return true; }, out);
	O3DS_CHECK_EQ(out.size(), (size_t)3);
	O3DS_CHECK_EQ(out[1].name, std::string("a1"));
	O3DS_CHECK_EQ(out[2].name, std::string("a2"));
}

O3DS_TEST(ControlAligner_FlushReleasesEverything)
{
	ControlAligner aligner(10.0);
	aligner.Push(TimedChange("A", 9000000, "x"), 0.0);
	aligner.Push(TimedChange("B", 9000000, "y"), 0.0);
	std::vector<Change> out;
	aligner.Flush(out);
	O3DS_CHECK_EQ(out.size(), (size_t)2);
	O3DS_CHECK_EQ(aligner.NumHeld(), (size_t)0);
}
