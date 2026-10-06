// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "HAL/CriticalSection.h"

/**
 * Received PCM16 audio waiting to be played (RCV-20). The game thread pushes packets as they
 * arrive; the audio render thread pulls what the mixer asks for.
 * - Playback starts only once Target Latency of audio is queued (pre-roll), and starts again that
 *   way after the queue runs dry (underrun).
 * - When more than Target Latency + 60 ms is queued, the oldest audio is dropped down to the
 *   target, so a sender clock that runs fast, or a burst after a hitch, cannot grow the latency.
 * - Audio is kept and handed out in whole frames (one sample per channel), so channels never swap.
 */
class OPEN3DRECEIVER_API FO3DAudioJitterBuffer
{
public:
	/** Audio above the target that is kept before the oldest is dropped. */
	static constexpr float TrimMarginMs = 60.0f;

	/** Sets the format and target latency and empties the buffer. */
	void Configure(int32 InSampleRate, int32 InNumChannels, float InTargetLatencyMs);

	/** Game thread: appends interleaved PCM16 bytes (a trailing partial frame is ignored). */
	void Push(TConstArrayView<uint8> PCM16Bytes);

	/**
	 * Audio thread: writes up to NumSamples interleaved samples (whole frames) to Out and returns
	 * how many it wrote; 0 while pre-rolling. Writing fewer than asked starts a new pre-roll.
	 */
	int32 Pull(int16* Out, int32 NumSamples);

	/** Empties the buffer; the next audio pre-rolls again. */
	void Reset();

	int32 GetQueuedSamples() const;
	bool IsBuffering() const;
	/** Samples dropped to keep the latency bounded, since Configure. */
	int64 GetDroppedSamples() const;

private:
	int32 QueuedLocked() const { return Samples.Num() - ReadIndex; }

	mutable FCriticalSection Lock;
	TArray<int16> Samples;
	int32 ReadIndex = 0;
	int32 NumChannels = 1;
	int32 TargetSamples = 0;
	int32 MaxSamples = 0;
	bool bBuffering = true;
	int64 DroppedSamples = 0;
};
