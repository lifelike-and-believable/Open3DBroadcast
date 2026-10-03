// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

#include <atomic>

DECLARE_LOG_CATEGORY_EXTERN(LogO3DPerformanceMetrics, Log, All);

namespace O3DMetrics
{
	/** Raise Target to Value if Value is larger. Lock-free; never moves the value down (SHR-26). */
	template <typename T>
	inline void AtomicStoreMax(std::atomic<T>& Target, T Value)
	{
		T Current = Target.load(std::memory_order_relaxed);
		while (Value > Current && !Target.compare_exchange_weak(Current, Value, std::memory_order_relaxed))
		{
		}
	}

	/**
	 * Exponential moving average update, Target = Target * (1 - Alpha) + Sample * Alpha, as a
	 * compare-exchange loop so concurrent updates are not lost (SHR-26).
	 */
	inline void AtomicUpdateEma(std::atomic<double>& Target, double Sample, double Alpha)
	{
		double Current = Target.load(std::memory_order_relaxed);
		while (!Target.compare_exchange_weak(Current, (Current * (1.0 - Alpha)) + (Sample * Alpha), std::memory_order_relaxed))
		{
		}
	}
}

/**
 * Counters for one transport (SHR-3, SHR-17). Obtained once from FO3DTransportMetricsRegistry
 * and held by the transport instance: the address is stable for as long as a reference is
 * held, and updates are lock-free atomics.
 */
struct FO3DTransportMetrics
{
	explicit FO3DTransportMetrics(FName InTransportName)
		: TransportName(InTransportName)
	{
	}

	FO3DTransportMetrics(const FO3DTransportMetrics&) = delete;
	FO3DTransportMetrics& operator=(const FO3DTransportMetrics&) = delete;

	const FName TransportName;

	// Connection state
	std::atomic<bool> bConnected{ false };
	std::atomic<int32> ConnectionAttempts{ 0 };
	std::atomic<int32> ReconnectCount{ 0 };

	// Data flow
	std::atomic<uint64> FramesSent{ 0 };               // Frames sent through transport
	std::atomic<uint64> BytesSent{ 0 };                // Bytes sent through transport
	std::atomic<uint64> FramesReceived{ 0 };           // Frames received (for bidirectional)
	std::atomic<uint64> BytesReceived{ 0 };            // Bytes received

	// Queue depth (for async transports)
	std::atomic<int32> PendingFrames{ 0 };             // Frames waiting to send
	std::atomic<int32> MaxPendingFrames{ 0 };          // Peak queue depth

	// Errors
	std::atomic<uint64> SendErrors{ 0 };
	std::atomic<uint64> ReceiveErrors{ 0 };
	std::atomic<int32> PipeCount{ 0 };                 // For NNG: number of connected pipes

	void RecordFrameSent(uint64 ByteCount)
	{
		FramesSent.fetch_add(1, std::memory_order_relaxed);
		BytesSent.fetch_add(ByteCount, std::memory_order_relaxed);
	}

	void RecordFrameReceived(uint64 ByteCount)
	{
		FramesReceived.fetch_add(1, std::memory_order_relaxed);
		BytesReceived.fetch_add(ByteCount, std::memory_order_relaxed);
	}

	void SetConnected(bool bInConnected) { bConnected.store(bInConnected, std::memory_order_relaxed); }
	void RecordConnectionAttempt() { ConnectionAttempts.fetch_add(1, std::memory_order_relaxed); }
	void SetPipeCount(int32 Count) { PipeCount.store(Count, std::memory_order_relaxed); }

	void UpdatePendingFrames(int32 PendingCount)
	{
		PendingFrames.store(PendingCount, std::memory_order_relaxed);
		O3DMetrics::AtomicStoreMax(MaxPendingFrames, PendingCount);
	}

	void RecordError(bool bSendError)
	{
		if (bSendError)
		{
			SendErrors.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			ReceiveErrors.fetch_add(1, std::memory_order_relaxed);
		}
	}

	/** Zero the counters. Connection state and pipe count are live state and are kept. */
	void ResetCounters()
	{
		FramesSent.store(0);
		BytesSent.store(0);
		FramesReceived.store(0);
		BytesReceived.store(0);
		PendingFrames.store(0);
		MaxPendingFrames.store(0);
		SendErrors.store(0);
		ReceiveErrors.store(0);
		ConnectionAttempts.store(0);
		ReconnectCount.store(0);
	}
};

