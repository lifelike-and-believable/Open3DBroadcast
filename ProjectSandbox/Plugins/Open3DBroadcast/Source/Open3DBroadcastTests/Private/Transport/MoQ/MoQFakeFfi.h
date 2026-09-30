// Copyright (c) Open3DStream Contributors
//
// Fake moq-ffi (ADR 0006 option F2). It builds an FMoQFfiApi whose functions act on
// per-instance state, so tests need no relay, no network and no global mutation. The MoQ
// transport is reached through Testing/MoQTesting.h (CreateSenderForTest and friends).
//
// WP-T2 generalised it for the conformance suite: publishers remember their namespace, track and
// delivery mode, and moq_publish_data delivers the bytes to every live subscriber on the same
// namespace and track, on the publishing thread, as a relay would. That gives the MoQ profile an
// offline sender-to-receiver round trip.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_MOQ

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTLS.h"
#include "HAL/UnrealMemory.h"
#include "Misc/ScopeLock.h"
#include "MoQFfiApi.h"
#include "Testing/MoQTesting.h"

#include <cstring>

class FMoQFakeFfi : public TSharedFromThis<FMoQFakeFfi, ESPMode::ThreadSafe>
{
public:
	enum class EConnectBehavior : uint8
	{
		/** CONNECTING, CONNECTED callbacks, then MOQ_OK. */
		Succeed,
		/** CONNECTING, FAILED callbacks, then an error result with a message. */
		Fail,
		/** Error result with a message and a thread-local last error, but no callback at all. */
		FailWithoutCallback,
	};

	struct FClient
	{
		int32 Id = 0;
		bool bDestroyed = false;
		bool bConnected = false;
		int32 DisconnectCalls = 0;
		MoqConnectionCallback Callback = nullptr;
		void* UserData = nullptr;
		TArray<FString> Announced;
	};

	struct FPublisher
	{
		int32 ClientId = 0;
		FString Namespace;
		FString Track;
		MoqDeliveryMode DeliveryMode = MOQ_DELIVERY_STREAM;
		bool bDestroyed = false;
	};

	struct FSubscriber
	{
		int32 ClientId = 0;
		FString Namespace;
		FString Track;
		MoqDataCallback Callback = nullptr;
		void* UserData = nullptr;
		bool bDestroyed = false;
	};

	static TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Create()
	{
		return MakeShared<FMoQFakeFfi, ESPMode::ThreadSafe>();
	}

	// ── Scenario controls (set from the test's game thread) ──────────────────────────────
	EConnectBehavior ConnectBehavior = EConnectBehavior::Succeed;
	/** When true, LaunchBlocking stores the work instead of running it: a connect that never returns. */
	bool bHoldBlockingWork = false;
	bool bSubscribeFails = false;
	/** When true (default), moq_publish_data hands the bytes to matching live subscribers. */
	bool bRoutePublishedData = true;

