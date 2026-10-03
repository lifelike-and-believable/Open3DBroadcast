// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DSenderPipeline.h"

#include "O3DSenderCurveProcessor.h"
#include "O3DSenderLogs.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Tasks/Task.h"

namespace
{
	TAutoConsoleVariable<int32> CVarO3DSenderAsyncPipeline(
		TEXT("o3d.Sender.AsyncPipeline"),
		1,
		TEXT("1 (default): sender components filter, serialize and send pose frames on a worker task (ADR 0008, WP-A2c). ")
		TEXT("0: on the game thread, as before WP-A2c. Read when capture starts. Kept for one release to isolate regressions."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarO3DSenderPipelineDepth(
		TEXT("o3d.Sender.PipelineDepth"),
		FO3DSenderPipeline::DefaultDepth,
		TEXT("Pose frames that may wait for a sender's worker (1 to 8, default 2). When another arrives the oldest waiting one is dropped (ADR 0008)."),
		ECVF_Default);

	/** Drain tasks of every pipeline that are scheduled or running (ShutdownModule waits for 0). */
	std::atomic<int32> GO3DSenderPipelineActiveDrains{ 0 };

	/** Guards GetSenderPipelineInstances(). */
	FCriticalSection& GetSenderPipelineInstancesLock()
	{
		static FCriticalSection InstancesLock;
		return InstancesLock;
	}

	/** Every live pipeline, for o3d.Sender.DumpPipelineStats. */
	TArray<FO3DSenderPipeline*>& GetSenderPipelineInstances()
	{
		static TArray<FO3DSenderPipeline*> Instances;
		return Instances;
	}

	/** Only the worker (one thread at a time) writes these, so a load and a store are enough. */
	void UpdatePipelineMaxSeconds(std::atomic<double>& Max, double Value)
	{
		if (Value > Max.load())
		{
			Max.store(Value);
		}
	}
}

FO3DSenderPipeline::FO3DSenderPipeline()
	: Serializer(MakeUnique<FO3DSenderSerializer>())
	, CurveFilter(MakeUnique<FO3DSenderCurveFilter>())
	, Pool(MaxDepth + 2)
{
	// Registered once, the first time a pipeline is created (the serializer registers
	// o3ds.Sender.DumpStats the same way).
	static const bool bRegisteredCmd = []()
	{
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("o3d.Sender.DumpPipelineStats"),
			TEXT("Dump the pose pipeline stats of every sender (queue, drops, worker time, capture-to-send latency) to the log"),
			FConsoleCommandDelegate::CreateStatic(&FO3DSenderPipeline::DumpAllStats),
			ECVF_Default);
		return true;
	}();
	(void)bRegisteredCmd;

	IdleEvent->Trigger();

	FScopeLock InstancesGuard(&GetSenderPipelineInstancesLock());
	GetSenderPipelineInstances().Add(this);
}

FO3DSenderPipeline::~FO3DSenderPipeline()
{
	// May run on the worker, when a task held the last reference. Touches no UObject.
	FScopeLock InstancesGuard(&GetSenderPipelineInstancesLock());
	GetSenderPipelineInstances().Remove(this);
}

bool FO3DSenderPipeline::IsAsyncEnabledByConsole()
{
	// Without worker threads (-nothreading) a launched task may only run when someone waits for
	// it, and nothing waits for a drain task, so frames would never leave: stay synchronous.
	return CVarO3DSenderAsyncPipeline.GetValueOnAnyThread() != 0 && FPlatformProcess::SupportsMultithreading();
}

int32 FO3DSenderPipeline::GetDepth() const
{
	const int32 Override = DepthOverride.load();
	const int32 Requested = Override > 0 ? Override : CVarO3DSenderPipelineDepth.GetValueOnAnyThread();
	return FMath::Clamp(Requested, MinDepth, MaxDepth);
}

void FO3DSenderPipeline::Start(bool bInAsync)
{
	bAsync.store(bInAsync);
	FItem Item;
	Item.Kind = EItemKind::Start;
	Push(MoveTemp(Item));
}

void FO3DSenderPipeline::Stop()
{
	// ADR 0008 item 10: queued frames are stale once capture stops; they are discarded, not sent.
	{
		FScopeLock QueueGuard(&QueueLock);
		for (int32 Index = Queue.Num() - 1; Index >= 0; --Index)
		{
			if (Queue[Index].Kind == EItemKind::Frame)
			{
				Pool.Release(MoveTemp(Queue[Index].Frame));
				Queue.RemoveAt(Index, 1, EAllowShrinking::No);
				FramesDiscardedOnStop.fetch_add(1);
			}
		}
		QueuedFrames = 0;
	}

	FItem Item;
	Item.Kind = EItemKind::Stop;
	Push(MoveTemp(Item));
}

void FO3DSenderPipeline::RemoveSubject(const FString& Subject)
{
	if (Subject.IsEmpty())
	{
		return;
	}
	FItem Item;
	Item.Kind = EItemKind::RemoveSubject;
	Item.Subject = Subject;
	Push(MoveTemp(Item));
}

TUniquePtr<FO3DSPoseFrame> FO3DSenderPipeline::AcquireFrame()
{
	return Pool.Acquire();
}

void FO3DSenderPipeline::FilterFrameInline(FO3DSPoseFrame& Frame)
{
	FScopeLock WorkerGuard(&WorkerLock);
	// Items an earlier asynchronous session left behind (a Stop that resets the filter) go first.
	while (ProcessNextQueuedItemLocked())
	{
	}
	CurveFilter->FilterFrame(Frame);
}

void FO3DSenderPipeline::SubmitFrame(TUniquePtr<FO3DSPoseFrame>&& Frame, bool bAlreadyFiltered)
{
	if (!Frame.IsValid())
	{
		return;
	}
	FramesSubmitted.fetch_add(1);
	FItem Item;
	Item.Kind = EItemKind::Frame;
	Item.Frame = MoveTemp(Frame);
	Item.bAlreadyFiltered = bAlreadyFiltered;
	Push(MoveTemp(Item));
}

void FO3DSenderPipeline::Push(FItem&& Item)
{
	if (!bAsync.load())
	{
		// Synchronous mode: process now, on this thread, after anything still queued.
		FScopeLock WorkerGuard(&WorkerLock);
		while (ProcessNextQueuedItemLocked())
		{
		}
		ProcessItemLocked(MoveTemp(Item));
		return;
	}

	{
		FScopeLock QueueGuard(&QueueLock);
		const bool bIsFrame = (Item.Kind == EItemKind::Frame);
		Queue.Add(MoveTemp(Item));
		if (bIsFrame)
		{
			++QueuedFrames;
			// Drop oldest (ADR 0008 open question 3): the frame that waited longest goes back to
			// the pool before it is serialized. Control items are never dropped.
			const int32 Depth = GetDepth();
			while (QueuedFrames > Depth)
			{
				const int32 Oldest = Queue.IndexOfByPredicate([](const FItem& Queued) { return Queued.Kind == EItemKind::Frame; });
				if (Oldest == INDEX_NONE)
				{
					break;
				}
				Pool.Release(MoveTemp(Queue[Oldest].Frame));
				Queue.RemoveAt(Oldest, 1, EAllowShrinking::No);
				--QueuedFrames;
				FramesDropped.fetch_add(1);
			}
			if (QueuedFrames > MaxQueuedFrames.load())
			{
				MaxQueuedFrames.store(QueuedFrames);
			}
		}
	}

	ScheduleDrain();
}

void FO3DSenderPipeline::ScheduleDrain()
{
	bool bExpected = false;
	if (!bDrainScheduled.compare_exchange_strong(bExpected, true))
	{
		// A drain task is scheduled or running; it re-checks the queue before it ends.
		return;
	}

	IdleEvent->Reset();
	GO3DSenderPipelineActiveDrains.fetch_add(1);

	// The task holds the pipeline, so the owner may release it while the task runs. The reference
	// is dropped inside the body, before the counter, so ShutdownModule's wait also covers a
	// pipeline destroyed on the worker.
	TSharedPtr<FO3DSenderPipeline> Self = AsShared();
	UE::Tasks::Launch(TEXT("O3DSenderPipelineDrain"), [Self]() mutable
	{
		Self->RunDrain();
		Self.Reset();
		GO3DSenderPipelineActiveDrains.fetch_sub(1);
	}, UE::Tasks::ETaskPriority::BackgroundHigh);
}

void FO3DSenderPipeline::RunDrain()
{
	for (;;)
	{
		for (;;)
		{
			// One item per lock, so DetachSender and SetSerializedFrameListener wait for at most one.
			FScopeLock WorkerGuard(&WorkerLock);
			if (!ProcessNextQueuedItemLocked())
			{
				break;
			}
		}

		bDrainScheduled.store(false);
		// An item pushed after the last check but before the flag was cleared found the flag set
		// and launched nothing: take it over unless another task already did.
		if (!HasQueuedItems())
		{
			break;
		}
		bool bExpected = false;
		if (!bDrainScheduled.compare_exchange_strong(bExpected, true))
		{
			break;
		}
	}
	// Not when another task has taken over meanwhile (it reset the event when it was scheduled).
	if (!bDrainScheduled.load())
	{
		IdleEvent->Trigger();
	}
}

bool FO3DSenderPipeline::HasQueuedItems() const
{
	FScopeLock QueueGuard(&QueueLock);
	return Queue.Num() > 0;
}

bool FO3DSenderPipeline::ProcessNextQueuedItemLocked()
{
	FItem Item;
	{
		FScopeLock QueueGuard(&QueueLock);
		if (Queue.Num() == 0)
		{
			return false;
		}
		Item = MoveTemp(Queue[0]);
		Queue.RemoveAt(0, 1, EAllowShrinking::No);
		if (Item.Kind == EItemKind::Frame)
		{
			--QueuedFrames;
		}
	}
	ProcessItemLocked(MoveTemp(Item));
	return true;
}

void FO3DSenderPipeline::ProcessItemLocked(FItem&& Item)
{
	switch (Item.Kind)
	{
	case EItemKind::Start:
		CurveFilter->Reset();
		break;
	case EItemKind::Stop:
		// The serializer state lives here, so it survives Stop/Start; every subject starts the next
		// session with a full sync (SND-1).
		Serializer->ClearAllCaches();
		CurveFilter->Reset();
		break;
	case EItemKind::RemoveSubject:
		Serializer->RemoveSubjectCache(Item.Subject);
		break;
	case EItemKind::Frame:
	default:
		if (Item.Frame.IsValid())
		{
			ProcessFrameLocked(*Item.Frame, Item.bAlreadyFiltered);
			Pool.Release(MoveTemp(Item.Frame));
		}
		break;
	}
}

void FO3DSenderPipeline::ProcessFrameLocked(FO3DSPoseFrame& Frame, bool bAlreadyFiltered)
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("O3D.Sender.Pipeline.Serialize");
	const double StartSeconds = FPlatformTime::Seconds();
	FramesProcessed.fetch_add(1);

