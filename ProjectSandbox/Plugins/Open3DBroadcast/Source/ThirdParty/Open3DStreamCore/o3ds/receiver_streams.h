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

#include "o3ds_export.h"
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
		uint64_t stream_key = 0;      //!< StreamKeyForNames(subject_names)
		std::vector<std::string> subject_names; //!< named subjects and updates, in wire order
	};

	//! Reads PacketMeta from a framed wire buffer (8-byte flags and CRC
	//! header, then the FlatBuffer). Checks the length, runs the
	//! FlatBuffers Verifier and rejects a non-finite time or more than
	//! ParseLimits::kMaxSubjects subjects or updates. Like
	//! SubjectList::PeekMeta it does not check the CRC; Parse() does.
	//! Returns false, with `out` reset to defaults, when the buffer is
	//! rejected.
	O3DS_API bool PeekPacketMeta(const char* data, size_t len, PacketMeta& out);

	//! A 64-bit FNV-1a hash of the sorted, de-duplicated subject names, used
	//! as the key of a new sender stream (see ReceiverStreamTable::
	//! ResolveKey). Returns 0 for no names, and never 0 otherwise.
	O3DS_API uint64_t StreamKeyForNames(std::vector<std::string> names);

	//! 64-bit FNV-1a over the transform count and, per transform, its
	//! name (length-prefixed) and parent id (RCV-4). Two skeletons with the
	//! same hierarchy but different bone names get different fingerprints.
	//! A null transform pointer contributes a fixed marker.
	O3DS_API uint64_t SkeletonFingerprint(const Subject& subject);

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
	class O3DS_API LegacyOrdering
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

	//! Owns one ReceiverStream per sender stream, bounded in number. Not
	//! thread-safe; confine it to one thread, like ReorderGate.
	//!
	//! The wire has no sender id, so a packet is assigned to a stream by
	//! the subject names it carries. Senders sharing one channel must
	//! already use distinct subject names, or LiveLink could not tell their
	//! subjects apart. A packet joins the stream that already owns one of
	//! its subjects (first match in wire order); otherwise it starts a new
	//! stream keyed by StreamKeyForNames(). A sender that adds or drops a
	//! subject therefore stays on one stream, and one that restarts keeps
	//! its stream, so ReorderGate's frame_epoch handling still sees the
	//! restart. Limitation: a sender that splits its subjects over separate
	//! packets that share one tx_seq counter would look like several streams
	//! with gaps; no current sender does that.
	class O3DS_API ReceiverStreamTable
	{
	public:
		static constexpr size_t kDefaultMaxStreams = 64;

		//! `computeWorldMatrices` is copied into each new stream's
		//! SubjectList::mComputeWorldMatrices.
		explicit ReceiverStreamTable(size_t maxStreams = kDefaultMaxStreams, bool computeWorldMatrices = true);

		// Move-only: the streams are owned through std::unique_ptr. Declared
		// explicitly because an O3DS_API class gets every member instantiated
		// by MSVC, and the implicit copy constructor of a std::map of
		// std::unique_ptr does not compile (see o3ds_export.h).
		ReceiverStreamTable(const ReceiverStreamTable&) = delete;
		ReceiverStreamTable& operator=(const ReceiverStreamTable&) = delete;
		ReceiverStreamTable(ReceiverStreamTable&&) = default;
		ReceiverStreamTable& operator=(ReceiverStreamTable&&) = default;

		//! The key of the stream a packet with these subject names belongs
		//! to: the stream owning the first already-known name, otherwise
		//! StreamKeyForNames(subjectNames).
		uint64_t ResolveKey(const std::vector<std::string>& subjectNames) const;

		//! Returns the stream for `key`, creating it if needed, and marks it
		//! seen at `nowS`. `subjectNames`, when given, become owned by this
		//! stream (names already owned by another stream are left there).
		//! When the table is full, the least recently seen stream is dropped
		//! first, so hostile input cannot grow it without bound; the number
		//! of owned names is bounded the same way. A reference stays valid
		//! until that stream is dropped by Acquire(), PruneIdle() or Clear().
		ReceiverStream& Acquire(uint64_t key, double nowS, const std::vector<std::string>* subjectNames = nullptr);

		//! Returns the stream for `key`, or nullptr. Does not mark it seen.
		ReceiverStream* Find(uint64_t key);

		//! Drops streams not seen for more than `idleSeconds`. Returns how
		//! many were dropped.
		size_t PruneIdle(double nowS, double idleSeconds);

		void Clear() { mStreams.clear(); mSubjectOwner.clear(); }
		size_t Size() const { return mStreams.size(); }

		//! Visits every stream in key order. `fn` must not add or remove
		//! streams.
		void ForEach(const std::function<void(uint64_t key, ReceiverStream& stream)>& fn);

	private:
		void Erase(std::map<uint64_t, std::unique_ptr<ReceiverStream>>::iterator it);

		size_t mMaxStreams;
		bool mComputeWorldMatrices;
		std::map<uint64_t, std::unique_ptr<ReceiverStream>> mStreams;
		std::map<std::string, uint64_t> mSubjectOwner; // subject name -> owning stream key
	};
}

#endif
