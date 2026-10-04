// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DPerformanceMetrics.h"
#include "Containers/ArrayView.h"
#include "Templates/Function.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/receiver_streams.h"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

/**
 * Orders the packets of every sender on one receiver (WP-A3 split it out of FO3DReceiverSource).
 * Each sender stream (RCV-5, O3DS::ReceiverStreamTable) keeps its own parse state, reorder gate,
 * clock estimator and legacy ordering. A packet from a sender without tx_seq goes through the
 * legacy timestamp ordering and is released at once or dropped; a sequenced packet goes through its
 * stream's reorder gate and is released in order, now or from a later Flush. Released packets go to
 * the apply callback, which parses and publishes them. Game thread, like the gates it holds.
 */
class FO3DReceiverStreamScheduler
{
public:
	/**
	 * Parses and publishes one released packet. GatedFrame is null on the legacy path, where
	 * LegacyTimestampSeconds is the transport's timestamp for the packet (the latency metric).
	 */
	using FApplyFrame = TFunction<void(O3DS::ReceiverStream& Stream, const FString& Label, const char* Data, size_t Len,
		double LegacyTimestampSeconds, const O3DS::Frame* GatedFrame)>;

	/** Metrics: the owning receiver source's handle (ADR 0012 item 4). */
	FO3DReceiverStreamScheduler(FApplyFrame InApply, FO3DReceiverMetricsHandleRef InMetrics);

	/**
	 * One verified packet (Meta from O3DS::PeekPacketMeta). ArrivalEpochUs is the packet's true
	 * arrival time (before any thread hop), used by the gated path's clock estimate.
	 */
	void Push(const FString& Subject, TConstArrayView<uint8> Buffer, double TimestampSeconds, const O3DS::PacketMeta& Meta,
		uint64 ArrivalEpochUs, double NowSeconds, const O3DS::LegacyOrderingConfig& LegacyConfig, bool bDebugLog);

	/** Releases gap-buffered frames whose wait timed out, for every stream. */
	void Flush(double NowSeconds);

	/** Reports the gates' stats as deltas against the last report, into the metrics handle. */
	void ReportGateMetricsDelta();

	/** Drops streams that sent nothing for IdleSeconds, so a restarted sender starts clean. */
	void PruneIdle(double NowSeconds, double IdleSeconds) { Streams.PruneIdle(NowSeconds, IdleSeconds); }

	/** Drops every stream (a transport start or stop). */
	void Reset() { Streams.Clear(); }

	/** The stream that owns these subjects, or null. */
	O3DS::ReceiverStream* FindBySubjects(const std::vector<std::string>& Subjects) { return Streams.Find(Streams.ResolveKey(Subjects)); }

	size_t GetNumStreams() const { return Streams.Size(); }

private:
	void Emit(uint64 StreamKey, O3DS::Frame&& Frame);

	FApplyFrame Apply;
	FO3DReceiverMetricsHandleRef Metrics;
	O3DS::ReceiverStreamTable Streams;
	/** Diagnostic label for the gate's emit path: the subject of the latest gated packet. */
	FString LastGateSubjectLabel;
};
