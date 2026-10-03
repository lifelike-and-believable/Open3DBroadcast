// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DSPoseFramePool.h"

#include "O3DSenderComponent.h"
#include "Misc/ScopeLock.h"

FO3DSPoseFramePool::FO3DSPoseFramePool(int32 InCapacity)
	: Capacity(FMath::Max(1, InCapacity))
{
	FreeFrames.Reserve(Capacity);
}

FO3DSPoseFramePool::~FO3DSPoseFramePool() = default;

TUniquePtr<FO3DSPoseFrame> FO3DSPoseFramePool::Acquire()
{
	FScopeLock PoolGuard(&Lock);
	if (FreeFrames.Num() > 0)
	{
		return FreeFrames.Pop(EAllowShrinking::No);
	}
	if (NumAllocated >= Capacity)
	{
		return nullptr;
	}
	++NumAllocated;
	return MakeUnique<FO3DSPoseFrame>();
}

void FO3DSPoseFramePool::Release(TUniquePtr<FO3DSPoseFrame>&& Frame)
{
	if (!Frame.IsValid())
	{
		return;
	}

	// Outside the lock: Reset() only empties the frame's own containers.
	Frame->Reset();

	FScopeLock PoolGuard(&Lock);
	if (FreeFrames.Num() < NumAllocated)
	{
		FreeFrames.Add(MoveTemp(Frame));
		return;
	}
	// A frame this pool did not hand out: never kept, so the bound holds. Deleted when Frame goes
	// out of scope.
}

int32 FO3DSPoseFramePool::GetNumAllocated() const
{
	FScopeLock PoolGuard(&Lock);
	return NumAllocated;
}

int32 FO3DSPoseFramePool::GetNumFree() const
{
	FScopeLock PoolGuard(&Lock);
	return FreeFrames.Num();
}
