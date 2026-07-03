// Acceptance matrix for src/o3ds/reorder_gate.h, per the roadmap doc's A1
// spec (docs/roadmap/resilient-streaming-and-motion-prediction.md, §3/A1.c).
#include "test_framework.h"

#include "o3ds/reorder_gate.h"

using namespace O3DS;

namespace
{
	// Frames carry no real payload in these tests - only the sequencing
	// metadata under test. `bytes` holds the seq as a marker so assertions
	// can identify which frame was delivered without depending on Frame's
	// move semantics leaving seq intact (it does, but this is self-evident
	// either way when reading a failure message).
	Frame MakeFrame(uint64_t seq, uint32_t epoch = 0)
	{
		Frame f;
		f.seq = seq;
		f.epoch = epoch;
		f.wallclock_us = 0;
		return f;
	}

	struct Delivered
	{
		std::vector<uint64_t> seqs;

		std::function<void(Frame&&)> Sink()
		{
			return [this](Frame&& f) { seqs.push_back(f.seq); };
		}
	};
}

O3DS_TEST(ReorderGate_InOrder_AllDelivered)
{
	ReorderGate gate;
	Delivered out;

	gate.Push(MakeFrame(1), 0.0, out.Sink());
	gate.Push(MakeFrame(2), 0.0, out.Sink());
	gate.Push(MakeFrame(3), 0.0, out.Sink());

	O3DS_CHECK_EQ(out.seqs.size(), (size_t)3);
	O3DS_CHECK_EQ(out.seqs[0], (uint64_t)1);
	O3DS_CHECK_EQ(out.seqs[1], (uint64_t)2);
	O3DS_CHECK_EQ(out.seqs[2], (uint64_t)3);
	O3DS_CHECK_EQ(gate.Stats().lost, (uint64_t)0);
	O3DS_CHECK_EQ(gate.Stats().reordered, (uint64_t)0);
	O3DS_CHECK_EQ(gate.Stats().delivered, (uint64_t)3);
}

O3DS_TEST(ReorderGate_ReorderRecovered)
{
	// 100, 102, 101 -> emit 100,101,102; reordered==1; lost==0
	ReorderGate gate;
	Delivered out;

	gate.Push(MakeFrame(100), 0.0, out.Sink());
	gate.Push(MakeFrame(102), 0.0, out.Sink());
	gate.Push(MakeFrame(101), 0.0, out.Sink());

	O3DS_CHECK_EQ(out.seqs.size(), (size_t)3);
	O3DS_CHECK_EQ(out.seqs[0], (uint64_t)100);
	O3DS_CHECK_EQ(out.seqs[1], (uint64_t)101);
	O3DS_CHECK_EQ(out.seqs[2], (uint64_t)102);
	O3DS_CHECK_EQ(gate.Stats().reordered, (uint64_t)1);
	O3DS_CHECK_EQ(gate.Stats().lost, (uint64_t)0);
}

O3DS_TEST(ReorderGate_StaleAfterSuccessor_Dropped)
{
	// 100, 101, 99 -> emit 100,101; 99 dropped stale; lost==0
	ReorderGate gate;
	Delivered out;

	gate.Push(MakeFrame(100), 0.0, out.Sink());
	gate.Push(MakeFrame(101), 0.0, out.Sink());
	gate.Push(MakeFrame(99), 0.0, out.Sink());

	O3DS_CHECK_EQ(out.seqs.size(), (size_t)2);
	O3DS_CHECK_EQ(out.seqs[0], (uint64_t)100);
	O3DS_CHECK_EQ(out.seqs[1], (uint64_t)101);
	O3DS_CHECK_EQ(gate.Stats().stale_dropped, (uint64_t)1);
	O3DS_CHECK_EQ(gate.Stats().dup_dropped, (uint64_t)0);
	O3DS_CHECK_EQ(gate.Stats().lost, (uint64_t)0);
}

