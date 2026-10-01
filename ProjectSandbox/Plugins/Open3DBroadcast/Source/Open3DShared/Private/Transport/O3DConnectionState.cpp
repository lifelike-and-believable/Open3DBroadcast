// Copyright Lifelike & Believable. All Rights Reserved.

#include "Transport/O3DConnectionState.h"

#include "Misc/ScopeLock.h"

void FO3DConnectionStateTracker::SetCallback(FO3DConnectionStateCallback InCallback)
{
	FScopeLock Lock(&Mutex);
	Callback = MoveTemp(InCallback);
}

void FO3DConnectionStateTracker::Begin(EO3DConnectionState InState, const FO3DTransportResult& Reason)
{
	FScopeLock Lock(&Mutex);
	bOpen.store(true, std::memory_order_release);
	ChangeLocked(InState, Reason);
}

bool FO3DConnectionStateTracker::Set(EO3DConnectionState InState, const FO3DTransportResult& Reason)
{
	FScopeLock Lock(&Mutex);
	if (!bOpen.load(std::memory_order_acquire))
	{
		return false;
	}
	return ChangeLocked(InState, Reason);
}

void FO3DConnectionStateTracker::End(EO3DConnectionState InState, const FO3DTransportResult& Reason)
{
	FScopeLock Lock(&Mutex);
	bOpen.store(false, std::memory_order_release);
	ChangeLocked(InState, Reason);
}

bool FO3DConnectionStateTracker::ChangeLocked(EO3DConnectionState InState, const FO3DTransportResult& Reason)
{
	const uint8 NewValue = static_cast<uint8>(InState);
	if (State.exchange(NewValue, std::memory_order_acq_rel) == NewValue)
	{
		return false;
	}
	if (Callback)
	{
		Callback(InState, Reason);
	}
	return true;
}
