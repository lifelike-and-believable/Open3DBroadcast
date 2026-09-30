#include "O3DPerformanceMetrics.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h"

// Define log category for metrics
DEFINE_LOG_CATEGORY(LogO3DPerformanceMetrics);

// =====================================================================
// SINGLETON IMPLEMENTATION
// =====================================================================

FO3DPerformanceMetrics& FO3DPerformanceMetrics::Get()
{
	static FO3DPerformanceMetrics Instance;
	return Instance;
}

// =====================================================================
// TRANSPORT METRICS REGISTRY
// =====================================================================

FO3DTransportMetricsRegistry::FO3DTransportMetricsRegistry() = default;
FO3DTransportMetricsRegistry::~FO3DTransportMetricsRegistry() = default;

FO3DTransportMetricsRef FO3DTransportMetricsRegistry::Acquire(FName TransportName)
{
	FScopeLock Lock(&Mutex);
	if (const FO3DTransportMetricsRef* Existing = Entries.Find(TransportName))
	{
		return *Existing;
	}
	FO3DTransportMetricsRef Created = MakeShared<FO3DTransportMetrics, ESPMode::ThreadSafe>(TransportName);
	Entries.Add(TransportName, Created);
	return Created;
}

TSharedPtr<FO3DTransportMetrics, ESPMode::ThreadSafe> FO3DTransportMetricsRegistry::Find(FName TransportName) const
{
	FScopeLock Lock(&Mutex);
	if (const FO3DTransportMetricsRef* Existing = Entries.Find(TransportName))
	{
		return *Existing;
	}
	return nullptr;
}

TArray<FO3DTransportMetricsSnapshot> FO3DTransportMetricsRegistry::Snapshot() const
{
	// Copy the handles under the lock, read the atomics outside it.
	TArray<FO3DTransportMetricsRef> Handles;
	{
		FScopeLock Lock(&Mutex);
		Handles.Reserve(Entries.Num());
		for (const TPair<FName, FO3DTransportMetricsRef>& Pair : Entries)
		{
			Handles.Add(Pair.Value);
		}
	}

	TArray<FO3DTransportMetricsSnapshot> Result;
	Result.Reserve(Handles.Num());
	for (const FO3DTransportMetricsRef& Handle : Handles)
	{
		const FO3DTransportMetrics& M = *Handle;
		FO3DTransportMetricsSnapshot& Out = Result.AddDefaulted_GetRef();
		Out.TransportName = M.TransportName;
		Out.bConnected = M.bConnected.load();
		Out.ConnectionAttempts = M.ConnectionAttempts.load();
		Out.ReconnectCount = M.ReconnectCount.load();
		Out.FramesSent = M.FramesSent.load();
		Out.BytesSent = M.BytesSent.load();
		Out.FramesReceived = M.FramesReceived.load();
		Out.BytesReceived = M.BytesReceived.load();
		Out.PendingFrames = M.PendingFrames.load();
		Out.MaxPendingFrames = M.MaxPendingFrames.load();
		Out.SendErrors = M.SendErrors.load();
		Out.ReceiveErrors = M.ReceiveErrors.load();
		Out.PipeCount = M.PipeCount.load();
	}
	Result.Sort([](const FO3DTransportMetricsSnapshot& A, const FO3DTransportMetricsSnapshot& B)
	{
		return A.TransportName.Compare(B.TransportName) < 0;
	});
	return Result;
}

void FO3DTransportMetricsRegistry::ResetCounters()
{
	FScopeLock Lock(&Mutex);
	for (const TPair<FName, FO3DTransportMetricsRef>& Pair : Entries)
	{
		Pair.Value->ResetCounters();
	}
}

int32 FO3DTransportMetricsRegistry::Num() const
{
	FScopeLock Lock(&Mutex);
	return Entries.Num();
}

// =====================================================================
// CORE API IMPLEMENTATION
// =====================================================================

