// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DReceiverStreamScheduler.h"

#include "O3DPerformanceMetrics.h"
#include "O3DReceiverLogs.h"

FO3DReceiverStreamScheduler::FO3DReceiverStreamScheduler(FApplyFrame InApply)
	: Apply(MoveTemp(InApply))
{
}

void FO3DReceiverStreamScheduler::Push(const FString& Subject, TConstArrayView<uint8> Buffer, double TimestampSeconds, const O3DS::PacketMeta& Meta,
	uint64 ArrivalEpochUs, double NowSeconds, const O3DS::LegacyOrderingConfig& LegacyConfig, bool bDebugLog)
{
	// RCV-5: one parse and ordering state per sender, so senders sharing a channel cannot delete
	// each other's subjects or mix their tx_seq spaces and clocks.
	const uint64 StreamKey = Streams.ResolveKey(Meta.subject_names);
	O3DS::ReceiverStream& Stream = Streams.Acquire(StreamKey, NowSeconds, &Meta.subject_names);

	if (Meta.tx_seq == 0)
	{
		// Legacy sender (no tx_seq): SubjectList.time ordering, per stream, decided before the
		// parse so a dropped frame never changes parse state (ADR 0005 (ix)). RCV-34: a silence or
		// timestamp-jump reset only forgets this stream's last applied time; the gate, clock
		// estimator and concealment state are untouched.
		const O3DS::LegacyOrdering::Decision Decision = Stream.legacy.Check(Meta.time, NowSeconds, LegacyConfig);
		if (bDebugLog && Stream.legacy.LastCheckReset())
		{
			UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Reset legacy ordering window (new=%.6f)"), Meta.time);
		}
		if (Decision != O3DS::LegacyOrdering::Decision::Apply)
		{
			if (bDebugLog)
			{
				UE_LOG(LogO3DReceiverSource, Verbose, TEXT("Dropping %s frame t=%.6f"),
					Decision == O3DS::LegacyOrdering::Decision::Duplicate ? TEXT("duplicate") : TEXT("out-of-order"), Meta.time);
			}
			FO3DPerformanceMetrics::Get().RecordReceiverFrameDropped();
			return;
		}

		Apply(Stream, Subject, reinterpret_cast<const char*>(Buffer.GetData()), static_cast<size_t>(Buffer.Num()), TimestampSeconds, nullptr);
		return;
	}

	O3DS::Frame Frame;
	Frame.seq = Meta.tx_seq;
	Frame.wallclock_us = Meta.tx_wallclock_us;
	Frame.epoch = Meta.frame_epoch;
	Frame.local_recv_us = ArrivalEpochUs; // true arrival instant - see Frame's doc comment
	Frame.bytes.assign(reinterpret_cast<const char*>(Buffer.GetData()),
		reinterpret_cast<const char*>(Buffer.GetData()) + Buffer.Num());

	LastGateSubjectLabel = Subject;

	Stream.gate.Push(std::move(Frame), NowSeconds, [this, StreamKey](O3DS::Frame&& F) { Emit(StreamKey, std::move(F)); });
	ReportGateMetricsDelta();
}

void FO3DReceiverStreamScheduler::Flush(double NowSeconds)
{
	// Each sender stream has its own gate (RCV-5). Emit looks its stream up by key and never adds
	// or removes streams, as ForEach requires.
	Streams.ForEach([this, NowSeconds](uint64 StreamKey, O3DS::ReceiverStream& Stream)
	{
		Stream.gate.Flush(NowSeconds, [this, StreamKey](O3DS::Frame&& F) { Emit(StreamKey, std::move(F)); });
	});
	ReportGateMetricsDelta();
}

void FO3DReceiverStreamScheduler::Emit(uint64 StreamKey, O3DS::Frame&& Frame)
{
	// The stream can only be missing if it was dropped between Push and a later Flush (idle prune
	// or table eviction); its buffered frames go with it.
	O3DS::ReceiverStream* Stream = Streams.Find(StreamKey);
	if (!Stream)
	{
		return;
	}
	Apply(*Stream, LastGateSubjectLabel, Frame.bytes.data(), Frame.bytes.size(), 0.0, &Frame);
}

/** Report the ReorderGate's stats as a delta against the last-reported snapshot (so
 *  multiple receiver sources correctly aggregate into the shared metrics singleton,
 *  the same convention as FramesReceived etc. - see FReceiverMetrics's doc comment). */
void FO3DReceiverStreamScheduler::ReportGateMetricsDelta()
{
	auto Delta = [](uint64 NewVal, uint64 OldVal) -> uint64 { return NewVal >= OldVal ? (NewVal - OldVal) : 0; };

	uint64 DeltaDup = 0, DeltaStale = 0, DeltaLost = 0, DeltaReordered = 0;
	int32 Pending = 0;
	Streams.ForEach([&](uint64, O3DS::ReceiverStream& Stream)
	{
		const O3DS::ReorderStats& Stats = Stream.gate.Stats();
		DeltaDup += Delta(Stats.dup_dropped, Stream.reportedGateStats.dup_dropped);
		DeltaStale += Delta(Stats.stale_dropped, Stream.reportedGateStats.stale_dropped);
		DeltaLost += Delta(Stats.lost, Stream.reportedGateStats.lost);
		DeltaReordered += Delta(Stats.reordered, Stream.reportedGateStats.reordered);
		Pending += static_cast<int32>(Stream.gate.PendingCount());
		Stream.reportedGateStats = Stats;
	});

	FO3DPerformanceMetrics& Metrics = FO3DPerformanceMetrics::Get();
	if (DeltaDup) Metrics.RecordGateDupDropped(DeltaDup);
	if (DeltaStale) Metrics.RecordGateStaleDropped(DeltaStale);
	if (DeltaLost) Metrics.RecordGateLost(DeltaLost);
	if (DeltaReordered) Metrics.RecordGateReordered(DeltaReordered);
	if (DeltaDup || DeltaStale) Metrics.RecordReceiverFrameDropped(DeltaDup + DeltaStale);

	Metrics.SetGateBufferOccupancy(Pending);
}