	FMoQFfiApiRef MakeApi()
	{
		TWeakPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Weak = AsShared();
		TSharedRef<FMoQFfiApi, ESPMode::ThreadSafe> Api = MakeShared<FMoQFfiApi, ESPMode::ThreadSafe>();

		Api->Init = []() { return true; };
		Api->ClientCreate = [Weak]() -> MoqClient*
		{
			const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin();
			return Self.IsValid() ? Self->CreateClient() : nullptr;
		};
		Api->ClientDestroy = [Weak](MoqClient* Client)
		{
			if (const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin())
			{
				FScopeLock Lock(&Self->Mutex);
				if (FClient* Found = Self->FindClient(Client))
				{
					Found->bDestroyed = true;
				}
			}
		};
		Api->Connect = [Weak](MoqClient* Client, const char* /*Url*/, MoqConnectionCallback Callback, void* UserData) -> MoqResult
		{
			const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin();
			return Self.IsValid() ? Self->Connect(Client, Callback, UserData) : MoqResult{MOQ_ERROR_INTERNAL, nullptr};
		};
		Api->Disconnect = [Weak](MoqClient* Client) -> MoqResult
		{
			const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin();
			if (!Self.IsValid())
			{
				return MoqResult{MOQ_OK, nullptr};
			}
			MoqConnectionCallback Callback = nullptr;
			void* UserData = nullptr;
			{
				FScopeLock Lock(&Self->Mutex);
				if (FClient* Found = Self->FindClient(Client))
				{
					++Found->DisconnectCalls;
					Found->bConnected = false;
					Callback = Found->Callback;
					UserData = Found->UserData;
				}
			}
			// Like moq-ffi, report DISCONNECTED from inside the call.
			if (Callback)
			{
				Callback(UserData, MOQ_STATE_DISCONNECTED);
			}
			return MoqResult{MOQ_OK, nullptr};
		};
		Api->AnnounceNamespace = [Weak](MoqClient* Client, const char* Namespace) -> MoqResult
		{
			if (const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin())
			{
				FScopeLock Lock(&Self->Mutex);
				if (FClient* Found = Self->FindClient(Client))
				{
					Found->Announced.Add(UTF8_TO_TCHAR(Namespace));
					Self->AnnounceLog.Add(FString::Printf(TEXT("%d:%s"), Found->Id, UTF8_TO_TCHAR(Namespace)));
				}
			}
			return MoqResult{MOQ_OK, nullptr};
		};
		Api->CreatePublisherEx = [Weak](MoqClient* Client, const char* Namespace, const char* Track, MoqDeliveryMode DeliveryMode) -> MoqPublisher*
		{
			const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin();
			if (!Self.IsValid())
			{
				return nullptr;
			}
			FScopeLock Lock(&Self->Mutex);
			++Self->PublishersCreated;
			FPublisher& Publisher = Self->Publishers.AddDefaulted_GetRef();
			const FClient* Found = Self->FindClient(Client);
			Publisher.ClientId = Found ? Found->Id : 0;
			Publisher.Namespace = Namespace ? UTF8_TO_TCHAR(Namespace) : TEXT("");
			Publisher.Track = Track ? UTF8_TO_TCHAR(Track) : TEXT("");
			Publisher.DeliveryMode = DeliveryMode;
			// 0x1000 + index + 1: unique, non-null and never dereferenced.
			return reinterpret_cast<MoqPublisher*>(static_cast<UPTRINT>(0x1000 + Self->Publishers.Num()));
		};
		Api->PublisherDestroy = [Weak](MoqPublisher* Publisher)
		{
			if (const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin())
			{
				FScopeLock Lock(&Self->Mutex);
				++Self->PublishersDestroyed;
				const int32 Index = static_cast<int32>(reinterpret_cast<UPTRINT>(Publisher)) - 0x1001;
				if (Self->Publishers.IsValidIndex(Index))
				{
					Self->Publishers[Index].bDestroyed = true;
				}
			}
		};
		Api->PublishData = [Weak](MoqPublisher* Publisher, const uint8_t* Data, size_t NumBytes, MoqDeliveryMode) -> MoqResult
		{
			const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin();
			if (!Self.IsValid())
			{
				return MoqResult{MOQ_OK, nullptr};
			}

			TArray<TPair<MoqDataCallback, void*>> Targets;
			{
				FScopeLock Lock(&Self->Mutex);
				++Self->PublishCalls;
				Self->PublishedBytes += static_cast<int64>(NumBytes);
				const int32 Index = static_cast<int32>(reinterpret_cast<UPTRINT>(Publisher)) - 0x1001;
				if (Self->bRoutePublishedData && Self->Publishers.IsValidIndex(Index))
				{
					const FPublisher& Source = Self->Publishers[Index];
					for (const FSubscriber& Sub : Self->Subscribers)
					{
						if (!Sub.bDestroyed && Sub.Callback && Sub.Namespace == Source.Namespace && Sub.Track == Source.Track)
						{
							Targets.Emplace(Sub.Callback, Sub.UserData);
						}
					}
				}
			}
			// Outside the lock, on the publishing thread, like a relay delivering to moq-ffi's
			// own callback threads.
			for (const TPair<MoqDataCallback, void*>& Target : Targets)
			{
				Target.Key(Target.Value, Data, NumBytes);
			}
			return MoqResult{MOQ_OK, nullptr};
		};
		Api->Subscribe = [Weak](MoqClient* Client, const char* Namespace, const char* Track, MoqDataCallback Callback, void* UserData) -> MoqSubscriber*
		{
			const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin();
			if (!Self.IsValid())
			{
				return nullptr;
			}
			FScopeLock Lock(&Self->Mutex);
			++Self->SubscribeCalls;
			if (Self->bSubscribeFails)
			{
				Self->SetLastErrorLocked(TEXT("fake: track not announced"));
				return nullptr;
			}
			FSubscriber& Sub = Self->Subscribers.AddDefaulted_GetRef();
			const FClient* Found = Self->FindClient(Client);
			Sub.ClientId = Found ? Found->Id : 0;
			Sub.Namespace = UTF8_TO_TCHAR(Namespace);
			Sub.Track = UTF8_TO_TCHAR(Track);
			Sub.Callback = Callback;
			Sub.UserData = UserData;
			// Index + 1 so the handle is never null.
			return reinterpret_cast<MoqSubscriber*>(static_cast<UPTRINT>(Self->Subscribers.Num()));
		};
		Api->SubscriberDestroy = [Weak](MoqSubscriber* Subscriber)
		{
			if (const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin())
			{
				FScopeLock Lock(&Self->Mutex);
				const int32 Index = static_cast<int32>(reinterpret_cast<UPTRINT>(Subscriber)) - 1;
				if (Self->Subscribers.IsValidIndex(Index))
				{
					Self->Subscribers[Index].bDestroyed = true;
				}
			}
		};
		Api->FreeStr = [Weak](const char* Str)
		{
			if (const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin())
			{
				{
					FScopeLock Lock(&Self->Mutex);
					++Self->StringsFreed;
				}
				FMemory::Free(const_cast<char*>(Str));
			}
		};
		Api->Version = []() -> const char* { return "moq_ffi fake (IETF Draft 07)"; };
		Api->LastError = [Weak]() -> const char*
		{
			const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin();
			if (!Self.IsValid())
			{
				return nullptr;
			}
			FScopeLock Lock(&Self->Mutex);
			++Self->LastErrorCalls;
			Self->LastErrorCallerThreads.Add(FPlatformTLS::GetCurrentThreadId());
			// Thread-local like moq-ffi: each thread sees only its own last error.
			const TArray<ANSICHAR>* Found = Self->LastErrorByThread.Find(FPlatformTLS::GetCurrentThreadId());
			return (Found && Found->Num() > 0) ? Found->GetData() : nullptr;
		};
		Api->LaunchBlocking = [Weak](TUniqueFunction<void()>&& Work)
		{
			const TSharedPtr<FMoQFakeFfi, ESPMode::ThreadSafe> Self = Weak.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			{
				FScopeLock Lock(&Self->Mutex);
				++Self->BlockingLaunches;
				if (Self->bHoldBlockingWork)
				{
					Self->HeldWork.Add(MoveTemp(Work));
					return;
				}
			}
			// Inline: the "blocking" call finishes before Connect() returns.
			Work();
		};

		return Api;
	}

