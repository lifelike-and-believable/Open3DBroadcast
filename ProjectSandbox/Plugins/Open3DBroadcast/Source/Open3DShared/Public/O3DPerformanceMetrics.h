#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Misc/DateTime.h"
#include "Templates/Atomic.h"

DECLARE_LOG_CATEGORY_EXTERN(LogO3DPerformanceMetrics, Log, All);

/**
 * Real-time performance metrics collection for Open3DBroadcast plugin
 *
 * Thread-safe atomic counters with minimal overhead (<1% CPU impact)
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
		TAtomic<uint64> FramesCaptured{ 0 };           // Total frames captured from component
		TAtomic<uint64> FramesQueued{ 0 };             // Total frames queued to transport
		TAtomic<uint64> FramesDropped{ 0 };            // Frames dropped due to backpressure

		// Serialization
		TAtomic<uint64> BytesSerialized{ 0 };          // Total bytes serialized
		TAtomic<uint64> SerializationErrors{ 0 };      // Serialization failures
		TAtomic<double> AvgSerializationTimeMs{ 0.0 }; // Rolling average serialization latency

		// Transport send
		TAtomic<uint64> BytesSent{ 0 };                // Total bytes sent to network/transport
		TAtomic<uint64> TransportFramesDropped{ 0 };   // Frames dropped by transport layer
		TAtomic<int32> ActiveSubjectCount{ 0 };        // Current number of subjects being broadcast

		// Allocations
		TAtomic<uint64> AllocationCount{ 0 };          // Number of allocations in send path
		TAtomic<uint64> AllocationBytes{ 0 };          // Total bytes allocated in send path

		// Timestamps
		FDateTime MetricsStartTime = FDateTime::Now();
		FDateTime LastCaptureTime = FDateTime::Now();
		double FrameIntervalMs = 33.33;                // Estimated frame interval (updated dynamically)
	};

	/**
	 * Receiver-side metrics (LiveLink)
	 */
	struct FReceiverMetrics
	{
		// Frame reception
		TAtomic<uint64> FramesReceived{ 0 };           // Total frames received
		TAtomic<uint64> FramesApplied{ 0 };            // Frames successfully applied to LiveLink
		TAtomic<uint64> FramesDropped{ 0 };            // Frames dropped (duplicate, out-of-order, etc)

		// Deserialization
		TAtomic<uint64> BytesDeserialized{ 0 };        // Total bytes deserialized
		TAtomic<uint64> DeserializationErrors{ 0 };    // Deserialization failures
		TAtomic<double> AvgDeserializationTimeMs{ 0.0 }; // Rolling average deserialization latency

		// Per-operation timing (to identify bottlenecks)
		TAtomic<double> AvgParseTimeMs{ 0.0 };         // Rolling avg: FlatBuffer parse time
		TAtomic<double> AvgPoseExtractionTimeMs{ 0.0 }; // Rolling avg: bone structure extraction
		TAtomic<double> AvgLiveLinkPushTimeMs{ 0.0 };  // Rolling avg: LiveLink frame data push time
		TAtomic<double> AvgTotalProcessingTimeMs{ 0.0 }; // Rolling avg: total per-frame processing

		// LiveLink updates
		TAtomic<uint64> SkeletonUpdates{ 0 };          // Number of skeleton hierarchy updates
		TAtomic<uint64> PoseUpdates{ 0 };              // Number of pose frame updates
		TAtomic<int32> ActiveSubjectCount{ 0 };        // Current number of subjects being received

		// Latency tracking
		TAtomic<double> AvgRoundTripLatencyMs{ 0.0 };  // Rolling average RTT (if timestamped)
		TAtomic<double> MaxLatencyMs{ 0.0 };           // Peak latency in last collection period

		// A2: ReorderGate / ClockOffsetEstimator metrics (see O3DReceiverSource.cpp).
		// Gate counters are cumulative deltas summed across all receiver sources sharing
		// this singleton (each source tracks its own previous ReorderStats snapshot and
		// reports only the delta per tick, the same convention as FramesReceived etc.).
		TAtomic<uint64> GateDupDropped{ 0 };
		TAtomic<uint64> GateStaleDropped{ 0 };
		TAtomic<uint64> GateLost{ 0 };
		TAtomic<uint64> GateReordered{ 0 };
		// Current buffer occupancy is a gauge, not a cumulative counter; with multiple
		// receiver sources this is a last-writer-wins snapshot (same accepted
		// simplification as ActiveSubjectCount above - exact for the common single-source
		// case).
		TAtomic<int32> GateBufferOccupancy{ 0 };
		// Clock-offset estimate (caveated - see ClockOffsetEstimator's doc comment: only
		// meaningful as an absolute latency figure if clocks are known to be synced;
		// otherwise treat as relative/indicative) and jitter (excess delay above the
		// rolling-min floor), both rolling EMAs alpha=0.2 like AvgRoundTripLatencyMs above.
		TAtomic<double> AvgClockOffsetMs{ 0.0 };
		TAtomic<double> AvgJitterMs{ 0.0 };

		// C1: receiver-side concealment (see O3DS::ConcealmentEngine / roadmap
		// doc §5/C1.d). Counters are cumulative deltas summed across all
		// per-subject engines and all receiver sources sharing this singleton,
		// the same convention as GateDupDropped etc. above. The error/pop
		// averages are gauges (last-writer-wins snapshot of whichever
		// subject/source most recently had a recovery), same simplification
		// as AvgClockOffsetMs/GateBufferOccupancy.
		TAtomic<uint64> ConcealedFrames{ 0 };            // TryConceal() returned true (predicted or held)
		TAtomic<uint64> ConcealmentFallbackHolds{ 0 };   // ...of which, held rather than freshly predicted
		TAtomic<uint64> ConcealmentCorrectionFrames{ 0 }; // frames output during a post-recovery correction blend
		TAtomic<uint64> ConcealmentRecoveries{ 0 };      // concealment spans that ended in a real-frame recovery
		TAtomic<double> AvgConcealmentPredictionTranslationError{ 0.0 }; // scene units
		TAtomic<double> AvgConcealmentPredictionRotationErrorDeg{ 0.0 };
		TAtomic<double> AvgConcealmentPopTranslation{ 0.0 }; // discontinuity at recovery - what concealment should reduce vs Hold
		TAtomic<double> AvgConcealmentPopRotationDeg{ 0.0 };

		// Timestamps
		FDateTime MetricsStartTime = FDateTime::Now();
		FDateTime LastApplyTime = FDateTime::Now();
	};

	/**
	 * Transport-specific metrics
	 */
	struct FTransportMetrics
	{
		FString TransportName;

		// Connection state
		TAtomic<bool> bConnected{ false };
		TAtomic<int32> ConnectionAttempts{ 0 };
		TAtomic<int32> ReconnectCount{ 0 };

		// Data flow
		TAtomic<uint64> FramesSent{ 0 };               // Frames sent through transport
		TAtomic<uint64> BytesSent{ 0 };                // Bytes sent through transport
		TAtomic<uint64> FramesReceived{ 0 };           // Frames received (for bidirectional)
		TAtomic<uint64> BytesReceived{ 0 };            // Bytes received

		// Queue depth (for async transports)
		TAtomic<int32> PendingFrames{ 0 };             // Frames waiting to send
		TAtomic<int32> MaxPendingFrames{ 0 };          // Peak queue depth

		// Network stats
		TAtomic<double> AvgPacketLossPercent{ 0.0 };   // Estimated packet loss
		TAtomic<double> AvgLatencyMs{ 0.0 };           // Network latency estimate
		TAtomic<double> AvgBandwidthMbps{ 0.0 };       // Estimated bandwidth usage

		// Errors
		TAtomic<uint64> SendErrors{ 0 };
		TAtomic<uint64> ReceiveErrors{ 0 };
		TAtomic<int32> PipeCount{ 0 };                 // For NNG: number of connected pipes
	};

	/**
	 * Allocation tracking (for memory profiling)
	 */
	struct FAllocationRecord
	{
		FString Context;           // Where allocation occurred (e.g. "WebRTCSender::Send()")
		uint64 AllocationCount = 0;
		uint64 TotalBytes = 0;
		double AvgAllocationSizeBytes = 0.0;
		FDateTime LastUpdated = FDateTime::Now();
	};

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

	/** Get or create transport metrics by name */
	FTransportMetrics* GetOrCreateTransportMetrics(const FString& TransportName);

	/** Get transport metrics by name (returns nullptr if not found) */
	FTransportMetrics* FindTransportMetrics(const FString& TransportName);

	/** Get all transport metrics */
	const TArray<FTransportMetrics>& GetAllTransportMetrics() const { return TransportMetrics; }

	/** Dump all metrics to log/console */
	void DumpMetrics() const;

	/** Dump metrics in CSV format for external analysis */
	FString GetMetricsAsCSV() const;

	// =====================================================================
	// SENDER SIDE API (inline for minimal overhead)
	// =====================================================================

	FORCEINLINE void RecordFrameCaptured() { ++SenderMetrics.FramesCaptured; }
	FORCEINLINE void RecordFrameQueued() { ++SenderMetrics.FramesQueued; }
	FORCEINLINE void RecordFrameDropped() { ++SenderMetrics.FramesDropped; }
	FORCEINLINE void RecordBytesSerialized(uint64 ByteCount) { SenderMetrics.BytesSerialized += ByteCount; }
	FORCEINLINE void RecordSerializationError() { ++SenderMetrics.SerializationErrors; }
	FORCEINLINE void RecordBytesSent(uint64 ByteCount) { SenderMetrics.BytesSent += ByteCount; }
	FORCEINLINE void RecordTransportFrameDropped() { ++SenderMetrics.TransportFramesDropped; }
	FORCEINLINE void RecordAllocation(uint64 ByteCount)
	{
		++SenderMetrics.AllocationCount;
		SenderMetrics.AllocationBytes += ByteCount;
	}
	FORCEINLINE void SetActiveSubjectCount(int32 Count) { SenderMetrics.ActiveSubjectCount.Store(Count); }
	FORCEINLINE void UpdateFrameInterval(double IntervalMs) { SenderMetrics.FrameIntervalMs = IntervalMs; }

	// =====================================================================
	// RECEIVER SIDE API
	// =====================================================================

	FORCEINLINE void RecordFrameReceived() { ++ReceiverMetrics.FramesReceived; }
	FORCEINLINE void RecordFrameApplied() { ++ReceiverMetrics.FramesApplied; }
	FORCEINLINE void RecordReceiverFrameDropped(uint64 Delta = 1) { ReceiverMetrics.FramesDropped += Delta; }
	FORCEINLINE void RecordBytesDeserialized(uint64 ByteCount) { ReceiverMetrics.BytesDeserialized += ByteCount; }
	FORCEINLINE void RecordDeserializationError() { ++ReceiverMetrics.DeserializationErrors; }
	FORCEINLINE void RecordSkeletonUpdate() { ++ReceiverMetrics.SkeletonUpdates; }
	FORCEINLINE void RecordPoseUpdate() { ++ReceiverMetrics.PoseUpdates; }
	FORCEINLINE void SetReceiverActiveSubjectCount(int32 Count) { ReceiverMetrics.ActiveSubjectCount.Store(Count); }

	/** Record per-operation timing metrics (for bottleneck identification) */
	void RecordParseTimeMs(double TimeMs);
	void RecordPoseExtractionTimeMs(double TimeMs);
	void RecordLiveLinkPushTimeMs(double TimeMs);
	void RecordTotalProcessingTimeMs(double TimeMs);

	/** Record latency for a frame (timestamp-based) */
	void RecordFrameLatency(double LatencyMs);

	// A2.d: ReorderGate / ClockOffsetEstimator metrics. Counters take a delta (not a
	// cumulative total) - see FReceiverMetrics's gate counters doc comment.
	FORCEINLINE void RecordGateDupDropped(uint64 Delta) { ReceiverMetrics.GateDupDropped += Delta; }
	FORCEINLINE void RecordGateStaleDropped(uint64 Delta) { ReceiverMetrics.GateStaleDropped += Delta; }
	FORCEINLINE void RecordGateLost(uint64 Delta) { ReceiverMetrics.GateLost += Delta; }
	FORCEINLINE void RecordGateReordered(uint64 Delta) { ReceiverMetrics.GateReordered += Delta; }
	FORCEINLINE void SetGateBufferOccupancy(int32 Count) { ReceiverMetrics.GateBufferOccupancy.Store(Count); }

	/** Record one frame's clock-offset estimate and jitter (excess delay), both in ms. */
	void RecordClockOffsetSampleMs(double OffsetMs, double JitterMs);

	// C1.d: concealment metrics. Counters take a delta (not a cumulative
	// total), same convention as the gate counters above.
	FORCEINLINE void RecordConcealedFrames(uint64 Delta) { ReceiverMetrics.ConcealedFrames += Delta; }
	FORCEINLINE void RecordConcealmentFallbackHolds(uint64 Delta) { ReceiverMetrics.ConcealmentFallbackHolds += Delta; }
	FORCEINLINE void RecordConcealmentCorrectionFrames(uint64 Delta) { ReceiverMetrics.ConcealmentCorrectionFrames += Delta; }
	FORCEINLINE void RecordConcealmentRecoveries(uint64 Delta) { ReceiverMetrics.ConcealmentRecoveries += Delta; }
	FORCEINLINE void SetConcealmentPredictionError(double TranslationUnits, double RotationDegrees)
	{
		ReceiverMetrics.AvgConcealmentPredictionTranslationError.Store(TranslationUnits);
		ReceiverMetrics.AvgConcealmentPredictionRotationErrorDeg.Store(RotationDegrees);
	}
	FORCEINLINE void SetConcealmentPop(double TranslationUnits, double RotationDegrees)
	{
		ReceiverMetrics.AvgConcealmentPopTranslation.Store(TranslationUnits);
		ReceiverMetrics.AvgConcealmentPopRotationDeg.Store(RotationDegrees);
	}

	// =====================================================================
	// TRANSPORT SIDE API
	// =====================================================================

	/** Record frame sent through specific transport */
	void RecordTransportFrameSent(const FString& TransportName, uint64 ByteCount);

	/** Record frame received through specific transport */
	void RecordTransportFrameReceived(const FString& TransportName, uint64 ByteCount);

	/** Update connection state */
	void SetTransportConnected(const FString& TransportName, bool bConnected);

	/** Record connection attempt */
	void RecordConnectionAttempt(const FString& TransportName);

	/** Record pending frame count */
	void UpdateTransportPendingFrames(const FString& TransportName, int32 PendingCount);

	/** Record transport error */
	void RecordTransportError(const FString& TransportName, bool bSendError = true);

	/** Record pipe count (for NNG) */
	void SetTransportPipeCount(const FString& TransportName, int32 PipeCount);

	// =====================================================================
	// ALLOCATION TRACKING
	// =====================================================================

	/** Record allocation for profiling */
	void RecordAllocationsForContext(const FString& Context, uint64 Count, uint64 TotalBytes);

	/** Get allocation records */
	const TArray<FAllocationRecord>& GetAllocationRecords() const { return AllocationRecords; }