void FO3DPerformanceMetrics::Reset()
{
	// Sender metrics
	SenderMetrics.FramesCaptured.store(0);
	SenderMetrics.FramesQueued.store(0);
	SenderMetrics.FramesDropped.store(0);
	SenderMetrics.BytesSerialized.store(0);
	SenderMetrics.SerializationErrors.store(0);
	SenderMetrics.AvgSerializationTimeMs.store(0.0);
	SenderMetrics.BytesSent.store(0);
	SenderMetrics.TransportFramesDropped.store(0);
	SenderMetrics.AllocationCount.store(0);
	SenderMetrics.AllocationBytes.store(0);
	SenderMetrics.MetricsStartSeconds.store(FPlatformTime::Seconds());

	// Receiver metrics
	ReceiverMetrics.FramesReceived.store(0);
	ReceiverMetrics.FramesApplied.store(0);
	ReceiverMetrics.FramesDropped.store(0);
	ReceiverMetrics.BytesDeserialized.store(0);
	ReceiverMetrics.DeserializationErrors.store(0);
	ReceiverMetrics.AvgDeserializationTimeMs.store(0.0);
	ReceiverMetrics.SkeletonUpdates.store(0);
	ReceiverMetrics.PoseUpdates.store(0);
	ReceiverMetrics.AvgRoundTripLatencyMs.store(0.0);
	ReceiverMetrics.MaxLatencyMs.store(0.0);
	ReceiverMetrics.GateDupDropped.store(0);
	ReceiverMetrics.GateStaleDropped.store(0);
	ReceiverMetrics.GateLost.store(0);
	ReceiverMetrics.GateReordered.store(0);
	ReceiverMetrics.GateBufferOccupancy.store(0);
	ReceiverMetrics.AvgClockOffsetMs.store(0.0);
	ReceiverMetrics.AvgJitterMs.store(0.0);
	ReceiverMetrics.ConcealedFrames.store(0);
	ReceiverMetrics.ConcealmentFallbackHolds.store(0);
	ReceiverMetrics.ConcealmentCorrectionFrames.store(0);
	ReceiverMetrics.ConcealmentRecoveries.store(0);
	ReceiverMetrics.AvgConcealmentPredictionTranslationError.store(0.0);
	ReceiverMetrics.AvgConcealmentPredictionRotationErrorDeg.store(0.0);
	ReceiverMetrics.AvgConcealmentPopTranslation.store(0.0);
	ReceiverMetrics.AvgConcealmentPopRotationDeg.store(0.0);
	ReceiverMetrics.ConcealmentRenderAheadFrames.store(0);
	ReceiverMetrics.MetricsStartSeconds.store(FPlatformTime::Seconds());

	// Transport metrics
	TransportRegistry.ResetCounters();

	FScopeLock Lock(&MetricsMutex);
	AllocationRecords.Empty();
}

// =====================================================================
// LATENCY TRACKING (Rolling Average)
// =====================================================================

void FO3DPerformanceMetrics::RecordFrameLatency(double LatencyMs)
{
	// Exponential moving average, alpha = 0.2 (weights recent values more heavily). SHR-26:
	// compare-exchange loops, so concurrent receivers do not lose updates and the peak never
	// moves down.
	O3DMetrics::AtomicUpdateEma(ReceiverMetrics.AvgRoundTripLatencyMs, LatencyMs, 0.2);
	O3DMetrics::AtomicStoreMax(ReceiverMetrics.MaxLatencyMs, LatencyMs);
}

void FO3DPerformanceMetrics::RecordClockOffsetSampleMs(double OffsetMs, double JitterMs)
{
	const double Alpha = 0.2;
	O3DMetrics::AtomicUpdateEma(ReceiverMetrics.AvgClockOffsetMs, OffsetMs, Alpha);
	O3DMetrics::AtomicUpdateEma(ReceiverMetrics.AvgJitterMs, JitterMs, Alpha);
}

// =====================================================================
// PER-OPERATION TIMING TRACKING (for bottleneck identification)
// =====================================================================

void FO3DPerformanceMetrics::RecordParseTimeMs(double TimeMs)
{
	// Exponential moving average with alpha = 0.2
	O3DMetrics::AtomicUpdateEma(ReceiverMetrics.AvgParseTimeMs, TimeMs, 0.2);
}

void FO3DPerformanceMetrics::RecordPoseExtractionTimeMs(double TimeMs)
{
	// Exponential moving average with alpha = 0.2
	O3DMetrics::AtomicUpdateEma(ReceiverMetrics.AvgPoseExtractionTimeMs, TimeMs, 0.2);
}

void FO3DPerformanceMetrics::RecordLiveLinkPushTimeMs(double TimeMs)
{
	// Exponential moving average with alpha = 0.2
	O3DMetrics::AtomicUpdateEma(ReceiverMetrics.AvgLiveLinkPushTimeMs, TimeMs, 0.2);
}

