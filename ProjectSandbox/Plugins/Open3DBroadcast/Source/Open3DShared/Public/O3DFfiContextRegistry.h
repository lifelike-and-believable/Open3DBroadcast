// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeRWLock.h"
#include "Templates/SharedPointer.h"

/**
 * Maps opaque FFI `user_data` tokens to weakly held callback contexts (ADR 0007 addendum,
 * WP-S5: TRF-12).
 *
 * A C callback receives a token, never an object address. The thunk calls Resolve(), which
 * pins the context under a read lock; a token that was unregistered (or never existed)
 * resolves to null, so a callback that arrives late cannot touch freed memory. Tokens are
 * never reused.
 *
 * Contexts must hold only atomics, immutable data and weak references: the FFI thread may
 * end up holding the last strong reference, so a context destructor must not call into the
 * FFI, close sockets or touch UObjects.
 *
 * Register/Unregister: any thread (in practice the game thread). Resolve: any thread.
 * Use one instance per context type, as a function-local static in the owning module.
 */
template <typename T>
class TO3DFfiContextRegistry
{
public:
	using FContextPtr = TSharedPtr<T, ESPMode::ThreadSafe>;

	void* Register(const TSharedRef<T, ESPMode::ThreadSafe>& Context)
	{
		FWriteScopeLock Lock(RWLock);
		const UPTRINT Token = ++NextToken;
		Contexts.Add(Token, Context);
		return reinterpret_cast<void*>(Token);
	}

	void Unregister(void* Token)
	{
		if (!Token)
		{
			return;
		}
		FWriteScopeLock Lock(RWLock);
		Contexts.Remove(reinterpret_cast<UPTRINT>(Token));
	}

	FContextPtr Resolve(void* Token) const
	{
		if (!Token)
		{
			return nullptr;
		}
		FReadScopeLock Lock(RWLock);
		if (const TWeakPtr<T, ESPMode::ThreadSafe>* Found = Contexts.Find(reinterpret_cast<UPTRINT>(Token)))
		{
			return Found->Pin();
		}
		return nullptr;
	}

	int32 Num() const
	{
		FReadScopeLock Lock(RWLock);
		return Contexts.Num();
	}

private:
	mutable FRWLock RWLock;
	TMap<UPTRINT, TWeakPtr<T, ESPMode::ThreadSafe>> Contexts;
	UPTRINT NextToken = 0;
};
