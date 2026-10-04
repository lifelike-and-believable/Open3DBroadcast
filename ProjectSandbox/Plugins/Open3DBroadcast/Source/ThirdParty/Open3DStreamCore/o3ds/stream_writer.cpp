#include "stream_writer.h"

#include "model.h"
#include "sequencing.h"

#include <algorithm>
#include <atomic>

namespace O3DS
{
	namespace
	{
		std::atomic<uint32_t> gLastEpoch{ 0 };
	}

	void StreamWriter::StartSession()
	{
		// ADR 0005 (iv): never reuse or go back to an epoch within a process,
		// even for two starts in the same wall-clock second, across writers.
		uint32_t last = gLastEpoch.load(std::memory_order_relaxed);
		uint32_t next = 0;
		do
		{
			next = std::max(NewSessionEpoch(), last + 1);
		} while (!gLastEpoch.compare_exchange_weak(last, next, std::memory_order_relaxed));
		mEpoch = next;
	}

	TxStamp StreamWriter::Next()
	{
		if (mEpoch == 0)
		{
			StartSession();
		}
		TxStamp stamp;
		stamp.tx_seq = mNextSeq++;
		stamp.tx_wallclock_us = NowUtcMicros();
		stamp.frame_epoch = mEpoch;
		return stamp;
	}

	int StreamWriter::WriteFull(Subject& subject, std::vector<char>& out, double timestamp, const SceneTime* sceneTime)
	{
		const TxStamp stamp = Next();
		const int size = subject.Serialize(out, timestamp, stamp.tx_seq, stamp.tx_wallclock_us, stamp.frame_epoch, sceneTime);
		if (size > 0)
		{
			mLastFullSeq[subject.mName] = stamp.tx_seq;
		}
		return size;
	}

	int StreamWriter::WriteFull(SubjectList& list, std::vector<char>& out, double timestamp, const SceneTime* sceneTime)
	{
		const TxStamp stamp = Next();
		const int size = list.Serialize(out, timestamp, stamp.tx_seq, stamp.tx_wallclock_us, stamp.frame_epoch, sceneTime);
		if (size > 0)
		{
			for (Subject* subject : list.mItems)
			{
				if (subject != nullptr)
				{
					mLastFullSeq[subject->mName] = stamp.tx_seq;
				}
			}
		}
		return size;
	}

	int StreamWriter::WriteUpdate(Subject& subject, std::vector<char>& out, size_t& count, double deltaThreshold,
		double timestamp, const QuantRanges* quantRanges, const SceneTime* sceneTime)
	{
		const TxStamp stamp = Next();
		return subject.SerializeUpdate(out, count, deltaThreshold, timestamp, quantRanges,
			stamp.tx_seq, stamp.tx_wallclock_us, stamp.frame_epoch, LastFullSeq(subject.mName), sceneTime);
	}

	int StreamWriter::WriteResidual(Subject& subject, std::vector<char>& out, size_t& count, double deltaThreshold, double timestamp,
		const SceneTime* sceneTime)
	{
		const TxStamp stamp = Next();
		return subject.SerializeUpdateResidual(out, count, deltaThreshold, timestamp,
			stamp.tx_seq, stamp.tx_wallclock_us, stamp.frame_epoch, LastFullSeq(subject.mName), sceneTime);
	}

	uint64_t StreamWriter::LastFullSeq(const std::string& subjectName) const
	{
		const auto it = mLastFullSeq.find(subjectName);
		return (it != mLastFullSeq.end()) ? it->second : 0;
	}
}
