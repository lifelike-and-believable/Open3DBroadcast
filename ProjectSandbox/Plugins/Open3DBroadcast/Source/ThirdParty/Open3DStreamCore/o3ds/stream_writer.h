#pragma once

// One logical sender stream (ADR 0005 (iv), resilient-streaming A1): stamps
// every frame it writes with tx_seq, tx_wallclock_us and frame_epoch, so a
// receiver's reorder gate, clock-offset mapping and concealment engage.
//
// - tx_seq comes from one counter per writer that StartSession does not
//   reset, so a Stop/Start cannot look like a backward jump within one epoch.
// - Each session takes epoch = max(NewSessionEpoch(), last epoch issued in
//   this process + 1). The floor is process-wide, not per writer, so a new
//   writer for the same stream (a sender recreated, or its state cleared)
//   always starts in a strictly larger epoch, even within one wall-clock
//   second, and a receiver's ReorderGate sees the restart instead of
//   dropping the restarted counter as stale.
// - The receiver keys streams by subject names (ReceiverStreamTable), so a
//   sender that writes one subject per frame needs one writer per subject;
//   a counter shared by several subjects would look like loss on each.
// - tx_wallclock_us is UTC at the write (NowUtcMicros), for latency and clock
//   offset estimates only; ordering uses tx_seq.
//
// Not thread-safe: one writer per stream, used by one thread at a time (the
// UE sender's pipeline worker).

#include "o3ds_export.h"
#include "quant/channel_quant.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace O3DS
{
	class Subject;
	class SubjectList;

	//! The sequencing fields one frame carries.
	struct TxStamp
	{
		uint64_t tx_seq = 0;
		uint64_t tx_wallclock_us = 0;
		uint32_t frame_epoch = 0;
	};

	class O3DS_API StreamWriter
	{
	public:
		//! Starts a session: a new epoch, max(NewSessionEpoch(), last epoch
		//! issued in this process + 1). The sequence counter keeps counting.
		void StartSession();

		//! The current epoch; 0 before the first session.
		uint32_t Epoch() const { return mEpoch; }

		//! The tx_seq of the last frame written; 0 before the first.
		uint64_t LastSeq() const { return mNextSeq - 1; }

		//! The stamp for the next frame (and advances the counter). Starts a
		//! session first when none was started.
		TxStamp Next();

		//! A full Subject (descriptor and values).
		int WriteFull(Subject& subject, std::vector<char>& out, double timestamp);
		//! Every subject of a list, in full.
		int WriteFull(SubjectList& list, std::vector<char>& out, double timestamp);
		//! A delta update (quantized when quantRanges is given).
		int WriteUpdate(Subject& subject, std::vector<char>& out, size_t& count, double deltaThreshold,
			double timestamp, const QuantRanges* quantRanges = nullptr);
		//! A residual update (the subject has a ResidualEncoder).
		int WriteResidual(Subject& subject, std::vector<char>& out, size_t& count, double deltaThreshold, double timestamp);

	private:
		uint64_t mNextSeq = 1;
		uint32_t mEpoch = 0;
	};
}