O3DS_TEST(ReorderGate_Duplicate_Dropped)
{
	ReorderGate gate;
	Delivered out;

	gate.Push(MakeFrame(100), 0.0, out.Sink());
	gate.Push(MakeFrame(100), 0.0, out.Sink());

	O3DS_CHECK_EQ(out.seqs.size(), (size_t)1);
	O3DS_CHECK_EQ(gate.Stats().dup_dropped, (uint64_t)1);
	O3DS_CHECK_EQ(gate.Stats().stale_dropped, (uint64_t)0);
}

O3DS_TEST(ReorderGate_GapTimeout_LostCounted)
{
	// 100, 102, then advance the clock past max_delay_s with no 101 -> emit
	// 102, lost==1.
	ReorderGate::Config config;
	config.max_delay_s = 0.05;
	ReorderGate gate(config);
	Delivered out;

	gate.Push(MakeFrame(100), 0.0, out.Sink());
	gate.Push(MakeFrame(102), 0.0, out.Sink());
	O3DS_CHECK_EQ(out.seqs.size(), (size_t)1); // 102 still held, waiting on 101

	gate.Flush(0.1, out.Sink()); // now well past max_delay_s

	O3DS_CHECK_EQ(out.seqs.size(), (size_t)2);
	O3DS_CHECK_EQ(out.seqs[1], (uint64_t)102);
	O3DS_CHECK_EQ(gate.Stats().lost, (uint64_t)1);
}

O3DS_TEST(ReorderGate_WindowOverflow_FlushesPastHole)
{
	// A persistent gap that fills the buffering window must flush even
	// before max_delay_s elapses.
	ReorderGate::Config config;
	config.max_window = 4;
	config.max_delay_s = 1000.0; // effectively disabled; window is the trigger here
	ReorderGate gate(config);
	Delivered out;

	gate.Push(MakeFrame(100), 0.0, out.Sink());
	// 101 is missing. Buffer 102..105 (4 entries == max_window) without ever
	// sending 101 or advancing the clock meaningfully.
	gate.Push(MakeFrame(102), 0.0, out.Sink());
	gate.Push(MakeFrame(103), 0.0, out.Sink());
	gate.Push(MakeFrame(104), 0.0, out.Sink());
	gate.Push(MakeFrame(105), 0.0, out.Sink());

	// The window is full, so the gap must have already been given up on
	// without needing an explicit Flush().
	O3DS_CHECK(out.seqs.size() >= (size_t)2); // at least 100 and the unblocked 102
	O3DS_CHECK_EQ(gate.Stats().lost, (uint64_t)1); // exactly seq 101
}

O3DS_TEST(ReorderGate_LegacyTxSeqZero_BypassesGate)
{
	ReorderGate gate;
	Delivered out;

	// seq==0 must pass straight through regardless of arrival order and
	// must not perturb gate state for subsequently-sequenced frames.
	gate.Push(MakeFrame(0), 0.0, out.Sink());
	gate.Push(MakeFrame(0), 0.0, out.Sink());
	gate.Push(MakeFrame(1), 0.0, out.Sink());
	gate.Push(MakeFrame(2), 0.0, out.Sink());

	O3DS_CHECK_EQ(out.seqs.size(), (size_t)4);
	O3DS_CHECK_EQ(gate.Stats().delivered, (uint64_t)2); // only the two sequenced frames counted
	O3DS_CHECK_EQ(gate.Stats().dup_dropped, (uint64_t)0);
}

O3DS_TEST(ReorderGate_GiveUpDrain_DoesNotInflateReorderedStat)
{
	// Regression test for a review finding: frames drained after
	// CheckTimeouts gives up on a lost gap were being counted as
	// `reordered`, even when they arrived in perfectly good order relative
	// to each other - they were just waiting behind a hole, not reordered.
	ReorderGate::Config config;
	config.max_window = 4;
	ReorderGate gate(config);
	Delivered out;

	gate.Push(MakeFrame(100), 0.0, out.Sink()); // delivered normally
	// 101 is missing. 102..105 arrive in strict ascending order and fill
	// the window, forcing a give-up on the 101 gap.
	gate.Push(MakeFrame(102), 0.0, out.Sink());
	gate.Push(MakeFrame(103), 0.0, out.Sink());
	gate.Push(MakeFrame(104), 0.0, out.Sink());
	gate.Push(MakeFrame(105), 0.0, out.Sink());

	O3DS_CHECK_EQ(gate.Stats().lost, (uint64_t)1);       // exactly seq 101
	O3DS_CHECK_EQ(gate.Stats().reordered, (uint64_t)0);  // nothing here was actually reordered
}

