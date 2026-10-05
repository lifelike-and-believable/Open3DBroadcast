// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DPerformanceMetrics.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h"
#include "O3DRuntimeContext.h"
#include "O3DRuntimeSubsystem.h"
#include "Engine/Engine.h"

// Define log category for metrics
DEFINE_LOG_CATEGORY(LogO3DPerformanceMetrics);

// =====================================================================
// DEFAULT CONTEXT
// =====================================================================

FO3DPerformanceMetrics& FO3DPerformanceMetrics::Get()
{
	return FO3DRuntimeContext::Default()->GetMetrics();
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
// RECEIVER HANDLES (ADR 0012 item 4)
// =====================================================================

void FO3DReceiverCounters::Reset()
{
	FramesReceived.store(0);
	FramesApplied.store(0);
	FramesDropped.store(0);
	BytesDeserialized.store(0);
	DeserializationErrors.store(0);
	UpdatesAwaitingFullSync.store(0);
	InvalidPosesDropped.store(0);
	SkeletonUpdates.store(0);
	PoseUpdates.store(0);
	GateDupDropped.store(0);
	GateStaleDropped.store(0);
	GateLost.store(0);
	GateReordered.store(0);
	ConcealedFrames.store(0);
	ConcealmentFallbackHolds.store(0);
	ConcealmentCorrectionFrames.store(0);
	ConcealmentRecoveries.store(0);
	ConcealmentRenderAheadFrames.store(0);
	ActiveSubjectCount.store(0);
	GateBufferOccupancy.store(0);
}

FO3DReceiverMetricsHandle::FO3DReceiverMetricsHandle(FO3DPerformanceMetrics& InAggregate, const FString& InOwnerName)
	: Aggregate(InAggregate)
	, OwnerName(InOwnerName)
{
}

FString FO3DReceiverMetricsHandle::GetOwnerName() const
{
	FScopeLock Lock(&NameMutex);
	return OwnerName;
}

void FO3DReceiverMetricsHandle::SetOwnerName(const FString& InOwnerName)
{
	FScopeLock Lock(&NameMutex);
	OwnerName = InOwnerName;
}

void FO3DSenderCounters::Reset()
{
	FramesCaptured.store(0);
	FramesDropped.store(0);
	BytesSerialized.store(0);
	BytesSent.store(0);
	TransportFramesDropped.store(0);
}

FO3DSenderMetricsHandle::FO3DSenderMetricsHandle(FO3DPerformanceMetrics& InAggregate, const FString& InOwnerName)
	: Aggregate(InAggregate)
	, OwnerName(InOwnerName)
{
}

FString FO3DSenderMetricsHandle::GetOwnerName() const
{
	FScopeLock Lock(&NameMutex);
	return OwnerName;
}

void FO3DSenderMetricsHandle::SetOwnerName(const FString& InOwnerName)
{
	FScopeLock Lock(&NameMutex);
	OwnerName = InOwnerName;
}

namespace O3DMetricsHandles
{
	/** Adds Handle to Handles, pruning released entries. Caller holds the lock. */
	template <typename HandleType>
	void Add(TArray<TWeakPtr<HandleType, ESPMode::ThreadSafe>>& Handles, const TSharedRef<HandleType, ESPMode::ThreadSafe>& Handle)
	{
		Handles.RemoveAll([](const TWeakPtr<HandleType, ESPMode::ThreadSafe>& Weak) { return !Weak.IsValid(); });
		Handles.Add(Handle);
	}

	/** The live entries of Handles, oldest first, pruning released ones. Caller holds the lock. */
	template <typename HandleType>
	TArray<TSharedRef<HandleType, ESPMode::ThreadSafe>> List(TArray<TWeakPtr<HandleType, ESPMode::ThreadSafe>>& Handles)
	{
		Handles.RemoveAll([](const TWeakPtr<HandleType, ESPMode::ThreadSafe>& Weak) { return !Weak.IsValid(); });
		TArray<TSharedRef<HandleType, ESPMode::ThreadSafe>> Result;
		Result.Reserve(Handles.Num());
		for (const TWeakPtr<HandleType, ESPMode::ThreadSafe>& Weak : Handles)
		{
			if (TSharedPtr<HandleType, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Result.Add(Pinned.ToSharedRef());
			}
		}
		return Result;
	}
}

FO3DReceiverMetricsHandleRef FO3DPerformanceMetrics::AcquireReceiverMetrics(const FString& OwnerName)
{
	FO3DReceiverMetricsHandleRef Handle = MakeShared<FO3DReceiverMetricsHandle, ESPMode::ThreadSafe>(*this, OwnerName);
	FScopeLock Lock(&HandlesMutex);
	O3DMetricsHandles::Add(ReceiverHandles, Handle);
	return Handle;
}

TArray<FO3DReceiverMetricsHandleRef> FO3DPerformanceMetrics::GetReceiverHandles() const
{
	FScopeLock Lock(&HandlesMutex);
	return O3DMetricsHandles::List(ReceiverHandles);
}

FO3DSenderMetricsHandleRef FO3DPerformanceMetrics::AcquireSenderMetrics(const FString& OwnerName)
{
	FO3DSenderMetricsHandleRef Handle = MakeShared<FO3DSenderMetricsHandle, ESPMode::ThreadSafe>(*this, OwnerName);
	FScopeLock Lock(&HandlesMutex);
	O3DMetricsHandles::Add(SenderHandles, Handle);
	return Handle;
}

TArray<FO3DSenderMetricsHandleRef> FO3DPerformanceMetrics::GetSenderHandles() const
{
	FScopeLock Lock(&HandlesMutex);
	return O3DMetricsHandles::List(SenderHandles);
}

// =====================================================================
// CORE API IMPLEMENTATION
// =====================================================================

void FO3DPerformanceMetrics::Reset()
{
	// Sender metrics
	SenderMetrics.FramesCaptured.store(0);
	SenderMetrics.FramesDropped.store(0);
	SenderMetrics.BytesSerialized.store(0);
	SenderMetrics.BytesSent.store(0);
	SenderMetrics.TransportFramesDropped.store(0);
	SenderMetrics.MetricsStartSeconds.store(FPlatformTime::Seconds());

	// Receiver metrics
	ReceiverMetrics.FramesReceived.store(0);
	ReceiverMetrics.FramesApplied.store(0);
	ReceiverMetrics.FramesDropped.store(0);
	ReceiverMetrics.BytesDeserialized.store(0);
	ReceiverMetrics.DeserializationErrors.store(0);
	ReceiverMetrics.InvalidPosesDropped.store(0);
	ReceiverMetrics.UpdatesAwaitingFullSync.store(0);
	ReceiverMetrics.AvgParseTimeMs.store(0.0);
	ReceiverMetrics.AvgPoseExtractionTimeMs.store(0.0);
	ReceiverMetrics.AvgLiveLinkPushTimeMs.store(0.0);
	ReceiverMetrics.AvgTotalProcessingTimeMs.store(0.0);
	ReceiverMetrics.SkeletonUpdates.store(0);
	ReceiverMetrics.PoseUpdates.store(0);
	ReceiverMetrics.ActiveSubjectCount.store(0);
	ReceiverMetrics.AvgReceiveToApplyLatencyMs.store(0.0);
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

	// The handles, so the aggregate keeps equalling their sum (ADR 0012 item 4).
	for (const FO3DReceiverMetricsHandleRef& Handle : GetReceiverHandles())
	{
		Handle->ResetCounters();
	}
	for (const FO3DSenderMetricsHandleRef& Handle : GetSenderHandles())
	{
		Handle->ResetCounters();
	}
}

// =====================================================================
// LATENCY TRACKING (Rolling Average)
// =====================================================================

void FO3DPerformanceMetrics::RecordFrameLatency(double LatencyMs)
{
	// Exponential moving average, alpha = 0.2 (weights recent values more heavily). SHR-26:
	// compare-exchange loops, so concurrent receivers do not lose updates and the peak never
	// moves down.
	O3DMetrics::AtomicUpdateEma(ReceiverMetrics.AvgReceiveToApplyLatencyMs, LatencyMs, 0.2);
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
// METRICS DUMPING
// =====================================================================

void FO3DPerformanceMetrics::DumpMetrics() const
{
	// SHR-17: copy the shared state first and log without holding any lock.
	const TArray<FO3DTransportMetricsSnapshot> TransportMetrics = GetTransportMetricsSnapshot();
	const TArray<FO3DReceiverMetricsHandleRef> ReceiverSources = GetReceiverHandles();
	const TArray<FO3DSenderMetricsHandleRef> Senders = GetSenderHandles();

	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  O3D PERFORMANCE METRICS REPORT"));
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));

	// Uptime
	const double UptimeSeconds = FPlatformTime::Seconds() - SenderMetrics.MetricsStartSeconds.load();
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("Metrics Uptime: %.1f seconds"), UptimeSeconds);
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));

	// ========== SENDER METRICS ==========
	{
		uint64 FramesCaptured = SenderMetrics.FramesCaptured.load();
		uint64 FramesDropped = SenderMetrics.FramesDropped.load();
		uint64 BytesSerialized = SenderMetrics.BytesSerialized.load();
		uint64 BytesSent = SenderMetrics.BytesSent.load();

		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("[SENDER - BROADCAST]"));
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Frames Captured: %llu"), FramesCaptured);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Frames Dropped: %llu (%.2f%% drop rate)"),
			FramesDropped, FramesCaptured > 0 ? (100.0 * FramesDropped / FramesCaptured) : 0.0);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Bytes Serialized: %.2f MB"), BytesSerialized / 1024.0 / 1024.0);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Bytes Sent: %.2f MB"), BytesSent / 1024.0 / 1024.0);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Transport Frames Dropped: %llu"), SenderMetrics.TransportFramesDropped.load());
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));
	}

	// ========== RECEIVER METRICS ==========
	{
		uint64 FramesReceived = ReceiverMetrics.FramesReceived.load();
		uint64 FramesApplied = ReceiverMetrics.FramesApplied.load();
		uint64 FramesDropped = ReceiverMetrics.FramesDropped.load();
		uint64 BytesDeserialized = ReceiverMetrics.BytesDeserialized.load();
		double AvgLatency = ReceiverMetrics.AvgReceiveToApplyLatencyMs.load();
		double MaxLatency = ReceiverMetrics.MaxLatencyMs.load();
		int32 ActiveSubjects = ReceiverMetrics.ActiveSubjectCount.load();

		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("[RECEIVER - LIVELINK]"));
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Frames Received: %llu"), FramesReceived);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Frames Applied: %llu"), FramesApplied);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Frames Dropped: %llu (%.2f%% drop rate)"),
			FramesDropped, FramesReceived > 0 ? (100.0 * FramesDropped / FramesReceived) : 0.0);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Bytes Deserialized: %.2f MB"), BytesDeserialized / 1024.0 / 1024.0);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Avg Receive-to-Apply Latency: %.2f ms"), AvgLatency);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Max Receive-to-Apply Latency: %.2f ms"), MaxLatency);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Active Subjects: %d"), ActiveSubjects);
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Skeleton Updates: %llu"), ReceiverMetrics.SkeletonUpdates.load());
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Pose Updates: %llu"), ReceiverMetrics.PoseUpdates.load());
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Deserialization Errors: %llu"), ReceiverMetrics.DeserializationErrors.load());
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Updates Awaiting Full Sync: %llu"), ReceiverMetrics.UpdatesAwaitingFullSync.load());
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Invalid Poses Dropped: %llu"), ReceiverMetrics.InvalidPosesDropped.load());
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));

		// Per-operation timing breakdown
		double AvgParseTime = ReceiverMetrics.AvgParseTimeMs.load();
		double AvgPoseExtraction = ReceiverMetrics.AvgPoseExtractionTimeMs.load();
		double AvgLiveLinkPush = ReceiverMetrics.AvgLiveLinkPushTimeMs.load();
		double AvgTotalProcessing = ReceiverMetrics.AvgTotalProcessingTimeMs.load();

		if (AvgParseTime > 0.0 || AvgPoseExtraction > 0.0 || AvgLiveLinkPush > 0.0 || AvgTotalProcessing > 0.0)
		{
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("[RECEIVER - PER-OPERATION TIMING]"));
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Avg FlatBuffer Parse Time: %.3f ms"), AvgParseTime);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Avg Pose Extraction Time: %.3f ms"), AvgPoseExtraction);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Avg LiveLink Push Time: %.3f ms"), AvgLiveLinkPush);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Avg Total Processing Time: %.3f ms"), AvgTotalProcessing);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));
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

			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("[RECEIVER - REORDER GATE / CLOCK MAPPING]"));
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Lost: %llu, Reordered: %llu, Duplicate: %llu, Stale: %llu"),
				GateLost, GateReordered, GateDupDropped, GateStaleDropped);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Gate Buffer Occupancy: %d"), GateOccupancy);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Avg Clock Offset (relative unless clocks declared synced): %.2f ms"), AvgOffset);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Avg Jitter (excess delay above rolling-min floor): %.2f ms"), AvgJitter);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));
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
				UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("[RECEIVER - CONCEALMENT]"));
				UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Concealed Frames: %llu (%llu held rather than predicted)"), ConcealedFrames, FallbackHolds);
				UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Correction Frames: %llu, Recoveries: %llu"), CorrectionFrames, Recoveries);
				UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Avg Prediction Error: %.3f units, %.2f deg"),
					ReceiverMetrics.AvgConcealmentPredictionTranslationError.load(), ReceiverMetrics.AvgConcealmentPredictionRotationErrorDeg.load());
				UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Avg Pop (discontinuity at recovery): %.3f units, %.2f deg"),
					ReceiverMetrics.AvgConcealmentPopTranslation.load(), ReceiverMetrics.AvgConcealmentPopRotationDeg.load());
				UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  Render-Ahead Frames (C1.c, opt-in): %llu"), RenderAheadFrames);
				UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));
			}
		}
	}

	// ========== PER SENDER (ADR 0012 item 4) ==========
	if (Senders.Num() > 0)
	{
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("[SENDERS]"));
		for (const FO3DSenderMetricsHandleRef& Handle : Senders)
		{
			const FO3DSenderCounters& C = Handle->GetCounters();
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  %s:"), *Handle->GetOwnerName());
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Frames Captured: %llu, Dropped: %llu, Transport Dropped: %llu"),
				C.FramesCaptured.load(), C.FramesDropped.load(), C.TransportFramesDropped.load());
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Bytes Serialized: %.2f MB, Sent: %.2f MB"),
				C.BytesSerialized.load() / 1024.0 / 1024.0, C.BytesSent.load() / 1024.0 / 1024.0);
		}
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));
	}

	// ========== PER RECEIVER SOURCE (ADR 0012 item 4) ==========
	if (ReceiverSources.Num() > 0)
	{
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("[RECEIVER SOURCES]"));
		for (const FO3DReceiverMetricsHandleRef& Handle : ReceiverSources)
		{
			const FO3DReceiverCounters& C = Handle->GetCounters();
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  %s:"), *Handle->GetOwnerName());
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Frames Received: %llu, Applied: %llu, Dropped: %llu"),
				C.FramesReceived.load(), C.FramesApplied.load(), C.FramesDropped.load());
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Bytes Deserialized: %.2f MB, Errors: %llu, Invalid Poses: %llu, Awaiting Full Sync: %llu"),
				C.BytesDeserialized.load() / 1024.0 / 1024.0, C.DeserializationErrors.load(), C.InvalidPosesDropped.load(), C.UpdatesAwaitingFullSync.load());
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Active Subjects: %d, Gate Lost: %llu, Concealed Frames: %llu"),
				C.ActiveSubjectCount.load(), C.GateLost.load(), C.ConcealedFrames.load());
		}
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));
	}

	// ========== TRANSPORT METRICS ==========
	if (TransportMetrics.Num() > 0)
	{
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("[TRANSPORTS]"));
		for (const FO3DTransportMetricsSnapshot& TMetrics : TransportMetrics)
		{
			const uint64 FramesSent = TMetrics.FramesSent;
			const uint64 BytesSent = TMetrics.BytesSent;
			const bool bConnected = TMetrics.bConnected;
			const int32 PendingFrames = TMetrics.PendingFrames;
			const int32 MaxPending = TMetrics.MaxPendingFrames;

			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  %s:"), *TMetrics.TransportName.ToString());
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Connected: %s"), bConnected ? TEXT("YES") : TEXT("NO"));
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Frames Sent: %llu"), FramesSent);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Bytes Sent: %.2f MB"), BytesSent / 1024.0 / 1024.0);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Pending Frames: %d (max: %d)"), PendingFrames, MaxPending);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Connection Attempts: %d, Reconnects: %d"),
				TMetrics.ConnectionAttempts, TMetrics.ReconnectCount);
			UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Send Errors: %llu, Receive Errors: %llu"),
				TMetrics.SendErrors, TMetrics.ReceiveErrors);
			if (TMetrics.PipeCount > 0)
			{
				UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("    Pipes Connected: %d"), TMetrics.PipeCount);
			}
		}
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));
	}

	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("  End of Report"));
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("========================================"));
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT(""));
}