/** Plain-value copy of one transport's counters, safe to keep and read on any thread. */
struct FO3DTransportMetricsSnapshot
{
	FName TransportName;
	bool bConnected = false;
	int32 ConnectionAttempts = 0;
	int32 ReconnectCount = 0;
	uint64 FramesSent = 0;
	uint64 BytesSent = 0;
	uint64 FramesReceived = 0;
	uint64 BytesReceived = 0;
	int32 PendingFrames = 0;
	int32 MaxPendingFrames = 0;
	uint64 SendErrors = 0;
	uint64 ReceiveErrors = 0;
	int32 PipeCount = 0;
};

using FO3DTransportMetricsRef = TSharedRef<FO3DTransportMetrics, ESPMode::ThreadSafe>;

/**
 * Name-keyed set of transport counters (SHR-3, SHR-17). Entries are heap objects held by
 * shared reference, so registering a new name never moves an existing entry, and a handle
 * stays valid for as long as its holder keeps it. The lock guards only the map; counter
 * updates through a handle never take it. Thread-safe.
 */
class OPEN3DSHARED_API FO3DTransportMetricsRegistry
{
public:
	FO3DTransportMetricsRegistry();
	~FO3DTransportMetricsRegistry();

	FO3DTransportMetricsRegistry(const FO3DTransportMetricsRegistry&) = delete;
	FO3DTransportMetricsRegistry& operator=(const FO3DTransportMetricsRegistry&) = delete;

	/** Return the counters for TransportName, creating them on first use. */
	FO3DTransportMetricsRef Acquire(FName TransportName);

	/** Return the counters for TransportName, or null when none were created. */
	TSharedPtr<FO3DTransportMetrics, ESPMode::ThreadSafe> Find(FName TransportName) const;

	/** Copy every entry's counters, sorted by name. */
	TArray<FO3DTransportMetricsSnapshot> Snapshot() const;

	/** Zero every entry's counters. Handles stay valid. */
	void ResetCounters();

	int32 Num() const;

private:
	mutable FCriticalSection Mutex;
	TMap<FName, FO3DTransportMetricsRef> Entries;
};

/**
 * Real-time performance metrics collection for Open3DBroadcast plugin
 *
 * Thread-safe atomic counters. Rolling averages and peaks use compare-exchange loops, so
 * concurrent updates are not lost (SHR-26). The overhead has not been measured.
 * Data is accumulated in real-time and can be queried via console command:
 *   o3d.DumpMetrics
 */
class OPEN3DSHARED_API FO3DPerformanceMetrics
{
public:
	/**
	 * Sender-side metrics (Broadcast)
	 */
	struct FSenderMetrics
	{
		// Frame production
		std::atomic<uint64> FramesCaptured{ 0 };           // Total frames captured from component
		std::atomic<uint64> FramesDropped{ 0 };            // Frames dropped due to backpressure

		// Serialization
		std::atomic<uint64> BytesSerialized{ 0 };          // Total bytes serialized

		// Transport send
		std::atomic<uint64> BytesSent{ 0 };                // Total bytes sent to network/transport
		std::atomic<uint64> TransportFramesDropped{ 0 };   // Frames dropped by transport layer

		// FPlatformTime::Seconds() when the metrics were last reset. Atomic because the console
		// command may reset while other threads read it (SHR-26).
		std::atomic<double> MetricsStartSeconds{ FPlatformTime::Seconds() };
	};

	/**
	 * Receiver-side metrics (LiveLink)
	 */
	struct FReceiverMetrics
	{
		// Frame reception
		std::atomic<uint64> FramesReceived{ 0 };           // Total frames received
		std::atomic<uint64> FramesApplied{ 0 };            // Frames successfully applied to LiveLink
		std::atomic<uint64> FramesDropped{ 0 };            // Frames dropped (duplicate, out-of-order, etc)

		// Deserialization
		std::atomic<uint64> BytesDeserialized{ 0 };        // Total bytes deserialized
		std::atomic<uint64> DeserializationErrors{ 0 };    // Deserialization failures
		std::atomic<uint64> UpdatesAwaitingFullSync{ 0 };  // Updates dropped until the next full Subject: missed full sync, residual gap, or no residual history (ADR 0005 (ix))
		std::atomic<uint64> InvalidPosesDropped{ 0 };      // Subjects skipped: a transform was not finite, had a zero rotation or was missing (RCV-13)