void FO3DPerformanceMetrics::RecordTotalProcessingTimeMs(double TimeMs)
{
	// Exponential moving average with alpha = 0.2
	O3DMetrics::AtomicUpdateEma(ReceiverMetrics.AvgTotalProcessingTimeMs, TimeMs, 0.2);
}

// =====================================================================
// ALLOCATION TRACKING
// =====================================================================

void FO3DPerformanceMetrics::RecordAllocationsForContext(const FString& Context, uint64 Count, uint64 TotalBytes)
{
	FScopeLock Lock(&MetricsMutex);

	// Find or create record for this context
	FAllocationRecord* Record = nullptr;
	for (FAllocationRecord& R : AllocationRecords)
	{
		if (R.Context == Context)
		{
			Record = &R;
			break;
		}
	}

	if (!Record)
	{
		Record = &AllocationRecords.Add_GetRef(FAllocationRecord());
		Record->Context = Context;
	}

	Record->AllocationCount = Count;
	Record->TotalBytes = TotalBytes;
	if (Count > 0)
	{
		Record->AvgAllocationSizeBytes = static_cast<double>(TotalBytes) / static_cast<double>(Count);
	}
	Record->LastUpdated = FDateTime::Now();
}

TArray<FO3DPerformanceMetrics::FAllocationRecord> FO3DPerformanceMetrics::GetAllocationRecords() const
{
	FScopeLock Lock(&MetricsMutex);
	return AllocationRecords;
}

// =====================================================================
// METRICS DUMPING
// =====================================================================