	/** Runs every held blocking call now (a connect that finally returns). */
	void RunHeldWork()
	{
		TArray<TUniqueFunction<void()>> Work;
		{
			FScopeLock Lock(&Mutex);
			Work = MoveTemp(HeldWork);
			HeldWork.Reset();
		}
		for (TUniqueFunction<void()>& Item : Work)
		{
			Item();
		}
	}

	/** Drops held blocking calls without running them. */
	void DiscardHeldWork()
	{
		TArray<TUniqueFunction<void()>> Work;
		{
			FScopeLock Lock(&Mutex);
			Work = MoveTemp(HeldWork);
			HeldWork.Reset();
		}
	}

	/** Fires the connection callback that client ClientId registered, as a moq-ffi thread would. */
	bool FireConnectionState(int32 ClientId, MoqConnectionState State)
	{
		MoqConnectionCallback Callback = nullptr;
		void* UserData = nullptr;
		{
			FScopeLock Lock(&Mutex);
			for (const TUniquePtr<FClient>& Client : Clients)
			{
				if (Client->Id == ClientId)
				{
					Callback = Client->Callback;
					UserData = Client->UserData;
				}
			}
		}
		if (!Callback)
		{
			return false;
		}
		Callback(UserData, State);
		return true;
	}

	/** Delivers bytes to the most recent live subscriber on Namespace. */
	bool DeliverData(const FString& Namespace, const TArray<uint8>& Bytes)
	{
		MoqDataCallback Callback = nullptr;
		void* UserData = nullptr;
		{
			FScopeLock Lock(&Mutex);
			for (int32 Index = Subscribers.Num() - 1; Index >= 0; --Index)
			{
				if (!Subscribers[Index].bDestroyed && Subscribers[Index].Namespace == Namespace)
				{
					Callback = Subscribers[Index].Callback;
					UserData = Subscribers[Index].UserData;
					break;
				}
			}
		}
		if (!Callback)
		{
			return false;
		}
		Callback(UserData, Bytes.GetData(), static_cast<size_t>(Bytes.Num()));
		return true;
	}