		// Per-operation timing (to identify bottlenecks)
		std::atomic<double> AvgParseTimeMs{ 0.0 };         // Rolling avg: FlatBuffer parse time
		std::atomic<double> AvgPoseExtractionTimeMs{ 0.0 }; // Rolling avg: bone structure extraction
		std::atomic<double> AvgLiveLinkPushTimeMs{ 0.0 };  // Rolling avg: LiveLink frame data push time
		std::atomic<double> AvgTotalProcessingTimeMs{ 0.0 }; // Rolling avg: total per-frame processing

		// LiveLink updates
		std::atomic<uint64> SkeletonUpdates{ 0 };          // Static data pushes: a subject new to LiveLink or with new bone or curve names
		std::atomic<uint64> PoseUpdates{ 0 };              // Number of pose frame updates
		std::atomic<int32> ActiveSubjectCount{ 0 };        // Current number of subjects being received

		// Latency tracking
		// Receive-to-apply latency (RCV-33): from the transport receiving a packet to the receiver
		// applying it, on this machine's clock; includes the reorder gate's wait on the gated path.
		// Not network latency: that is in AvgClockOffsetMs / AvgJitterMs below.
		std::atomic<double> AvgReceiveToApplyLatencyMs{ 0.0 };
		std::atomic<double> MaxLatencyMs{ 0.0 };           // Peak receive-to-apply latency since the last reset

		// A2: ReorderGate / ClockOffsetEstimator metrics (see O3DReceiverSource.cpp).
		// Gate counters are cumulative deltas summed across all receiver sources sharing
		// this singleton (each source tracks its own previous ReorderStats snapshot and
		// reports only the delta per tick, the same convention as FramesReceived etc.).
		std::atomic<uint64> GateDupDropped{ 0 };
		std::atomic<uint64> GateStaleDropped{ 0 };
		std::atomic<uint64> GateLost{ 0 };
		std::atomic<uint64> GateReordered{ 0 };
		// Current buffer occupancy is a gauge, not a cumulative counter; with multiple
		// receiver sources this is a last-writer-wins snapshot (same accepted
		// simplification as ActiveSubjectCount above - exact for the common single-source
		// case).
		std::atomic<int32> GateBufferOccupancy{ 0 };
		// Clock-offset estimate (caveated - see ClockOffsetEstimator's doc comment: only
		// meaningful as an absolute latency figure if clocks are known to be synced;
		// otherwise treat as relative/indicative) and jitter (excess delay above the
		// rolling-min floor), both rolling EMAs alpha=0.2 like AvgReceiveToApplyLatencyMs above.
		std::atomic<double> AvgClockOffsetMs{ 0.0 };
		std::atomic<double> AvgJitterMs{ 0.0 };

		// C1: receiver-side concealment (see O3DS::ConcealmentEngine / roadmap
		// doc §5/C1.d). Counters are cumulative deltas summed across all
		// per-subject engines and all receiver sources sharing this singleton,
		// the same convention as GateDupDropped etc. above. The error/pop
		// averages are gauges (last-writer-wins snapshot of whichever
		// subject/source most recently had a recovery), same simplification
		// as AvgClockOffsetMs/GateBufferOccupancy.
		std::atomic<uint64> ConcealedFrames{ 0 };            // TryConceal() returned true (predicted or held)
		std::atomic<uint64> ConcealmentFallbackHolds{ 0 };   // ...of which, held rather than freshly predicted
		std::atomic<uint64> ConcealmentCorrectionFrames{ 0 }; // frames output during a post-recovery correction blend
		std::atomic<uint64> ConcealmentRecoveries{ 0 };      // concealment spans that ended in a real-frame recovery
		std::atomic<double> AvgConcealmentPredictionTranslationError{ 0.0 }; // scene units
		std::atomic<double> AvgConcealmentPredictionRotationErrorDeg{ 0.0 };
		std::atomic<double> AvgConcealmentPopTranslation{ 0.0 }; // discontinuity at recovery - what concealment should reduce vs Hold
		std::atomic<double> AvgConcealmentPopRotationDeg{ 0.0 };

		// C1.c: latency-hiding/render-ahead (opt-in, default OFF). A simple
		// counter, not error-scored like the recovery metrics above - see
		// O3DS::ConcealmentEngine::TryRenderAhead()'s doc comment on why it's
		// tracked independently of the loss-recovery metrics.
		std::atomic<uint64> ConcealmentRenderAheadFrames{ 0 };

		// FPlatformTime::Seconds() when the metrics were last reset.
		std::atomic<double> MetricsStartSeconds{ FPlatformTime::Seconds() };
	};

