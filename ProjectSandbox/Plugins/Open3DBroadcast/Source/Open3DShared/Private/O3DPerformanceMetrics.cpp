#include "O3DPerformanceMetrics.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"

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
// CORE API IMPLEMENTATION
// =====================================================================

void FO3DPerformanceMetrics::Reset()
{
	FScopeLock Lock(&MetricsMutex);

	// Sender metrics
	SenderMetrics.FramesCaptured.Store(0);
	SenderMetrics.FramesQueued.Store(0);
	SenderMetrics.FramesDropped.Store(0);
	SenderMetrics.BytesSerialized.Store(0);
	SenderMetrics.SerializationErrors.Store(0);
	SenderMetrics.AvgSerializationTimeMs.Store(0.0);
	SenderMetrics.BytesSent.Store(0);
	SenderMetrics.TransportFramesDropped.Store(0);
	SenderMetrics.AllocationCount.Store(0);
	SenderMetrics.AllocationBytes.Store(0);
	SenderMetrics.MetricsStartTime = FDateTime::Now();

	// Receiver metrics
	ReceiverMetrics.FramesReceived.Store(0);
	ReceiverMetrics.FramesApplied.Store(0);
	ReceiverMetrics.FramesDropped.Store(0);
	ReceiverMetrics.BytesDeserialized.Store(0);
	ReceiverMetrics.DeserializationErrors.Store(0);
	ReceiverMetrics.AvgDeserializationTimeMs.Store(0.0);
	ReceiverMetrics.SkeletonUpdates.Store(0);
	ReceiverMetrics.PoseUpdates.Store(0);
	ReceiverMetrics.AvgRoundTripLatencyMs.Store(0.0);
	ReceiverMetrics.MaxLatencyMs.Store(0.0);
	ReceiverMetrics.MetricsStartTime = FDateTime::Now();

	// Transport metrics
	for (FTransportMetrics& TMetrics : TransportMetrics)
	{
		TMetrics.FramesSent.Store(0);
		TMetrics.BytesSent.Store(0);
		TMetrics.FramesReceived.Store(0);
		TMetrics.BytesReceived.Store(0);
		TMetrics.PendingFrames.Store(0);
		TMetrics.MaxPendingFrames.Store(0);
		TMetrics.SendErrors.Store(0);
		TMetrics.ReceiveErrors.Store(0);
		TMetrics.ConnectionAttempts.Store(0);
		TMetrics.ReconnectCount.Store(0);
	}

	AllocationRecords.Empty();
}

FO3DPerformanceMetrics::FTransportMetrics* FO3DPerformanceMetrics::GetOrCreateTransportMetrics(const FString& TransportName)
{
	FScopeLock Lock(&MetricsMutex);

	// Find existing
	for (FTransportMetrics& Metrics : TransportMetrics)
	{
		if (Metrics.TransportName == TransportName)
		{
			return &Metrics;
		}
	}

	// Create new - use SetNum() instead of Add_GetRef() since FTransportMetrics has non-copyable TAtomic members
	int32 NewIndex = TransportMetrics.Num();
	TransportMetrics.SetNum(NewIndex + 1);
	TransportMetrics[NewIndex].TransportName = TransportName;
	return &TransportMetrics[NewIndex];
}

FO3DPerformanceMetrics::FTransportMetrics* FO3DPerformanceMetrics::FindTransportMetrics(const FString& TransportName)
{
	FScopeLock Lock(&MetricsMutex);

	for (FTransportMetrics& Metrics : TransportMetrics)
	{
		if (Metrics.TransportName == TransportName)
		{
			return &Metrics;
		}
	}

	return nullptr;
}

// =====================================================================
// LATENCY TRACKING (Rolling Average)
// =====================================================================