private:
	FO3DPerformanceMetrics() = default;
	~FO3DPerformanceMetrics() = default;

	// Prevent copying
	FO3DPerformanceMetrics(const FO3DPerformanceMetrics&) = delete;
	FO3DPerformanceMetrics& operator=(const FO3DPerformanceMetrics&) = delete;

	// Global metrics state
	FSenderMetrics SenderMetrics;
	FReceiverMetrics ReceiverMetrics;
	TArray<FTransportMetrics> TransportMetrics;
	TArray<FAllocationRecord> AllocationRecords;

	// Thread safety
	mutable FCriticalSection MetricsMutex;
};

// =====================================================================
// CONVENIENCE MACROS (for easy instrumentation)
// =====================================================================

#define O3D_RECORD_FRAME_CAPTURED() \
	FO3DPerformanceMetrics::Get().RecordFrameCaptured()

#define O3D_RECORD_FRAME_QUEUED() \
	FO3DPerformanceMetrics::Get().RecordFrameQueued()

#define O3D_RECORD_BYTES_SERIALIZED(ByteCount) \
	FO3DPerformanceMetrics::Get().RecordBytesSerialized(ByteCount)

#define O3D_RECORD_BYTES_SENT(ByteCount) \
	FO3DPerformanceMetrics::Get().RecordBytesSent(ByteCount)

#define O3D_SET_SUBJECT_COUNT(Count) \
	FO3DPerformanceMetrics::Get().SetActiveSubjectCount(Count)

#define O3D_RECORD_ALLOCATION(ByteCount) \
	FO3DPerformanceMetrics::Get().RecordAllocation(ByteCount)