	// The frame is owned here until it goes back to the pool; nobody else reads it.
	if (!bAlreadyFiltered)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("O3D.Sender.Pipeline.Filter");
		CurveFilter->FilterFrame(Frame);
	}

	TArray<uint8> Bytes;
	bool bFullSync = false;
	if (Serializer->SerializePoseFrameTo(Frame.Subject, Frame, Bytes, bFullSync))
	{
		if (Listener != nullptr)
		{
			Listener->Broadcast(Frame.Subject, Bytes, Frame.CaptureTimeSec);
		}
		SendLocked(Frame, MoveTemp(Bytes), bFullSync);
	}

	const double WorkerSeconds = FPlatformTime::Seconds() - StartSeconds;
	LastWorkerSeconds.store(WorkerSeconds);
	UpdatePipelineMaxSeconds(MaxWorkerSeconds, WorkerSeconds);
}

void FO3DSenderPipeline::SendLocked(const FO3DSPoseFrame& Frame, TArray<uint8>&& Bytes, bool bFullSync)
{
	if (!Sender.IsValid())
	{
		PayloadsWithoutTransport.fetch_add(1);
		RequestFullSyncAfterLostResidual(Frame);
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE_STR("O3D.Sender.Pipeline.Send");
	// Stamped at send time on the worker, so a frame dropped from the queue never leaves a gap.
	LastSendSequence.fetch_add(1);
	PayloadsHandedToTransport.fetch_add(1);
	const double CaptureToSendSeconds = FPlatformTime::Seconds() - Frame.CaptureTimeSec;
	LastCaptureToSendSeconds.store(CaptureToSendSeconds);
	UpdatePipelineMaxSeconds(MaxCaptureToSendSeconds, CaptureToSendSeconds);

	// The serializer's buffer moves into the transport (FO3DSendPayload owns its bytes, ADR 0007
	// item 3); no copy.
	const EO3DSendResult Result = Sender->SendSerialized(FO3DSendPayload(MoveTemp(Bytes), Frame.Subject, Frame.CaptureTimeSec, bFullSync));
	if (Result == EO3DSendResult::Queued)
	{
		PayloadsAccepted.fetch_add(1);
		return;
	}

	PayloadsRefused.fetch_add(1);
	if (bFullSync && Result == EO3DSendResult::DroppedBackpressure)
	{
		// ADR 0008 item 2 / ADR 0007 item 3: a refused full sync must not leave the receivers
		// with updates they cannot apply; the next frame of this subject is a full sync.
		Serializer->RequestFullSync(Frame.Subject);
	}
	else if (!bFullSync)
	{
		RequestFullSyncAfterLostResidual(Frame);
	}
	// Not retried: the next frame supersedes this one. DroppedBackpressure is counted in the
	// transport's DroppedFrames; NotConnected is expected while a peer or session is missing.
	UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Transport did not take subject '%s' (%s)."), *Frame.Subject, LexToString(Result));
}

void FO3DSenderPipeline::RequestFullSyncAfterLostResidual(const FO3DSPoseFrame& Frame)
{
	// ADR 0005 (ix): the serialized residual update consumed a tx_seq, so a receiver sees a gap
	// and holds the subject until the next full Subject. Send it on the next frame rather than
	// after FullSyncIntervalSeconds. Quantized updates stay applicable across a gap.
	if (Frame.Encoding.Mode == EO3DSenderEncodingMode::Residual)
	{
		Serializer->RequestFullSync(Frame.Subject);
	}
}

void FO3DSenderPipeline::AttachSender(const TSharedPtr<IOpen3DSender>& InSender)
{
	FScopeLock WorkerGuard(&WorkerLock);
	Sender = InSender;
}

void FO3DSenderPipeline::DetachSender()
{
	// Waits for a frame being processed; afterwards the worker never calls the sender again.
	TSharedPtr<IOpen3DSender> Released;
	{
		FScopeLock WorkerGuard(&WorkerLock);
		Released = MoveTemp(Sender);
		Sender.Reset();
	}
	// Released here, outside the lock, on the owner thread.
}

void FO3DSenderPipeline::SetSerializedFrameListener(FOnO3DSerializedFrame* InListener)
{
	FScopeLock WorkerGuard(&WorkerLock);
	Listener = InListener;
}

void FO3DSenderPipeline::SetStatsLabel(const FString& InLabel)
{
	{
		FScopeLock QueueGuard(&QueueLock);
		StatsLabel = InLabel;
	}
	Serializer->SetStatsLabel(InLabel);
}

FO3DSenderPipelineStats FO3DSenderPipeline::GetStats() const
{
	FO3DSenderPipelineStats Stats;
	Stats.FramesSubmitted = FramesSubmitted.load();
	Stats.FramesDropped = FramesDropped.load();
	Stats.FramesDiscardedOnStop = FramesDiscardedOnStop.load();
	Stats.FramesProcessed = FramesProcessed.load();
	Stats.PayloadsHandedToTransport = PayloadsHandedToTransport.load();
	Stats.PayloadsAccepted = PayloadsAccepted.load();
	Stats.PayloadsRefused = PayloadsRefused.load();
	Stats.PayloadsWithoutTransport = PayloadsWithoutTransport.load();
	Stats.LastSendSequence = LastSendSequence.load();
	{
		FScopeLock QueueGuard(&QueueLock);
		Stats.QueuedFrames = QueuedFrames;
	}
	Stats.MaxQueuedFrames = MaxQueuedFrames.load();
	Stats.LastWorkerSeconds = LastWorkerSeconds.load();
	Stats.MaxWorkerSeconds = MaxWorkerSeconds.load();
	Stats.LastCaptureToSendSeconds = LastCaptureToSendSeconds.load();
	Stats.MaxCaptureToSendSeconds = MaxCaptureToSendSeconds.load();
	Stats.bAsync = bAsync.load();
	return Stats;
}

bool FO3DSenderPipeline::IsIdle() const
{
	return !bDrainScheduled.load() && !HasQueuedItems();
}

bool FO3DSenderPipeline::WaitForIdle(double TimeoutSeconds) const
{
	const double Deadline = FPlatformTime::Seconds() + FMath::Max(0.0, TimeoutSeconds);
	for (;;)
	{
		if (IsIdle())
		{
			return true;
		}
		const double RemainingMs = (Deadline - FPlatformTime::Seconds()) * 1000.0;
		if (RemainingMs <= 0.0)
		{
			return IsIdle();
		}
		// The event ends the wait as soon as a drain task finishes; the cap only bounds a missed
		// trigger (a task scheduled between the check and the wait resets it). A trigger that
		// raced with a newly scheduled task can leave the event set while work remains; yield then
		// instead of spinning.
		if (IdleEvent->Wait((uint32)FMath::Clamp(RemainingMs, 1.0, 10.0)) && !IsIdle())
		{
			FPlatformProcess::Sleep(0.0f);
		}
	}
}

bool FO3DSenderPipeline::WaitForAllIdle(double TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + FMath::Max(0.0, TimeoutSeconds);
	while (GO3DSenderPipelineActiveDrains.load() > 0)
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			return false;
		}
		FPlatformProcess::Sleep(0.001f);
	}
	return true;
}

