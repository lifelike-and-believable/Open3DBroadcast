// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Templates/UniquePtr.h"

struct FO3DSPoseFrame;

/**
 * Bounded pool of sampled pose frames (ADR 0008 item 4, SND-9; WP-A2a). A frame handed back with
 * Release() is emptied with FO3DSPoseFrame::Reset(), which keeps the capacity of its arrays and
 * strings, so a steady capture reuses the same allocations frame after frame.
 *
 * Bounded: at most Capacity frames ever exist at once, counting the frames handed out and the ones
 * waiting in the pool. Acquire() returns null when all of them are out; the caller then skips that
 * sample. The WP-A2c pipeline (FO3DSenderPipeline) sizes its pool as the largest queue depth plus
 * 2 (ADR 0008 item 4); frames are created on demand, so a capture at the default depth of 2 never
 * holds more than four, and the queue drops its oldest frame before the pool can run out.
 *
 * Acquire() and Release() may be called from different threads (the pipeline worker returns
 * frames); the lock is held only to move a pointer.
 */
class OPEN3DSENDER_API FO3DSPoseFramePool
{
public:
	/** Default capacity: the ADR 0008 default queue depth (2) plus 2. */
	static constexpr int32 DefaultCapacity = 4;

	/** Capacity is clamped to at least 1. */
	explicit FO3DSPoseFramePool(int32 InCapacity = DefaultCapacity);
	~FO3DSPoseFramePool();

	FO3DSPoseFramePool(const FO3DSPoseFramePool&) = delete;
	FO3DSPoseFramePool& operator=(const FO3DSPoseFramePool&) = delete;
	FO3DSPoseFramePool(FO3DSPoseFramePool&&) = delete;
	FO3DSPoseFramePool& operator=(FO3DSPoseFramePool&&) = delete;

	/** An empty frame, reused when one is free; null when Capacity frames are already out. */
	TUniquePtr<FO3DSPoseFrame> Acquire();

	/** Gives a frame from Acquire() back. It is reset here. Null is ignored. */
	void Release(TUniquePtr<FO3DSPoseFrame>&& Frame);

	int32 GetCapacity() const { return Capacity; }
	/** Frames that exist (handed out plus free). Never more than GetCapacity(). */
	int32 GetNumAllocated() const;
	/** Frames waiting in the pool. */
	int32 GetNumFree() const;

private:
	const int32 Capacity;
	mutable FCriticalSection Lock;
	TArray<TUniquePtr<FO3DSPoseFrame>> FreeFrames;
	int32 NumAllocated = 0;
};