	// ── Observations ─────────────────────────────────────────────────────────────────────
	int32 GetClientsCreated() const { FScopeLock Lock(&Mutex); return Clients.Num(); }
	bool IsClientDestroyed(int32 ClientId) const
	{
		FScopeLock Lock(&Mutex);
		for (const TUniquePtr<FClient>& Client : Clients)
		{
			if (Client->Id == ClientId)
			{
				return Client->bDestroyed;
			}
		}
		return false;
	}
	int32 GetDisconnectCalls(int32 ClientId) const
	{
		FScopeLock Lock(&Mutex);
		for (const TUniquePtr<FClient>& Client : Clients)
		{
			if (Client->Id == ClientId)
			{
				return Client->DisconnectCalls;
			}
		}
		return 0;
	}
	TArray<FString> GetAnnounced(int32 ClientId) const
	{
		FScopeLock Lock(&Mutex);
		for (const TUniquePtr<FClient>& Client : Clients)
		{
			if (Client->Id == ClientId)
			{
				return Client->Announced;
			}
		}
		return TArray<FString>();
	}
	TArray<FString> GetAnnounceLog() const { FScopeLock Lock(&Mutex); return AnnounceLog; }
	int32 GetBlockingLaunches() const { FScopeLock Lock(&Mutex); return BlockingLaunches; }
	int32 GetHeldWorkCount() const { FScopeLock Lock(&Mutex); return HeldWork.Num(); }
	int32 GetSubscribeCalls() const { FScopeLock Lock(&Mutex); return SubscribeCalls; }
	/** "Namespace|Track" of every subscriber that has not been destroyed. */
	TArray<FString> GetLiveSubscriptions() const
	{
		FScopeLock Lock(&Mutex);
		TArray<FString> Out;
		for (const FSubscriber& Sub : Subscribers)
		{
			if (!Sub.bDestroyed)
			{
				Out.Add(Sub.Namespace + TEXT("|") + Sub.Track);
			}
		}
		return Out;
	}
	int32 GetPublishersCreated() const { FScopeLock Lock(&Mutex); return PublishersCreated; }
	int32 GetPublishersDestroyed() const { FScopeLock Lock(&Mutex); return PublishersDestroyed; }
	int32 GetPublishCalls() const { FScopeLock Lock(&Mutex); return PublishCalls; }
	/** Every publisher created so far, including destroyed ones, in creation order. */
	TArray<FPublisher> GetPublishers() const { FScopeLock Lock(&Mutex); return Publishers; }
	int32 GetStringsAllocated() const { FScopeLock Lock(&Mutex); return StringsAllocated; }
	int32 GetStringsFreed() const { FScopeLock Lock(&Mutex); return StringsFreed; }
	int32 GetLastErrorCalls() const { FScopeLock Lock(&Mutex); return LastErrorCalls; }
	TArray<uint32> GetLastErrorCallerThreads() const { FScopeLock Lock(&Mutex); return LastErrorCallerThreads; }

	/** Sets the thread-local "last error" as moq-ffi does on a failing call. */
	void SetLastErrorForThisThread(const FString& Message)
	{
		FScopeLock Lock(&Mutex);
		SetLastErrorLocked(Message);
	}

private:
	MoqClient* CreateClient()
	{
		FScopeLock Lock(&Mutex);
		TUniquePtr<FClient>& Client = Clients.Add_GetRef(MakeUnique<FClient>());
		Client->Id = Clients.Num();
		// The fake client's address doubles as the opaque MoqClient pointer.
		return reinterpret_cast<MoqClient*>(Client.Get());
	}