	/** Transport-specific metrics (see FO3DTransportMetrics). */
	using FTransportMetrics = FO3DTransportMetrics;

	// =====================================================================
	// PUBLIC API
	// =====================================================================

	/** Get global metrics singleton */
	static FO3DPerformanceMetrics& Get();

	/** Reset all metrics to zero */
	void Reset();

	/** Get sender metrics (read-only reference) */
	const FSenderMetrics& GetSenderMetrics() const { return SenderMetrics; }

	/** Get receiver metrics (read-only reference) */
	const FReceiverMetrics& GetReceiverMetrics() const { return ReceiverMetrics; }

	/**
	 * Get or create the counters for a transport (SHR-3, SHR-17). Call once, for example when
	 * the transport is created, and keep the handle: updating through it takes no lock and does
	 * no name lookup.
	 */
	FO3DTransportMetricsRef AcquireTransportMetrics(FName TransportName) { return TransportRegistry.Acquire(TransportName); }

	/** Get transport metrics by name (null if not found). */
	TSharedPtr<FO3DTransportMetrics, ESPMode::ThreadSafe> FindTransportMetrics(FName TransportName) const { return TransportRegistry.Find(TransportName); }

	/** Copy of every transport's counters, sorted by name (replaces the unlocked array reference). */
	TArray<FO3DTransportMetricsSnapshot> GetTransportMetricsSnapshot() const { return TransportRegistry.Snapshot(); }

	/** Dump all metrics to log/console */
	void DumpMetrics() const;

	/** The metrics as "Metric,Value" CSV rows, transports included, for external analysis */
	FString GetMetricsAsCSV() const;

	// =====================================================================
	// SENDER SIDE API (inline for minimal overhead)
	// =====================================================================

	FORCEINLINE void RecordFrameCaptured() { ++SenderMetrics.FramesCaptured; }
	FORCEINLINE void RecordFrameDropped() { ++SenderMetrics.FramesDropped; }
	FORCEINLINE void RecordBytesSerialized(uint64 ByteCount) { SenderMetrics.BytesSerialized += ByteCount; }
	FORCEINLINE void RecordBytesSent(uint64 ByteCount) { SenderMetrics.BytesSent += ByteCount; }
	FORCEINLINE void RecordTransportFrameDropped() { ++SenderMetrics.TransportFramesDropped; }

	// =====================================================================
	// RECEIVER SIDE API
	// =====================================================================

	FORCEINLINE void RecordFrameReceived() { ++ReceiverMetrics.FramesReceived; }
	FORCEINLINE void RecordFrameApplied() { ++ReceiverMetrics.FramesApplied; }
	FORCEINLINE void RecordReceiverFrameDropped(uint64 Delta = 1) { ReceiverMetrics.FramesDropped += Delta; }
	FORCEINLINE void RecordBytesDeserialized(uint64 ByteCount) { ReceiverMetrics.BytesDeserialized += ByteCount; }
	FORCEINLINE void RecordDeserializationError() { ++ReceiverMetrics.DeserializationErrors; }
	FORCEINLINE void RecordInvalidPoseDropped() { ++ReceiverMetrics.InvalidPosesDropped; }
	FORCEINLINE void RecordUpdatesAwaitingFullSync(uint64 Count) { ReceiverMetrics.UpdatesAwaitingFullSync += Count; }
	FORCEINLINE void RecordSkeletonUpdate() { ++ReceiverMetrics.SkeletonUpdates; }
	FORCEINLINE void RecordPoseUpdate() { ++ReceiverMetrics.PoseUpdates; }
	FORCEINLINE void SetReceiverActiveSubjectCount(int32 Count) { ReceiverMetrics.ActiveSubjectCount.store(Count); }

	/** Record per-operation timing metrics (for bottleneck identification) */
	void RecordParseTimeMs(double TimeMs);
	void RecordPoseExtractionTimeMs(double TimeMs);
	void RecordLiveLinkPushTimeMs(double TimeMs);
	void RecordTotalProcessingTimeMs(double TimeMs);

	/** Records one frame's receive-to-apply latency (see AvgReceiveToApplyLatencyMs) */
	void RecordFrameLatency(double LatencyMs);