O3DS_TEST(ReorderGate_DuplicateOfPendingFrame_CountedAsDup)
{
	// Regression test for a review finding: a resend of a seq that's
	// already sitting in the pending-gap buffer was silently dropped by
	// map::emplace with no stats update at all.
	ReorderGate gate;
	Delivered out;

	gate.Push(MakeFrame(100), 0.0, out.Sink()); // delivered
	gate.Push(MakeFrame(102), 0.0, out.Sink()); // buffered, waiting on 101
	gate.Push(MakeFrame(102), 0.0, out.Sink()); // a duplicate of the buffered frame

	O3DS_CHECK_EQ(gate.Stats().dup_dropped, (uint64_t)1);
}

O3DS_TEST(ReorderGate_OldEpochStraggler_DroppedNotMisappliedAsRestart)
{
	// Regression test for the review's critical finding: a frame from an
	// OLDER epoch, reordered in flight to arrive after the new session has
	// already started, must not be mistaken for another restart. Treating
	// frame_epoch as "changed vs not" instead of an ORDERED value would
	// apply this stale frame mid-stream and, because it would also drag
	// mLastEpoch backward, make every subsequent legitimate frame look like
	// yet another false restart.
	ReorderGate gate;
	Delivered out;

	gate.Push(MakeFrame(500, /*epoch*/ 1), 0.0, out.Sink());
	gate.Push(MakeFrame(501, /*epoch*/ 1), 0.0, out.Sink());
	gate.Push(MakeFrame(1, /*epoch*/ 2), 0.0, out.Sink());   // genuine restart
	gate.Push(MakeFrame(2, /*epoch*/ 2), 0.0, out.Sink());
	gate.Push(MakeFrame(502, /*epoch*/ 1), 0.0, out.Sink()); // stale straggler from epoch 1
	gate.Push(MakeFrame(3, /*epoch*/ 2), 0.0, out.Sink());   // must NOT look like another restart
	gate.Push(MakeFrame(4, /*epoch*/ 2), 0.0, out.Sink());

	// 502 must never be delivered - it's stale data from a dead session.
	O3DS_CHECK_EQ(out.seqs.size(), (size_t)6);
	O3DS_CHECK_EQ(out.seqs[0], (uint64_t)500);
	O3DS_CHECK_EQ(out.seqs[1], (uint64_t)501);
	O3DS_CHECK_EQ(out.seqs[2], (uint64_t)1);
	O3DS_CHECK_EQ(out.seqs[3], (uint64_t)2);
	O3DS_CHECK_EQ(out.seqs[4], (uint64_t)3);
	O3DS_CHECK_EQ(out.seqs[5], (uint64_t)4);
	O3DS_CHECK_EQ(gate.Stats().stale_dropped, (uint64_t)1); // the 502 straggler
}

