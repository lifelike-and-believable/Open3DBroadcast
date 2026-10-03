// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Counters of one sender pipeline (ADR 0008 items 3 and 11, WP-A2c), copied from atomics, so
 * reading them from any thread never races with the worker. Times are on the sender clock
 * (FPlatformTime::Seconds()). UO3DSenderComponent::GetPipelineStats() returns them and
 * o3d.Sender.DumpPipelineStats logs them for every live pipeline.
 */
struct FO3DSenderPipelineStats
{
	/** Sampled pose frames handed to the pipeline. */
	uint64 FramesSubmitted = 0;
	/** Pose frames dropped from the queue to make room for a newer one (drop oldest; Stats.PipelineDropped in ADR 0008). */
	uint64 FramesDropped = 0;
	/** Queued pose frames discarded by StopCapture (stale by definition, ADR 0008 item 10). */
	uint64 FramesDiscardedOnStop = 0;
	/** Pose frames the worker took from the queue and processed (filter and serializer). */
	uint64 FramesProcessed = 0;
	/** Payloads handed to IOpen3DSender::SendSerialized. */
	uint64 PayloadsHandedToTransport = 0;
	/** Of those, the ones the transport accepted (EO3DSendResult::Queued). */
	uint64 PayloadsAccepted = 0;
	/** Of those, the ones the transport refused (any other result; not retried). */
	uint64 PayloadsRefused = 0;
	/** Serialized payloads with no transport attached (capture without a running transport). */
	uint64 PayloadsWithoutTransport = 0;
	/**
	 * Sequence number of the last payload handed to the transport, starting at 1 and stamped on
	 * the worker at send time, so the payloads that are sent are numbered without a gap whatever
	 * the queue dropped. Counts every subject's payloads together; the tx_seq on the wire is
	 * counted per subject by the serializer's O3DS::StreamWriter (ADR 0005 (iv)).
	 */
	uint64 LastSendSequence = 0;
	/** Pose frames waiting in the queue now, and the most that ever waited (never above the depth). */
	int32 QueuedFrames = 0;
	int32 MaxQueuedFrames = 0;
	/** Worker time per processed frame (filter, serialize, send): the last one and the largest. */
	double LastWorkerSeconds = 0.0;
	double MaxWorkerSeconds = 0.0;
	/** Capture-to-SendSerialized latency (CaptureTimeSec to the call): the last one and the largest. */
	double LastCaptureToSendSeconds = 0.0;
	double MaxCaptureToSendSeconds = 0.0;
	/** True when frames are processed on the worker (o3d.Sender.AsyncPipeline at the last start). */
	bool bAsync = false;
};