void FO3DPerformanceMetrics::DumpMetrics() const
{
	// SHR-17: copy the shared state first and log without holding any lock.
	const TArray<FO3DTransportMetricsSnapshot> TransportMetrics = GetTransportMetricsSnapshot();
	const TArray<FAllocationRecord> AllocationRecordsCopy = GetAllocationRecords();

	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  O3D PERFORMANCE METRICS REPORT"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));

	// Uptime
	const double UptimeSeconds = FPlatformTime::Seconds() - SenderMetrics.MetricsStartSeconds.load();
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("Metrics Uptime: %.1f seconds"), UptimeSeconds);
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));

	// ========== SENDER METRICS ==========
	{
		uint64 FramesCaptured = SenderMetrics.FramesCaptured.load();
		uint64 FramesQueued = SenderMetrics.FramesQueued.load();
		uint64 FramesDropped = SenderMetrics.FramesDropped.load();
		uint64 BytesSerialized = SenderMetrics.BytesSerialized.load();
		uint64 BytesSent = SenderMetrics.BytesSent.load();
		uint64 AllocationCount = SenderMetrics.AllocationCount.load();
		uint64 AllocationBytes = SenderMetrics.AllocationBytes.load();
		int32 ActiveSubjects = SenderMetrics.ActiveSubjectCount.load();

		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[SENDER - BROADCAST]"));
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Frames Captured: %llu"), FramesCaptured);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Frames Queued: %llu"), FramesQueued);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Frames Dropped: %llu (%.2f%% drop rate)"),
			FramesDropped, FramesCaptured > 0 ? (100.0 * FramesDropped / FramesCaptured) : 0.0);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Bytes Serialized: %.2f MB"), BytesSerialized / 1024.0 / 1024.0);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Bytes Sent: %.2f MB"), BytesSent / 1024.0 / 1024.0);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Active Subjects: %d"), ActiveSubjects);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Allocations: %llu (%llu bytes, avg %.0f bytes/alloc)"),
			AllocationCount, AllocationBytes,
			AllocationCount > 0 ? (double)AllocationBytes / AllocationCount : 0.0);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Serialization Errors: %llu"), SenderMetrics.SerializationErrors.load());
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	}

	// ========== RECEIVER METRICS ==========
	{
		uint64 FramesReceived = ReceiverMetrics.FramesReceived.load();
		uint64 FramesApplied = ReceiverMetrics.FramesApplied.load();
		uint64 FramesDropped = ReceiverMetrics.FramesDropped.load();
		uint64 BytesDeserialized = ReceiverMetrics.BytesDeserialized.load();
		double AvgLatency = ReceiverMetrics.AvgRoundTripLatencyMs.load();
		double MaxLatency = ReceiverMetrics.MaxLatencyMs.load();
		int32 ActiveSubjects = ReceiverMetrics.ActiveSubjectCount.load();

		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[RECEIVER - LIVELINK]"));
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Frames Received: %llu"), FramesReceived);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Frames Applied: %llu"), FramesApplied);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Frames Dropped: %llu (%.2f%% drop rate)"),
			FramesDropped, FramesReceived > 0 ? (100.0 * FramesDropped / FramesReceived) : 0.0);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Bytes Deserialized: %.2f MB"), BytesDeserialized / 1024.0 / 1024.0);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Round-Trip Latency: %.2f ms"), AvgLatency);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Max Latency: %.2f ms"), MaxLatency);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Active Subjects: %d"), ActiveSubjects);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Skeleton Updates: %llu"), ReceiverMetrics.SkeletonUpdates.load());
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Pose Updates: %llu"), ReceiverMetrics.PoseUpdates.load());
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Deserialization Errors: %llu"), ReceiverMetrics.DeserializationErrors.load());
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));

		// Per-operation timing breakdown
		double AvgParseTime = ReceiverMetrics.AvgParseTimeMs.load();
		double AvgPoseExtraction = ReceiverMetrics.AvgPoseExtractionTimeMs.load();
		double AvgLiveLinkPush = ReceiverMetrics.AvgLiveLinkPushTimeMs.load();
		double AvgTotalProcessing = ReceiverMetrics.AvgTotalProcessingTimeMs.load();

		if (AvgParseTime > 0.0 || AvgPoseExtraction > 0.0 || AvgLiveLinkPush > 0.0 || AvgTotalProcessing > 0.0)
		{
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[RECEIVER - PER-OPERATION TIMING]"));
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg FlatBuffer Parse Time: %.3f ms"), AvgParseTime);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Pose Extraction Time: %.3f ms"), AvgPoseExtraction);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg LiveLink Push Time: %.3f ms"), AvgLiveLinkPush);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Total Processing Time: %.3f ms"), AvgTotalProcessing);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
		}

		// A2: reorder gate / clock-offset diagnostics
		{
			uint64 GateLost = ReceiverMetrics.GateLost.load();
			uint64 GateReordered = ReceiverMetrics.GateReordered.load();
			uint64 GateDupDropped = ReceiverMetrics.GateDupDropped.load();
			uint64 GateStaleDropped = ReceiverMetrics.GateStaleDropped.load();
			int32 GateOccupancy = ReceiverMetrics.GateBufferOccupancy.load();
			double AvgOffset = ReceiverMetrics.AvgClockOffsetMs.load();
			double AvgJitter = ReceiverMetrics.AvgJitterMs.load();

			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[RECEIVER - REORDER GATE / CLOCK MAPPING]"));
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Lost: %llu, Reordered: %llu, Duplicate: %llu, Stale: %llu"),
				GateLost, GateReordered, GateDupDropped, GateStaleDropped);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Gate Buffer Occupancy: %d"), GateOccupancy);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Clock Offset (relative unless clocks declared synced): %.2f ms"), AvgOffset);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Jitter (excess delay above rolling-min floor): %.2f ms"), AvgJitter);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
		}

		// C1: receiver-side concealment diagnostics
		{
			uint64 ConcealedFrames = ReceiverMetrics.ConcealedFrames.load();
			uint64 FallbackHolds = ReceiverMetrics.ConcealmentFallbackHolds.load();
			uint64 CorrectionFrames = ReceiverMetrics.ConcealmentCorrectionFrames.load();
			uint64 Recoveries = ReceiverMetrics.ConcealmentRecoveries.load();
			uint64 RenderAheadFrames = ReceiverMetrics.ConcealmentRenderAheadFrames.load();

			if (ConcealedFrames > 0 || CorrectionFrames > 0 || Recoveries > 0 || RenderAheadFrames > 0)
			{
				UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[RECEIVER - CONCEALMENT]"));
				UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Concealed Frames: %llu (%llu held rather than predicted)"), ConcealedFrames, FallbackHolds);
				UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Correction Frames: %llu, Recoveries: %llu"), CorrectionFrames, Recoveries);
				UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Prediction Error: %.3f units, %.2f deg"),
					ReceiverMetrics.AvgConcealmentPredictionTranslationError.load(), ReceiverMetrics.AvgConcealmentPredictionRotationErrorDeg.load());
				UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Pop (discontinuity at recovery): %.3f units, %.2f deg"),
					ReceiverMetrics.AvgConcealmentPopTranslation.load(), ReceiverMetrics.AvgConcealmentPopRotationDeg.load());
				UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Render-Ahead Frames (C1.c, opt-in): %llu"), RenderAheadFrames);
				UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
			}
		}
	}

	// ========== TRANSPORT METRICS ==========
	if (TransportMetrics.Num() > 0)
	{
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[TRANSPORTS]"));
		for (const FO3DTransportMetricsSnapshot& TMetrics : TransportMetrics)
		{
			const uint64 FramesSent = TMetrics.FramesSent;
			const uint64 BytesSent = TMetrics.BytesSent;
			const bool bConnected = TMetrics.bConnected;
			const int32 PendingFrames = TMetrics.PendingFrames;
			const int32 MaxPending = TMetrics.MaxPendingFrames;

			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  %s:"), *TMetrics.TransportName.ToString());
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Connected: %s"), bConnected ? TEXT("YES") : TEXT("NO"));
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Frames Sent: %llu"), FramesSent);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Bytes Sent: %.2f MB"), BytesSent / 1024.0 / 1024.0);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Pending Frames: %d (max: %d)"), PendingFrames, MaxPending);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Connection Attempts: %d, Reconnects: %d"),
				TMetrics.ConnectionAttempts, TMetrics.ReconnectCount);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Send Errors: %llu, Receive Errors: %llu"),
				TMetrics.SendErrors, TMetrics.ReceiveErrors);
			if (TMetrics.PipeCount > 0)
			{
				UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Pipes Connected: %d"), TMetrics.PipeCount);
			}
		}
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	}

	// ========== ALLOCATION BREAKDOWN ==========
	if (AllocationRecordsCopy.Num() > 0)
	{
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[ALLOCATION BREAKDOWN]"));
		for (const FAllocationRecord& Record : AllocationRecordsCopy)
		{
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  %s: %llu allocations, %.2f MB (avg %.0f bytes/alloc)"),
				*Record.Context, Record.AllocationCount,
				Record.TotalBytes / 1024.0 / 1024.0,
				Record.AvgAllocationSizeBytes);
		}
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	}

	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  End of Report"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
}