int32 FO3DSenderPipeline::GetNumActiveDrainTasks()
{
	return GO3DSenderPipelineActiveDrains.load();
}

void FO3DSenderPipeline::DumpAllStats()
{
	UE_LOG(LogO3DSender, Display, TEXT("---- O3D Sender Pipeline Stats ----"));
	FScopeLock InstancesGuard(&GetSenderPipelineInstancesLock());
	if (GetSenderPipelineInstances().Num() == 0)
	{
		UE_LOG(LogO3DSender, Display, TEXT("(no sender pipelines)"));
		return;
	}
	for (const FO3DSenderPipeline* Instance : GetSenderPipelineInstances())
	{
		if (Instance == nullptr)
		{
			continue;
		}
		const FO3DSenderPipelineStats Stats = Instance->GetStats();
		FString Label;
		{
			FScopeLock QueueGuard(&Instance->QueueLock);
			Label = Instance->StatsLabel;
		}
		UE_LOG(LogO3DSender, Display,
			TEXT("Pipeline(%s) async=%d depth=%d submitted=%llu dropped=%llu discardedOnStop=%llu processed=%llu sent=%llu accepted=%llu refused=%llu noTransport=%llu lastSeq=%llu queued=%d maxQueued=%d workerMs(last=%.3f max=%.3f) captureToSendMs(last=%.3f max=%.3f)"),
			Label.IsEmpty() ? TEXT("<unnamed>") : *Label,
			Stats.bAsync ? 1 : 0,
			Instance->GetDepth(),
			(unsigned long long)Stats.FramesSubmitted,
			(unsigned long long)Stats.FramesDropped,
			(unsigned long long)Stats.FramesDiscardedOnStop,
			(unsigned long long)Stats.FramesProcessed,
			(unsigned long long)Stats.PayloadsHandedToTransport,
			(unsigned long long)Stats.PayloadsAccepted,
			(unsigned long long)Stats.PayloadsRefused,
			(unsigned long long)Stats.PayloadsWithoutTransport,
			(unsigned long long)Stats.LastSendSequence,
			Stats.QueuedFrames,
			Stats.MaxQueuedFrames,
			Stats.LastWorkerSeconds * 1000.0,
			Stats.MaxWorkerSeconds * 1000.0,
			Stats.LastCaptureToSendSeconds * 1000.0,
			Stats.MaxCaptureToSendSeconds * 1000.0);
	}
}