	// A2.d: ReorderGate / ClockOffsetEstimator metrics. Counters take a delta (not a
	// cumulative total) - see FReceiverMetrics's gate counters doc comment.
	FORCEINLINE void RecordGateDupDropped(uint64 Delta) { ReceiverMetrics.GateDupDropped += Delta; }
	FORCEINLINE void RecordGateStaleDropped(uint64 Delta) { ReceiverMetrics.GateStaleDropped += Delta; }
	FORCEINLINE void RecordGateLost(uint64 Delta) { ReceiverMetrics.GateLost += Delta; }
	FORCEINLINE void RecordGateReordered(uint64 Delta) { ReceiverMetrics.GateReordered += Delta; }
	FORCEINLINE void SetGateBufferOccupancy(int32 Count) { ReceiverMetrics.GateBufferOccupancy.store(Count); }

	/** Record one frame's clock-offset estimate and jitter (excess delay), both in ms. */
	void RecordClockOffsetSampleMs(double OffsetMs, double JitterMs);

	// C1.d: concealment metrics. Counters take a delta (not a cumulative
	// total), same convention as the gate counters above.
	FORCEINLINE void RecordConcealedFrames(uint64 Delta) { ReceiverMetrics.ConcealedFrames += Delta; }
	FORCEINLINE void RecordConcealmentFallbackHolds(uint64 Delta) { ReceiverMetrics.ConcealmentFallbackHolds += Delta; }
	FORCEINLINE void RecordConcealmentCorrectionFrames(uint64 Delta) { ReceiverMetrics.ConcealmentCorrectionFrames += Delta; }
	FORCEINLINE void RecordConcealmentRecoveries(uint64 Delta) { ReceiverMetrics.ConcealmentRecoveries += Delta; }
	FORCEINLINE void RecordConcealmentRenderAheadFrames(uint64 Delta) { ReceiverMetrics.ConcealmentRenderAheadFrames += Delta; }
	FORCEINLINE void SetConcealmentPredictionError(double TranslationUnits, double RotationDegrees)
	{
		ReceiverMetrics.AvgConcealmentPredictionTranslationError.store(TranslationUnits);
		ReceiverMetrics.AvgConcealmentPredictionRotationErrorDeg.store(RotationDegrees);
	}
	FORCEINLINE void SetConcealmentPop(double TranslationUnits, double RotationDegrees)
	{
		ReceiverMetrics.AvgConcealmentPopTranslation.store(TranslationUnits);
		ReceiverMetrics.AvgConcealmentPopRotationDeg.store(RotationDegrees);
	}

	// =====================================================================
	// TRANSPORT SIDE API
	// =====================================================================

	// Convenience wrappers that look the transport up by name on every call (one map lookup
	// under the registry lock). Per-frame paths hold an AcquireTransportMetrics() handle
	// instead (SHR-17).

	/** Record frame sent through specific transport */
	void RecordTransportFrameSent(FName TransportName, uint64 ByteCount) { AcquireTransportMetrics(TransportName)->RecordFrameSent(ByteCount); }

	/** Record frame received through specific transport */
	void RecordTransportFrameReceived(FName TransportName, uint64 ByteCount) { AcquireTransportMetrics(TransportName)->RecordFrameReceived(ByteCount); }

	/** Update connection state */
	void SetTransportConnected(FName TransportName, bool bConnected) { AcquireTransportMetrics(TransportName)->SetConnected(bConnected); }

	/** Record connection attempt */
	void RecordConnectionAttempt(FName TransportName) { AcquireTransportMetrics(TransportName)->RecordConnectionAttempt(); }

	/** Record pending frame count */
	void UpdateTransportPendingFrames(FName TransportName, int32 PendingCount) { AcquireTransportMetrics(TransportName)->UpdatePendingFrames(PendingCount); }

	/** Record transport error */
	void RecordTransportError(FName TransportName, bool bSendError = true) { AcquireTransportMetrics(TransportName)->RecordError(bSendError); }

	/** Record pipe count (for NNG) */
	void SetTransportPipeCount(FName TransportName, int32 PipeCount) { AcquireTransportMetrics(TransportName)->SetPipeCount(PipeCount); }

private:
	FO3DPerformanceMetrics() = default;
	~FO3DPerformanceMetrics() = default;

	// Prevent copying
	FO3DPerformanceMetrics(const FO3DPerformanceMetrics&) = delete;
	FO3DPerformanceMetrics& operator=(const FO3DPerformanceMetrics&) = delete;

	// Global metrics state
	FSenderMetrics SenderMetrics;
	FReceiverMetrics ReceiverMetrics;
	FO3DTransportMetricsRegistry TransportRegistry;
};