FString FO3DPerformanceMetrics::GetMetricsAsCSV() const
{
	// Reads atomics only; no lock needed.
	FString CSV;
	CSV += TEXT("Metric,Value\n");

	// Sender metrics
	CSV += FString::Printf(TEXT("FramesCaptured,%llu\n"), SenderMetrics.FramesCaptured.load());
	CSV += FString::Printf(TEXT("FramesQueued,%llu\n"), SenderMetrics.FramesQueued.load());
	CSV += FString::Printf(TEXT("FramesDropped,%llu\n"), SenderMetrics.FramesDropped.load());
	CSV += FString::Printf(TEXT("BytesSerialized,%llu\n"), SenderMetrics.BytesSerialized.load());
	CSV += FString::Printf(TEXT("BytesSent,%llu\n"), SenderMetrics.BytesSent.load());

	// Receiver metrics
	CSV += FString::Printf(TEXT("ReceiverFramesReceived,%llu\n"), ReceiverMetrics.FramesReceived.load());
	CSV += FString::Printf(TEXT("ReceiverFramesApplied,%llu\n"), ReceiverMetrics.FramesApplied.load());
	CSV += FString::Printf(TEXT("ReceiverFramesDropped,%llu\n"), ReceiverMetrics.FramesDropped.load());
	CSV += FString::Printf(TEXT("ReceiverBytesDeserialized,%llu\n"), ReceiverMetrics.BytesDeserialized.load());
	CSV += FString::Printf(TEXT("AvgRoundTripLatencyMs,%.2f\n"), ReceiverMetrics.AvgRoundTripLatencyMs.load());
	CSV += FString::Printf(TEXT("GateLost,%llu\n"), ReceiverMetrics.GateLost.load());
	CSV += FString::Printf(TEXT("GateReordered,%llu\n"), ReceiverMetrics.GateReordered.load());
	CSV += FString::Printf(TEXT("GateDupDropped,%llu\n"), ReceiverMetrics.GateDupDropped.load());
	CSV += FString::Printf(TEXT("GateStaleDropped,%llu\n"), ReceiverMetrics.GateStaleDropped.load());
	CSV += FString::Printf(TEXT("GateBufferOccupancy,%d\n"), ReceiverMetrics.GateBufferOccupancy.load());
	CSV += FString::Printf(TEXT("AvgClockOffsetMs,%.2f\n"), ReceiverMetrics.AvgClockOffsetMs.load());
	CSV += FString::Printf(TEXT("AvgJitterMs,%.2f\n"), ReceiverMetrics.AvgJitterMs.load());
	CSV += FString::Printf(TEXT("ConcealedFrames,%llu\n"), ReceiverMetrics.ConcealedFrames.load());
	CSV += FString::Printf(TEXT("ConcealmentFallbackHolds,%llu\n"), ReceiverMetrics.ConcealmentFallbackHolds.load());
	CSV += FString::Printf(TEXT("ConcealmentCorrectionFrames,%llu\n"), ReceiverMetrics.ConcealmentCorrectionFrames.load());
	CSV += FString::Printf(TEXT("ConcealmentRecoveries,%llu\n"), ReceiverMetrics.ConcealmentRecoveries.load());
	CSV += FString::Printf(TEXT("ConcealmentRenderAheadFrames,%llu\n"), ReceiverMetrics.ConcealmentRenderAheadFrames.load());
	CSV += FString::Printf(TEXT("AvgConcealmentPredictionTranslationError,%.4f\n"), ReceiverMetrics.AvgConcealmentPredictionTranslationError.load());
	CSV += FString::Printf(TEXT("AvgConcealmentPredictionRotationErrorDeg,%.2f\n"), ReceiverMetrics.AvgConcealmentPredictionRotationErrorDeg.load());
	CSV += FString::Printf(TEXT("AvgConcealmentPopTranslation,%.4f\n"), ReceiverMetrics.AvgConcealmentPopTranslation.load());
	CSV += FString::Printf(TEXT("AvgConcealmentPopRotationDeg,%.2f\n"), ReceiverMetrics.AvgConcealmentPopRotationDeg.load());

	return CSV;
}

