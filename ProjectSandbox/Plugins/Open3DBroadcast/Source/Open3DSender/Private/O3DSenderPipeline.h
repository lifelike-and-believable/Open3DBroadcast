// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include "O3DSPoseFramePool.h"
#include "O3DSenderComponent.h"
#include "O3DSenderPipelineStats.h"
#include "O3DSenderSerializer.h"

#include <atomic>

class IOpen3DSender;
class FO3DSenderCurveFilter;

/**
 * The per-sender pose pipeline (ADR 0008 items 2, 3 and 10; WP-A2c). The game thread samples
 * into a frame from this pipeline's pool and hands it over; a worker task then filters the curves,
 * serializes the frame and calls IOpen3DSender::SendSerialized. The worker owns everything
 * stateful (curve filter, serializer) and never touches a UObject: it reads only the frame, its
 * settings snapshot and this object.
 *
 * Threads.
 * - Owner thread (the component's game thread; a test's thread for a standalone pipeline): Start,
 *   Stop, RemoveSubject, AcquireFrame, FilterFrameInline, SubmitFrame, AttachSender, DetachSender,
 *   SetSerializedFrameListener, SetStatsLabel, SetDepthOverride. One owner thread at a time.
 * - Worker: a UE::Tasks task launched with ETaskPriority::BackgroundHigh when the queue goes from
 *   empty to non-empty. An atomic flag allows one drain task at a time, so items are processed
 *   one after another, in order (ADR 0008 item 3).
 * - Any thread: GetStats, IsIdle, WaitForIdle (never on the worker), GetSerializer (see there).
 *
 * Queue. Pose frames and control items (Start, Stop, RemoveSubject) share one FIFO, so control
 * reaches the worker in order with the frames around it. Control items are never dropped. At most
 * GetDepth() pose frames wait; when another arrives the oldest waiting frame goes back to the pool
 * (drop oldest, ADR 0008 open question 3) and FramesDropped grows. A frame is dropped before it is
 * serialized, so a full sync is never lost and the payloads that are sent are numbered without a
 * gap (FO3DSenderPipelineStats::LastSendSequence).
 *
 * Ownership. Every task holds a TSharedPtr to this object, so the owner may release its reference
 * with a task in flight; the last reference may then be dropped on the worker. The transport
 * sender is held here and released by DetachSender, which waits for a frame being processed: after
 * it returns the worker never calls the sender again, so the transport can be stopped and
 * released (ADR 0007 registry drain). SetSerializedFrameListener(nullptr) waits the same way. Both
 * wait for at most one frame (one serialize and one non-blocking SendSerialized), never for the
 * network or for queued frames.
 *
 * Synchronous mode (o3d.Sender.AsyncPipeline 0, latched by Start): every item is processed on the
 * owner thread inside the call, as before WP-A2c. Items an earlier asynchronous session left in
 * the queue are processed first.
 *
 * Must be created with MakeShared (tasks take a reference with AsShared).
 */
class FO3DSenderPipeline : public TSharedFromThis<FO3DSenderPipeline>
{
public:
	/** ADR 0008 item 3: default depth 2, console variable o3d.Sender.PipelineDepth clamped to 1..8. */
	static constexpr int32 DefaultDepth = 2;
	static constexpr int32 MinDepth = 1;
	static constexpr int32 MaxDepth = 8;

	FO3DSenderPipeline();
	~FO3DSenderPipeline();

	FO3DSenderPipeline(const FO3DSenderPipeline&) = delete;
	FO3DSenderPipeline& operator=(const FO3DSenderPipeline&) = delete;

	/**
	 * o3d.Sender.AsyncPipeline (default 1), read by the component when capture starts. False also
	 * when the platform runs without worker threads (FPlatformProcess::SupportsMultithreading).
	 */
	static bool IsAsyncEnabledByConsole();

	/** Begins a capture session in the given mode (Start control item: resets the curve filter). */
	void Start(bool bInAsync);

	/**
	 * Ends a capture session: queued pose frames are discarded (back to the pool, counted in
	 * FramesDiscardedOnStop) and a Stop control item clears the serializer caches and the curve
	 * filter, so the next session starts every subject with a full sync. Does not wait.
	 */
	void Stop();

	/** Forgets one subject's serializer state (rename, SND-1), in order with the queued frames. */
	void RemoveSubject(const FString& Subject);

	/** A pooled, empty frame; null when every frame is out (the sample is then skipped). */
	TUniquePtr<FO3DSPoseFrame> AcquireFrame();

	/**
	 * Synchronous mode only: filters Frame's curves on the calling thread, so OnPoseFrameReady
	 * listeners see the filtered curves as before WP-A2c. Pass bAlreadyFiltered to SubmitFrame.
	 */
	void FilterFrameInline(FO3DSPoseFrame& Frame);

	/** Hands a sampled frame over. The frame must come from AcquireFrame; it is not touched again by the caller. */
	void SubmitFrame(TUniquePtr<FO3DSPoseFrame>&& Frame, bool bAlreadyFiltered);

	/** Mode latched by the last Start. */
	bool IsAsync() const { return bAsync.load(); }

	/** The transport the worker sends to (the sender transport controller calls these). Wait for a frame being processed. */
	void AttachSender(const TSharedPtr<IOpen3DSender>& InSender);
	void DetachSender();