FString FO3DPerformanceMetrics::GetMetricsAsCSV() const
{
	// Reads atomics, and the transports through their snapshot (taken under the registry lock).
	FString CSV;
	CSV += TEXT("Metric,Value\n");

	// Sender metrics
	CSV += FString::Printf(TEXT("FramesCaptured,%llu\n"), SenderMetrics.FramesCaptured.load());
	CSV += FString::Printf(TEXT("FramesDropped,%llu\n"), SenderMetrics.FramesDropped.load());
	CSV += FString::Printf(TEXT("BytesSerialized,%llu\n"), SenderMetrics.BytesSerialized.load());
	CSV += FString::Printf(TEXT("BytesSent,%llu\n"), SenderMetrics.BytesSent.load());
	CSV += FString::Printf(TEXT("TransportFramesDropped,%llu\n"), SenderMetrics.TransportFramesDropped.load());

	// Receiver metrics
	CSV += FString::Printf(TEXT("ReceiverFramesReceived,%llu\n"), ReceiverMetrics.FramesReceived.load());
	CSV += FString::Printf(TEXT("ReceiverFramesApplied,%llu\n"), ReceiverMetrics.FramesApplied.load());
	CSV += FString::Printf(TEXT("ReceiverFramesDropped,%llu\n"), ReceiverMetrics.FramesDropped.load());
	CSV += FString::Printf(TEXT("ReceiverBytesDeserialized,%llu\n"), ReceiverMetrics.BytesDeserialized.load());
	CSV += FString::Printf(TEXT("ReceiverInvalidPosesDropped,%llu\n"), ReceiverMetrics.InvalidPosesDropped.load());
	CSV += FString::Printf(TEXT("ReceiverUpdatesAwaitingFullSync,%llu\n"), ReceiverMetrics.UpdatesAwaitingFullSync.load());
	CSV += FString::Printf(TEXT("ReceiverDeserializationErrors,%llu\n"), ReceiverMetrics.DeserializationErrors.load());
	CSV += FString::Printf(TEXT("ReceiverSkeletonUpdates,%llu\n"), ReceiverMetrics.SkeletonUpdates.load());
	CSV += FString::Printf(TEXT("ReceiverPoseUpdates,%llu\n"), ReceiverMetrics.PoseUpdates.load());
	CSV += FString::Printf(TEXT("AvgReceiveToApplyLatencyMs,%.2f\n"), ReceiverMetrics.AvgReceiveToApplyLatencyMs.load());
	CSV += FString::Printf(TEXT("MaxReceiveToApplyLatencyMs,%.2f\n"), ReceiverMetrics.MaxLatencyMs.load());
	CSV += FString::Printf(TEXT("AvgParseTimeMs,%.3f\n"), ReceiverMetrics.AvgParseTimeMs.load());
	CSV += FString::Printf(TEXT("AvgPoseExtractionTimeMs,%.3f\n"), ReceiverMetrics.AvgPoseExtractionTimeMs.load());
	CSV += FString::Printf(TEXT("AvgLiveLinkPushTimeMs,%.3f\n"), ReceiverMetrics.AvgLiveLinkPushTimeMs.load());
	CSV += FString::Printf(TEXT("AvgTotalProcessingTimeMs,%.3f\n"), ReceiverMetrics.AvgTotalProcessingTimeMs.load());
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

	// Transports (SHR-25): one row per counter, prefixed with the transport name.
	for (const FO3DTransportMetricsSnapshot& Transport : GetTransportMetricsSnapshot())
	{
		const FString Prefix = FString::Printf(TEXT("Transport.%s."), *Transport.TransportName.ToString());
		CSV += FString::Printf(TEXT("%sConnected,%d\n"), *Prefix, Transport.bConnected ? 1 : 0);
		CSV += FString::Printf(TEXT("%sFramesSent,%llu\n"), *Prefix, Transport.FramesSent);
		CSV += FString::Printf(TEXT("%sBytesSent,%llu\n"), *Prefix, Transport.BytesSent);
		CSV += FString::Printf(TEXT("%sFramesReceived,%llu\n"), *Prefix, Transport.FramesReceived);
		CSV += FString::Printf(TEXT("%sBytesReceived,%llu\n"), *Prefix, Transport.BytesReceived);
		CSV += FString::Printf(TEXT("%sPendingFrames,%d\n"), *Prefix, Transport.PendingFrames);
		CSV += FString::Printf(TEXT("%sMaxPendingFrames,%d\n"), *Prefix, Transport.MaxPendingFrames);
		CSV += FString::Printf(TEXT("%sConnectionAttempts,%d\n"), *Prefix, Transport.ConnectionAttempts);
		CSV += FString::Printf(TEXT("%sReconnectCount,%d\n"), *Prefix, Transport.ReconnectCount);
		CSV += FString::Printf(TEXT("%sSendErrors,%llu\n"), *Prefix, Transport.SendErrors);
		CSV += FString::Printf(TEXT("%sReceiveErrors,%llu\n"), *Prefix, Transport.ReceiveErrors);
		CSV += FString::Printf(TEXT("%sPipeCount,%d\n"), *Prefix, Transport.PipeCount);
	}

	return CSV;
}