O3DS_TEST(ReorderGate_OldEpochStraggler_DoesNotWipeBufferedNewSessionFrames)
{
	// Worse variant of the above: if the straggler were still (incorrectly)
	// treated as a restart, its state-clearing would silently discard any
	// legitimately-buffered new-session frame, not just misapply itself.
	ReorderGate gate;
	Delivered out;

	gate.Push(MakeFrame(500, /*epoch*/ 1), 0.0, out.Sink());
	gate.Push(MakeFrame(1, /*epoch*/ 2), 0.0, out.Sink());
	// 3 arrives before 2, so 3 sits buffered in the pending-gap map.
	gate.Push(MakeFrame(3, /*epoch*/ 2), 0.0, out.Sink());
	// An old-epoch straggler must not wipe that buffered frame.
	gate.Push(MakeFrame(600, /*epoch*/ 1), 0.0, out.Sink());
	gate.Push(MakeFrame(2, /*epoch*/ 2), 0.0, out.Sink()); // unblocks the buffered 3

	O3DS_CHECK_EQ(out.seqs.size(), (size_t)4);
	O3DS_CHECK_EQ(out.seqs[0], (uint64_t)500);
	O3DS_CHECK_EQ(out.seqs[1], (uint64_t)1);
	O3DS_CHECK_EQ(out.seqs[2], (uint64_t)2);
	O3DS_CHECK_EQ(out.seqs[3], (uint64_t)3); // proves 3 survived the straggler, not lost
	O3DS_CHECK_EQ(gate.Stats().lost, (uint64_t)0);
}

O3DS_TEST(ReorderGate_RestartViaEpoch_Rebaselines)
{
	ReorderGate gate;
	Delivered out;

	gate.Push(MakeFrame(500, /*epoch*/ 1), 0.0, out.Sink());
	gate.Push(MakeFrame(501, /*epoch*/ 1), 0.0, out.Sink());

	// Publisher restarts: its SequenceCounter resets to 1 and frame_epoch
	// bumps. A naive backward-jump-only gate would treat seq 1 as ancient
	// stale traffic; the epoch change must instead trigger a clean
	// re-baseline and deliver seq 1 immediately.
	gate.Push(MakeFrame(1, /*epoch*/ 2), 0.0, out.Sink());
	gate.Push(MakeFrame(2, /*epoch*/ 2), 0.0, out.Sink());

	O3DS_CHECK_EQ(out.seqs.size(), (size_t)4);
	O3DS_CHECK_EQ(out.seqs[2], (uint64_t)1);
	O3DS_CHECK_EQ(out.seqs[3], (uint64_t)2);
	// The old session's data must not be misreported as loss on restart.
	O3DS_CHECK_EQ(gate.Stats().lost, (uint64_t)0);
}

O3DS_TEST(ReorderGate_RestartViaBackjumpHeuristic_LegacyStreamWithoutEpoch)
{
	// No frame_epoch ever set on this stream (epoch stays 0 throughout) -
	// restart must fall back to the backward-jump heuristic.
	ReorderGate::Config config;
	config.reset_backjump = 50;
	ReorderGate gate(config);
	Delivered out;

	gate.Push(MakeFrame(1000), 0.0, out.Sink());
	gate.Push(MakeFrame(1001), 0.0, out.Sink());

	// Small backward step (within threshold) must NOT be treated as a
	// restart - it's ordinary stale/duplicate traffic.
	gate.Push(MakeFrame(990), 0.0, out.Sink());
	O3DS_CHECK_EQ(out.seqs.size(), (size_t)2);
	O3DS_CHECK_EQ(gate.Stats().stale_dropped, (uint64_t)1);

	// Large backward jump (beyond threshold) must re-baseline.
	gate.Push(MakeFrame(1), 0.0, out.Sink());
	O3DS_CHECK_EQ(out.seqs.size(), (size_t)3);
	O3DS_CHECK_EQ(out.seqs[2], (uint64_t)1);
}

O3DS_TEST(ReorderGate_MalformedInputsAreSafe)
{
	// The gate itself only ever touches Frame's scalar fields (seq/epoch/
	// wallclock_us); it never dereferences `bytes`. This test exists mainly
	// to document/guard that an empty payload is a legitimate input, not a
	// crash - the real untrusted-buffer hardening lives in
	// SubjectList::PeekMeta (see sequencing_tests.cpp).
	ReorderGate gate;
	Delivered out;

	Frame f = MakeFrame(1);
	f.bytes.clear();
	gate.Push(std::move(f), 0.0, out.Sink());

	O3DS_CHECK_EQ(out.seqs.size(), (size_t)1);
}