void FO3DPerformanceMetrics::RecordFrameLatency(double LatencyMs)
{
	// Simple exponential moving average: α = 0.2 (weights recent values more heavily)
	const double Alpha = 0.2;
	const double CurrentAvg = ReceiverMetrics.AvgRoundTripLatencyMs.Load();
	const double NewAvg = (CurrentAvg * (1.0 - Alpha)) + (LatencyMs * Alpha);
	ReceiverMetrics.AvgRoundTripLatencyMs.Store(NewAvg);

	// Track peak
	if (LatencyMs > ReceiverMetrics.MaxLatencyMs.Load())
	{
		ReceiverMetrics.MaxLatencyMs.Store(LatencyMs);
	}
}

// =====================================================================
// PER-OPERATION TIMING TRACKING (for bottleneck identification)
// =====================================================================

void FO3DPerformanceMetrics::RecordParseTimeMs(double TimeMs)
{
	// Exponential moving average with α = 0.2
	const double Alpha = 0.2;
	const double CurrentAvg = ReceiverMetrics.AvgParseTimeMs.Load();
	const double NewAvg = (CurrentAvg * (1.0 - Alpha)) + (TimeMs * Alpha);
	ReceiverMetrics.AvgParseTimeMs.Store(NewAvg);
}

void FO3DPerformanceMetrics::RecordPoseExtractionTimeMs(double TimeMs)
{
	// Exponential moving average with α = 0.2
	const double Alpha = 0.2;
	const double CurrentAvg = ReceiverMetrics.AvgPoseExtractionTimeMs.Load();
	const double NewAvg = (CurrentAvg * (1.0 - Alpha)) + (TimeMs * Alpha);
	ReceiverMetrics.AvgPoseExtractionTimeMs.Store(NewAvg);
}

void FO3DPerformanceMetrics::RecordLiveLinkPushTimeMs(double TimeMs)
{
	// Exponential moving average with α = 0.2
	const double Alpha = 0.2;
	const double CurrentAvg = ReceiverMetrics.AvgLiveLinkPushTimeMs.Load();
	const double NewAvg = (CurrentAvg * (1.0 - Alpha)) + (TimeMs * Alpha);
	ReceiverMetrics.AvgLiveLinkPushTimeMs.Store(NewAvg);
}

void FO3DPerformanceMetrics::RecordTotalProcessingTimeMs(double TimeMs)
{
	// Exponential moving average with α = 0.2
	const double Alpha = 0.2;
	const double CurrentAvg = ReceiverMetrics.AvgTotalProcessingTimeMs.Load();
	const double NewAvg = (CurrentAvg * (1.0 - Alpha)) + (TimeMs * Alpha);
	ReceiverMetrics.AvgTotalProcessingTimeMs.Store(NewAvg);
}

// =====================================================================
// TRANSPORT TRACKING
// =====================================================================

void FO3DPerformanceMetrics::RecordTransportFrameSent(const FString& TransportName, uint64 ByteCount)
{
	if (FTransportMetrics* TMetrics = GetOrCreateTransportMetrics(TransportName))
	{
		++TMetrics->FramesSent;
		TMetrics->BytesSent += ByteCount;
	}
}

void FO3DPerformanceMetrics::RecordTransportFrameReceived(const FString& TransportName, uint64 ByteCount)
{
	if (FTransportMetrics* TMetrics = GetOrCreateTransportMetrics(TransportName))
	{
		++TMetrics->FramesReceived;
		TMetrics->BytesReceived += ByteCount;
	}
}

void FO3DPerformanceMetrics::SetTransportConnected(const FString& TransportName, bool bConnected)
{
	if (FTransportMetrics* TMetrics = GetOrCreateTransportMetrics(TransportName))
	{
		TMetrics->bConnected.Store(bConnected);
	}
}

void FO3DPerformanceMetrics::RecordConnectionAttempt(const FString& TransportName)
{
	if (FTransportMetrics* TMetrics = GetOrCreateTransportMetrics(TransportName))
	{
		++TMetrics->ConnectionAttempts;
	}
}

void FO3DPerformanceMetrics::UpdateTransportPendingFrames(const FString& TransportName, int32 PendingCount)
{
	if (FTransportMetrics* TMetrics = GetOrCreateTransportMetrics(TransportName))
	{
		TMetrics->PendingFrames.Store(PendingCount);
		if (PendingCount > TMetrics->MaxPendingFrames.Load())
		{
			TMetrics->MaxPendingFrames.Store(PendingCount);
		}
	}
}