// =====================================================================
// CONSOLE COMMAND REGISTRATION
// =====================================================================

namespace O3DMetricsCommands
{
	/** The named runtime contexts (ADR 0012 item 5), or none before the engine starts. */
	TArray<FO3DRuntimeContextRef> NamedContexts()
	{
		if (GEngine != nullptr)
		{
			if (UO3DRuntimeSubsystem* Subsystem = GEngine->GetEngineSubsystem<UO3DRuntimeSubsystem>())
			{
				return Subsystem->GetNamedContexts();
			}
		}
		return {};
	}
}

void DumpO3DMetrics()
{
	// The default context first (the HUD and CSV show only it), then each named context.
	FO3DPerformanceMetrics::Get().DumpMetrics();
	for (const FO3DRuntimeContextRef& Context : O3DMetricsCommands::NamedContexts())
	{
		UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("Runtime context '%s':"), *Context->GetName().ToString());
		Context->GetMetrics().DumpMetrics();
	}
}

static FAutoConsoleCommand DumpMetricsCmd(
	TEXT("o3d.DumpMetrics"),
	TEXT("Dump all Open3DBroadcast performance metrics: the default runtime context, then each named one"),
	FConsoleCommandDelegate::CreateStatic(&DumpO3DMetrics)
);

void ResetO3DMetrics()
{
	// Every context, default and named.
	FO3DPerformanceMetrics::Get().Reset();
	for (const FO3DRuntimeContextRef& Context : O3DMetricsCommands::NamedContexts())
	{
		Context->GetMetrics().Reset();
	}
	UE_LOG(LogO3DPerformanceMetrics, Display, TEXT("Performance metrics reset"));
}

static FAutoConsoleCommand ResetMetricsCmd(
	TEXT("o3d.ResetMetrics"),
	TEXT("Reset all Open3DBroadcast performance metrics, in every runtime context"),
	FConsoleCommandDelegate::CreateStatic(&ResetO3DMetrics)
);
