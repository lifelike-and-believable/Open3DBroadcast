/*
Open 3D Stream

Copyright 2026 Open3DStream Contributors

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

#ifndef OPEN3D_STREAM_RECEIVER_STREAMS_H
#define OPEN3D_STREAM_RECEIVER_STREAMS_H

// WP-S4 (receiver correctness): the UE-independent part of the receiver.
// It keeps one parse and ordering state per sender stream, so that several
// senders on one transport channel cannot interfere (RCV-5), and it
// provides the skeleton fingerprint the receiver uses to decide when bone
// names must be republished (RCV-4).

#include "model.h"
#include "reorder_gate.h"
#include "clock_offset.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace O3DS
{
	//! Metadata read from a wire buffer without a full parse.
	struct PacketMeta
	{
		uint64_t tx_seq = 0;          //!< SubjectList.tx_seq (0 = legacy sender)
		uint64_t tx_wallclock_us = 0; //!< SubjectList.tx_wallclock_us
		uint32_t frame_epoch = 0;     //!< SubjectList.frame_epoch
		double time = 0.0;            //!< SubjectList.time, the sender's content clock
		uint64_t stream_key = 0;      //!< see StreamKeyForNames()
	};

	//! Reads PacketMeta from a framed wire buffer (8-byte flags and CRC
	//! header, then the FlatBuffer). Checks the length, runs the
	//! FlatBuffers Verifier and rejects a non-finite time or more than
	//! ParseLimits::kMaxSubjects subjects or updates. Like
	//! SubjectList::PeekMeta it does not check the CRC; Parse() does.
	//! Returns false, with `out` reset to defaults, when the buffer is
	//! rejected.
	bool PeekPacketMeta(const char* data, size_t len, PacketMeta& out);

	//! Identifies the sender stream a packet belongs to when several
	//! senders share one channel. The wire has no sender id, so the key is
	//! a 64-bit FNV-1a hash of the sorted, de-duplicated subject names the
	//! packet carries (full subjects and updates together). Senders on one
	//! channel must already use distinct subject names, or LiveLink could
	//! not tell their subjects apart. A sender that sends a stable set of
	//! subjects keeps one key across restarts, so ReorderGate's
	//! frame_epoch handling still sees the restart. A packet with no named
	//! subjects gets key 0.
	uint64_t StreamKeyForNames(std::vector<std::string> names);

	//! 64-bit FNV-1a over the transform count and, per transform, its
	//! name (length-prefixed) and parent id (RCV-4). Two skeletons with the
	//! same hierarchy but different bone names get different fingerprints.
	//! A null transform pointer contributes a fixed marker.
	uint64_t SkeletonFingerprint(const Subject& subject);

	//! Tuning for LegacyOrdering. The defaults match the UE receiver's
	//! console variables.
	struct LegacyOrderingConfig
	{
		bool dropOutOfOrder = true;              //!< drop a time older than the last applied one
		double silenceResetSeconds = 2.0;        //!< forget ordering after this much silence (0 disables)
		double timestampJumpResetSeconds = 1.0;  //!< forget ordering when time jumps back by more (0 disables)
	};

	//! Duplicate and out-of-order suppression by SubjectList.time for
	//! senders that do not set tx_seq. One instance per sender stream.
	//! Meant to run before Parse(), so a dropped frame never changes parse
	//! state (ADR 0005 (ix)). A reset here only forgets the last applied
	//! time; it does not touch the stream's ReorderGate, clock estimator
	//! or any concealment state (RCV-34).
	class LegacyOrdering
	{
	public:
		enum class Decision
		{
			Apply,
			Duplicate,
			OutOfOrder,
		};

		//! `nowS` is any monotonic seconds clock, compared only with other
		//! `nowS` values passed to this instance.
		Decision Check(double subjectListTime, double nowS, const LegacyOrderingConfig& config);

		//! True when the last Check() reset the ordering window first.
		bool LastCheckReset() const { return mLastCheckReset; }

		double LastAppliedTime() const { return mLastApplied; }

		void Reset();

	private:
		double mLastApplied = -1.0;
		double mLastSeenS = -1.0;
		bool mLastCheckReset = false;
	};

	//! Everything the receiver keeps per sender stream.
	struct ReceiverStream
	{
		SubjectList subjects;
		ReorderGate gate;
		ClockOffsetEstimator clock;
		LegacyOrdering legacy;
		ReorderStats reportedGateStats; //!< last gate stats reported to metrics, for deltas
		double lastSeenS = 0.0;
	};

	//! Owns one ReceiverStream per stream key, bounded in number. Not
	//! thread-safe; confine it to one thread, like ReorderGate.
	class ReceiverStreamTable
	{
	public:
		static constexpr size_t kDefaultMaxStreams = 64;

		//! `computeWorldMatrices` is copied into each new stream's
		//! SubjectList::mComputeWorldMatrices.
		explicit ReceiverStreamTable(size_t maxStreams = kDefaultMaxStreams, bool computeWorldMatrices = true);

		//! Returns the stream for `key`, creating it if needed, and marks it
		//! seen at `nowS`. When the table is full, the least recently seen
		//! stream is dropped first, so hostile input cannot grow it without
		//! bound. A reference stays valid until that stream is dropped by
		//! Acquire(), PruneIdle() or Clear().
		ReceiverStream& Acquire(uint64_t key, double nowS);

		//! Returns the stream for `key`, or nullptr. Does not mark it seen.
		ReceiverStream* Find(uint64_t key);

		//! Drops streams not seen for more than `idleSeconds`. Returns how
		//! many were dropped.
		size_t PruneIdle(double nowS, double idleSeconds);

		void Clear() { mStreams.clear(); }
		size_t Size() const { return mStreams.size(); }

		//! Visits every stream in key order. `fn` must not add or remove
		//! streams.
		void ForEach(const std::function<void(uint64_t key, ReceiverStream& stream)>& fn);

	private:
		size_t mMaxStreams;
		bool mComputeWorldMatrices;
		std::map<uint64_t, std::unique_ptr<ReceiverStream>> mStreams;
	};
}

#endif