	/**
	 * Broadcast on the worker (or, in synchronous mode, the owner thread) after each serialized
	 * frame, before it is sent: the component's OnSerializedFrame. Null removes it; waits for a
	 * frame being processed, so the listener is never called after this returns.
	 */
	void SetSerializedFrameListener(FOnO3DSerializedFrame* InListener);

	/** Name for o3ds.Sender.DumpStats and o3d.Sender.DumpPipelineStats. */
	void SetStatsLabel(const FString& InLabel);

	/** Test hook: a fixed depth (clamped to MinDepth..MaxDepth) instead of the console variable; 0 restores it. */
	void SetDepthOverride(int32 InDepth) { DepthOverride.store(InDepth); }
	int32 GetDepth() const;

	/**
	 * The serializer the worker uses. Its stats getters are thread-safe; anything else (and every
	 * test that inspects it) must wait for IsIdle first, because the worker owns it.
	 */
	FO3DSenderSerializer& GetSerializer() const { return *Serializer; }

	FO3DSenderPipelineStats GetStats() const;

	/** True when nothing is queued and no drain task is scheduled or running. */
	bool IsIdle() const;

	/** Waits on an event (with a timeout) until IsIdle. Never call it on the worker. */
	bool WaitForIdle(double TimeoutSeconds) const;

	/** Waits until no drain task of any pipeline is scheduled or running (Open3DSender ShutdownModule, tests). */
	static bool WaitForAllIdle(double TimeoutSeconds);

	/** Drain tasks of every pipeline that are scheduled or running. */
	static int32 GetNumActiveDrainTasks();

	/** o3d.Sender.DumpPipelineStats: logs GetStats() of every live pipeline. */
	static void DumpAllStats();

private:
	enum class EItemKind : uint8
	{
		Frame,
		Start,
		Stop,
		RemoveSubject
	};

	struct FItem
	{
		EItemKind Kind = EItemKind::Frame;
		TUniquePtr<FO3DSPoseFrame> Frame;
		/** RemoveSubject: the subject to forget. */
		FString Subject;
		/** Frame: curves were already filtered (synchronous mode). */
		bool bAlreadyFiltered = false;
	};

	/** Queues Item (asynchronous mode) or processes it now (synchronous mode). */
	void Push(FItem&& Item);
	void ScheduleDrain();
	/** Body of a drain task. */
	void RunDrain();
	bool HasQueuedItems() const;

	/** The next queued item, processed. WorkerLock must be held. False when the queue is empty. */
	bool ProcessNextQueuedItemLocked();
	void ProcessItemLocked(FItem&& Item);
	void ProcessFrameLocked(FO3DSPoseFrame& Frame, bool bAlreadyFiltered);
	void SendLocked(const FO3DSPoseFrame& Frame, TArray<uint8>&& Bytes, bool bFullSync);

	/** Worker-owned. Created in the constructor and never replaced. */
	TUniquePtr<FO3DSenderSerializer> Serializer;
	TUniquePtr<FO3DSenderCurveFilter> CurveFilter;

	/** Frames for the owner thread to sample into; the worker gives them back. Capacity MaxDepth + 2. */
	FO3DSPoseFramePool Pool;

	/**
	 * Held while one item is processed (worker or, in synchronous mode, owner thread) and by the
	 * owner thread to change Sender or Listener. Taken before QueueLock, never after it.
	 */
	mutable FCriticalSection WorkerLock;
	TSharedPtr<IOpen3DSender> Sender;
	FOnO3DSerializedFrame* Listener = nullptr;

	/** Guards Queue, QueuedFrames and StatsLabel; held only to move pointers. */
	mutable FCriticalSection QueueLock;
	TArray<FItem> Queue;
	int32 QueuedFrames = 0;
	FString StatsLabel;

	/** One drain task at a time (ADR 0008 item 3, "drain task scheduled" flag). */
	std::atomic<bool> bDrainScheduled{ false };
	std::atomic<bool> bAsync{ true };
	std::atomic<int32> DepthOverride{ 0 };

	/** Triggered when a drain task ends; reset when one is scheduled. WaitForIdle re-checks IsIdle. */
	mutable FEventRef IdleEvent{ EEventMode::ManualReset };

	// Stats (FO3DSenderPipelineStats). Written by one thread at a time; read from any thread.
	std::atomic<uint64> FramesSubmitted{ 0 };
	std::atomic<uint64> FramesDropped{ 0 };
	std::atomic<uint64> FramesDiscardedOnStop{ 0 };
	std::atomic<uint64> FramesProcessed{ 0 };
	std::atomic<uint64> PayloadsHandedToTransport{ 0 };
	std::atomic<uint64> PayloadsAccepted{ 0 };
	std::atomic<uint64> PayloadsRefused{ 0 };
	std::atomic<uint64> PayloadsWithoutTransport{ 0 };
	std::atomic<uint64> LastSendSequence{ 0 };
	std::atomic<int32> MaxQueuedFrames{ 0 };
	std::atomic<double> LastWorkerSeconds{ 0.0 };
	std::atomic<double> MaxWorkerSeconds{ 0.0 };
	std::atomic<double> LastCaptureToSendSeconds{ 0.0 };
	std::atomic<double> MaxCaptureToSendSeconds{ 0.0 };
};
