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
	FORCEINLINE void RecordReceiverFrameDropped() { ++ReceiverMetrics.FramesDropped; }
	FORCEINLINE void RecordBytesDeserialized(uint64 ByteCount) { ReceiverMetrics.BytesDeserialized += ByteCount; }
	FORCEINLINE void RecordDeserializationError() { ++ReceiverMetrics.DeserializationErrors; }
	FORCEINLINE void RecordSkeletonUpdate() { ++ReceiverMetrics.SkeletonUpdates; }
	FORCEINLINE void RecordPoseUpdate() { ++ReceiverMetrics.PoseUpdates; }
	FORCEINLINE void SetReceiverActiveSubjectCount(int32 Count) { ReceiverMetrics.ActiveSubjectCount.Store(Count); }

	/** Record latency for a frame (timestamp-based) */
	void RecordFrameLatency(double LatencyMs);

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