void FO3DPerformanceMetrics::RecordTransportError(const FString& TransportName, bool bSendError)
{
	if (FTransportMetrics* TMetrics = GetOrCreateTransportMetrics(TransportName))
	{
		if (bSendError)
		{
			++TMetrics->SendErrors;
		}
		else
		{
			++TMetrics->ReceiveErrors;
		}
	}
}

void FO3DPerformanceMetrics::SetTransportPipeCount(const FString& TransportName, int32 PipeCount)
{
	if (FTransportMetrics* TMetrics = GetOrCreateTransportMetrics(TransportName))
	{
		TMetrics->PipeCount.Store(PipeCount);
	}
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

// =====================================================================
// METRICS DUMPING
// =====================================================================

void FO3DPerformanceMetrics::DumpMetrics() const
{
	FScopeLock Lock(&MetricsMutex);

	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  O3D PERFORMANCE METRICS REPORT"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));

	// Uptime
	FTimespan Uptime = FDateTime::Now() - SenderMetrics.MetricsStartTime;
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("Metrics Uptime: %.1f seconds"), Uptime.GetTotalSeconds());
	UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));

	// ========== SENDER METRICS ==========
	{
		uint64 FramesCaptured = SenderMetrics.FramesCaptured.Load();
		uint64 FramesQueued = SenderMetrics.FramesQueued.Load();
		uint64 FramesDropped = SenderMetrics.FramesDropped.Load();
		uint64 BytesSerialized = SenderMetrics.BytesSerialized.Load();
		uint64 BytesSent = SenderMetrics.BytesSent.Load();
		uint64 AllocationCount = SenderMetrics.AllocationCount.Load();
		uint64 AllocationBytes = SenderMetrics.AllocationBytes.Load();
		int32 ActiveSubjects = SenderMetrics.ActiveSubjectCount.Load();

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
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Serialization Errors: %llu"), SenderMetrics.SerializationErrors.Load());
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	}

	// ========== RECEIVER METRICS ==========
	{
		uint64 FramesReceived = ReceiverMetrics.FramesReceived.Load();
		uint64 FramesApplied = ReceiverMetrics.FramesApplied.Load();
		uint64 FramesDropped = ReceiverMetrics.FramesDropped.Load();
		uint64 BytesDeserialized = ReceiverMetrics.BytesDeserialized.Load();
		double AvgLatency = ReceiverMetrics.AvgRoundTripLatencyMs.Load();
		double MaxLatency = ReceiverMetrics.MaxLatencyMs.Load();
		int32 ActiveSubjects = ReceiverMetrics.ActiveSubjectCount.Load();

		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[RECEIVER - LIVELINK]"));
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Frames Received: %llu"), FramesReceived);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Frames Applied: %llu"), FramesApplied);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Frames Dropped: %llu (%.2f%% drop rate)"),
			FramesDropped, FramesReceived > 0 ? (100.0 * FramesDropped / FramesReceived) : 0.0);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Bytes Deserialized: %.2f MB"), BytesDeserialized / 1024.0 / 1024.0);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Round-Trip Latency: %.2f ms"), AvgLatency);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Max Latency: %.2f ms"), MaxLatency);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Active Subjects: %d"), ActiveSubjects);
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Skeleton Updates: %llu"), ReceiverMetrics.SkeletonUpdates.Load());
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Pose Updates: %llu"), ReceiverMetrics.PoseUpdates.Load());
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Deserialization Errors: %llu"), ReceiverMetrics.DeserializationErrors.Load());
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));

		// Per-operation timing breakdown
		double AvgParseTime = ReceiverMetrics.AvgParseTimeMs.Load();
		double AvgPoseExtraction = ReceiverMetrics.AvgPoseExtractionTimeMs.Load();
		double AvgLiveLinkPush = ReceiverMetrics.AvgLiveLinkPushTimeMs.Load();
		double AvgTotalProcessing = ReceiverMetrics.AvgTotalProcessingTimeMs.Load();

		if (AvgParseTime > 0.0 || AvgPoseExtraction > 0.0 || AvgLiveLinkPush > 0.0 || AvgTotalProcessing > 0.0)
		{
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[RECEIVER - PER-OPERATION TIMING]"));
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg FlatBuffer Parse Time: %.3f ms"), AvgParseTime);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Pose Extraction Time: %.3f ms"), AvgPoseExtraction);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg LiveLink Push Time: %.3f ms"), AvgLiveLinkPush);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  Avg Total Processing Time: %.3f ms"), AvgTotalProcessing);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
		}
	}

	// ========== TRANSPORT METRICS ==========
	if (TransportMetrics.Num() > 0)
	{
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[TRANSPORTS]"));
		for (const FTransportMetrics& TMetrics : TransportMetrics)
		{
			uint64 FramesSent = TMetrics.FramesSent.Load();
			uint64 BytesSent = TMetrics.BytesSent.Load();
			bool bConnected = TMetrics.bConnected.Load();
			int32 PendingFrames = TMetrics.PendingFrames.Load();
			int32 MaxPending = TMetrics.MaxPendingFrames.Load();

			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("  %s:"), *TMetrics.TransportName);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Connected: %s"), bConnected ? TEXT("YES") : TEXT("NO"));
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Frames Sent: %llu"), FramesSent);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Bytes Sent: %.2f MB"), BytesSent / 1024.0 / 1024.0);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Pending Frames: %d (max: %d)"), PendingFrames, MaxPending);
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Connection Attempts: %d, Reconnects: %d"),
				TMetrics.ConnectionAttempts.Load(), TMetrics.ReconnectCount.Load());
			UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Send Errors: %llu, Receive Errors: %llu"),
				TMetrics.SendErrors.Load(), TMetrics.ReceiveErrors.Load());
			if (TMetrics.PipeCount > 0)
			{
				UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("    Pipes Connected: %d"), TMetrics.PipeCount.Load());
			}
		}
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT(""));
	}

	// ========== ALLOCATION BREAKDOWN ==========
	if (AllocationRecords.Num() > 0)
	{
		UE_LOG(LogO3DPerformanceMetrics, Warning, TEXT("[ALLOCATION BREAKDOWN]"));
		for (const FAllocationRecord& Record : AllocationRecords)
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
	FScopeLock Lock(&MetricsMutex);

	FString CSV;
	CSV += TEXT("Metric,Value\n");

	// Sender metrics
	CSV += FString::Printf(TEXT("FramesCaptured,%llu\n"), SenderMetrics.FramesCaptured.Load());
	CSV += FString::Printf(TEXT("FramesQueued,%llu\n"), SenderMetrics.FramesQueued.Load());
	CSV += FString::Printf(TEXT("FramesDropped,%llu\n"), SenderMetrics.FramesDropped.Load());
	CSV += FString::Printf(TEXT("BytesSerialized,%llu\n"), SenderMetrics.BytesSerialized.Load());
	CSV += FString::Printf(TEXT("BytesSent,%llu\n"), SenderMetrics.BytesSent.Load());

	// Receiver metrics
	CSV += FString::Printf(TEXT("ReceiverFramesReceived,%llu\n"), ReceiverMetrics.FramesReceived.Load());
	CSV += FString::Printf(TEXT("ReceiverFramesApplied,%llu\n"), ReceiverMetrics.FramesApplied.Load());
	CSV += FString::Printf(TEXT("ReceiverFramesDropped,%llu\n"), ReceiverMetrics.FramesDropped.Load());
	CSV += FString::Printf(TEXT("ReceiverBytesDeserialized,%llu\n"), ReceiverMetrics.BytesDeserialized.Load());
	CSV += FString::Printf(TEXT("AvgRoundTripLatencyMs,%.2f\n"), ReceiverMetrics.AvgRoundTripLatencyMs.Load());

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