// =====================================================================
// CONSOLE COMMAND REGISTRATION
// =====================================================================

void DumpO3DMetrics()
{
	FO3DPerformanceMetrics::Get().DumpMetrics();
}

static FAutoConsoleCommand DumpMetricsCmd(
	TEXT("o3d.DumpMetrics"),
	TEXT("Dump all Open3DBroadcast performance metrics"),
	FConsoleCommandDelegate::CreateStatic(&DumpO3DMetrics)
);

void ResetO3DMetrics()
{
	FO3DPerformanceMetrics::Get().Reset();
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("Performance metrics reset"));
}

static FAutoConsoleCommand ResetMetricsCmd(
	TEXT("o3d.ResetMetrics"),
	TEXT("Reset all Open3DBroadcast performance metrics"),
	FConsoleCommandDelegate::CreateStatic(&ResetO3DMetrics)
);

// =====================================================================
// PHASE 13 PROFILING COMMANDS - Main Thread Diagnostics
// =====================================================================

void PrintProfileGuide()
{
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("\n"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("========== PHASE 13: MAIN THREAD PROFILING GUIDE =========="));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("To diagnose the 2000+ ms latency spike root cause:"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("Step 1: Enable frame rate limiting (optional but recommended)"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  t.MaxFrameRate 30"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("Step 2: Monitor main thread activity during stalls"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  stat unit         - Overall frame breakdown (Game/Render/GPU time)"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  stat engine       - Engine subsystem times"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  stat game         - Game thread specific details"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  stat scenerendering - Rendering system details"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  stat gc           - Garbage collection stats"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("Step 3: Start animation and watch console"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  - Watch for spikes in Game thread time when animation stalls"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  - Note which system is running (shown in stat output)"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  - Check if GC is active during stalls (stat gc output)"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("Step 4: Collect metrics"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  o3d.DumpMetrics   - Show current receiver timing breakdown"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("Key Information from Phase 12:"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  - Receiver processes in 0.219-0.256 ms (EXCELLENT)"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  - LiveLink push takes 0.010-0.011 ms (INSTANT)"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  - Yet max latency is 2000+ ms (ASYNCHRONOUS QUEUE)"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  - The 2000 ms gap is spent WAITING for LiveLink queue"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("What to Look For:"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  1. If Game thread time spikes: Main thread is busy (GC/Render/Physics)"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  2. If stat gc shows activity: Garbage collection may be the culprit"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  3. If no obvious spike: LiveLink is batching/queuing updates"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("========== END PROFILING GUIDE ==========\n"));
}

static FAutoConsoleCommand ProfileGuideCmd(
	TEXT("o3d.ProfileGuide"),
	TEXT("Show Phase 13 main thread profiling guide"),
	FConsoleCommandDelegate::CreateStatic(&PrintProfileGuide)
);