	FClient* FindClient(MoqClient* Handle)
	{
		for (const TUniquePtr<FClient>& Client : Clients)
		{
			if (reinterpret_cast<MoqClient*>(Client.Get()) == Handle)
			{
				return Client.Get();
			}
		}
		return nullptr;
	}

	/** A moq_free_str-owned copy, counted so tests can check every message is freed once. */
	const char* AllocMessageLocked(const char* Text)
	{
		const SIZE_T Len = FCStringAnsi::Strlen(Text);
		char* Copy = static_cast<char*>(FMemory::Malloc(Len + 1));
		FMemory::Memcpy(Copy, Text, Len + 1);
		++StringsAllocated;
		return Copy;
	}

	void SetLastErrorLocked(const FString& Message)
	{
		const FTCHARToUTF8 Utf8(*Message);
		TArray<ANSICHAR>& Buffer = LastErrorByThread.FindOrAdd(FPlatformTLS::GetCurrentThreadId());
		Buffer.Reset();
		Buffer.Append(Utf8.Get(), Utf8.Length());
		Buffer.Add('\0');
	}

	MoqResult Connect(MoqClient* Handle, MoqConnectionCallback Callback, void* UserData)
	{
		EConnectBehavior Behavior;
		{
			FScopeLock Lock(&Mutex);
			Behavior = ConnectBehavior;
			if (FClient* Found = FindClient(Handle))
			{
				Found->Callback = Callback;
				Found->UserData = UserData;
			}
		}

		switch (Behavior)
		{
		case EConnectBehavior::Succeed:
			if (Callback)
			{
				Callback(UserData, MOQ_STATE_CONNECTING);
			}
			{
				FScopeLock Lock(&Mutex);
				if (FClient* Found = FindClient(Handle))
				{
					Found->bConnected = true;
				}
			}
			if (Callback)
			{
				Callback(UserData, MOQ_STATE_CONNECTED);
			}
			return MoqResult{MOQ_OK, nullptr};

		case EConnectBehavior::Fail:
		{
			if (Callback)
			{
				Callback(UserData, MOQ_STATE_CONNECTING);
				Callback(UserData, MOQ_STATE_FAILED);
			}
			FScopeLock Lock(&Mutex);
			SetLastErrorLocked(TEXT("fake: handshake refused"));
			return MoqResult{MOQ_ERROR_CONNECTION_FAILED, AllocMessageLocked("fake connection failed")};
		}

		case EConnectBehavior::FailWithoutCallback:
		default:
		{
			FScopeLock Lock(&Mutex);
			SetLastErrorLocked(TEXT("fake: invalid url scheme"));
			return MoqResult{MOQ_ERROR_INVALID_ARGUMENT, AllocMessageLocked("fake invalid argument")};
		}
		}
	}

	mutable FCriticalSection Mutex;
	TArray<TUniquePtr<FClient>> Clients;
	TArray<FSubscriber> Subscribers;
	TArray<FPublisher> Publishers;
	TArray<FString> AnnounceLog;
	TArray<TUniqueFunction<void()>> HeldWork;
	/** Per-thread last error; a returned pointer stays valid until that thread's next error. */
	TMap<uint32, TArray<ANSICHAR>> LastErrorByThread;
	TArray<uint32> LastErrorCallerThreads;
	int32 BlockingLaunches = 0;
	int32 SubscribeCalls = 0;
	int32 PublishersCreated = 0;
	int32 PublishersDestroyed = 0;
	int32 PublishCalls = 0;
	int64 PublishedBytes = 0;
	int32 StringsAllocated = 0;
	int32 StringsFreed = 0;
	int32 LastErrorCalls = 0;
};

namespace MoQFakeTest
{
	/** Delivers queued FFI callbacks on the game thread, as the dispatcher's ticker would. */
	inline int32 Pump()
	{
		return MoQTesting::PumpDispatcher();
	}

	/** A manual clock: tests advance it instead of sleeping. */
	struct FManualClock
	{
		TSharedRef<double, ESPMode::ThreadSafe> Now = MakeShared<double, ESPMode::ThreadSafe>(1000.0);

		TFunction<double()> AsFunction() const
		{
			TSharedRef<double, ESPMode::ThreadSafe> Shared = Now;
			return [Shared]() { return *Shared; };
		}

		void Advance(double Seconds) { *Now += Seconds; }
		double Get() const { return *Now; }
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_MOQ
