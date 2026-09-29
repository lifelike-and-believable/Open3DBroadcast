#pragma once

#include "CoreMinimal.h"

#include "Containers/Array.h"
#include "Containers/Set.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Atomic.h"
#include "Templates/Function.h"
#include "Shared/MoQHandles.h"
#include "Shared/MoQTypes.h"

struct FMoQPublisherConfig
{
    FString Namespace;
    FString TrackName;
    MoqDeliveryMode DeliveryMode = MOQ_DELIVERY_STREAM;
};

struct FMoQSubscriptionConfig
{
    FString Namespace;
    FString TrackName;
    TFunction<void(const TArray64<uint8>&)> OnData;
};

class FMoQSessionWrapper;

/**
 * State reachable from the moq_connect callback (WP-S5, TRF-12). The FFI receives an opaque
 * token that resolves to this context, never the wrapper's address. It holds atomics and a
 * weak wrapper reference only, so a Tokio thread can drop the last reference safely; the
 * wrapper itself is only ever pinned on the game thread.
 */
struct FMoQConnectionContext
{
    TAtomic<MoqConnectionState> CurrentState{MOQ_STATE_DISCONNECTED};
    TAtomic<bool> bExpectingDisconnect{false};
    /** Set on the game thread in Initialize() before any connect; not changed afterwards. */
    TWeakPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Wrapper;
};

class FMoQSessionWrapper : public TSharedFromThis<FMoQSessionWrapper, ESPMode::ThreadSafe>
{
public:
    FMoQSessionWrapper();
    ~FMoQSessionWrapper();

    FMoQResult Initialize(const FString& InRelayUrl);

    FMoQResult Connect();
    void Disconnect();

    bool IsConnected() const;

    using FMoQConnectionStateDelegate = TMulticastDelegate<void(MoqConnectionState)>;
    FMoQConnectionStateDelegate& OnConnectionStateChanged() { return ConnectionStateDelegate; }

    FMoQResult AnnounceNamespace(const FString& Namespace);

    FMoQResult CreatePublisher(const FMoQPublisherConfig& Config, TSharedPtr<FMoQPublisherHandle>& OutPublisher);

    FMoQResult Subscribe(const FMoQSubscriptionConfig& Config, TSharedPtr<FMoQSubscriberHandle>& OutSubscriber);
    using FSubscribeAsyncCallback = TFunction<void(FMoQResult, TSharedPtr<FMoQSubscriberHandle>)>;
    FMoQResult SubscribeAsync(const FMoQSubscriptionConfig& Config, FSubscribeAsyncCallback&& Completion);
    void Unsubscribe(const TSharedPtr<FMoQSubscriberHandle>& SubscriberHandle);

private:
    /** Reached from moq_subscribe callbacks through an opaque token (TRF-12). */
    struct FSubscriberBinding
    {
        TFunction<void(const TArray64<uint8>&)> DataHandler;
    };

    struct FSubscriberEntry
    {
        TSharedPtr<FSubscriberBinding, ESPMode::ThreadSafe> Binding;
        void* Token = nullptr;
    };

    static void HandleConnectionStateThunk(void* UserData, MoqConnectionState State);
    /** Records the state and queues the delegate broadcast on the game thread. Returns false if no wrapper is bound. */
    static bool RecordConnectionState(FMoQConnectionContext& Context, MoqConnectionState State);
    void HandleConnectionStateInternal(MoqConnectionState State);

    static void HandleSubscriberDataThunk(void* UserData, const uint8_t* Data, size_t DataLen);
#if WITH_DEV_AUTOMATION_TESTS
    static void InvokeSubscriberThunkForTest(const TFunction<void(const TArray64<uint8>&)>& Callback, const TArray64<uint8>& Payload);
#endif

    bool ValidateInitialized(FString& OutReason) const;
    FMoQResult EnsureClientAvailable();
    void RemoveSubscriberBinding(MoqSubscriber* Subscriber);

    FMoQSessionHandle SessionHandle;
    FString RelayUrl;
    FThreadSafeBool bInitialized = false;

    TSharedRef<FMoQConnectionContext, ESPMode::ThreadSafe> ConnectionContext;
    /** Opaque moq_connect user data; resolves to ConnectionContext until the destructor. */
    void* ConnectionToken = nullptr;

    TSet<FString> AnnouncedNamespaces;
    mutable FCriticalSection NamespaceMutex;

    TMap<MoqSubscriber*, FSubscriberEntry> SubscriberBindings;
    mutable FCriticalSection SubscriberMutex;

    FMoQConnectionStateDelegate ConnectionStateDelegate;
    TWeakPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> SelfWeak;
#if WITH_DEV_AUTOMATION_TESTS
    friend class FMoQSessionWrapperTestHelper;
#endif
};

#if WITH_DEV_AUTOMATION_TESTS
class FMoQSessionWrapperTestHelper
{
public:
    static void InvokeConnectionState(FMoQSessionWrapper& Wrapper, MoqConnectionState State)
    {
        Wrapper.HandleConnectionStateInternal(State);
    }

    static void InvokeSubscriberCallback(const TFunction<void(const TArray64<uint8>&)>& Callback, const TArray64<uint8>& Payload)
    {
        FMoQSessionWrapper::InvokeSubscriberThunkForTest(Callback, Payload);
    }

    /** Fires the raw FFI subscriber thunk with an arbitrary user_data value (a stale or unknown token). */
    static void InvokeSubscriberThunkWithToken(void* Token, const TArray64<uint8>& Payload)
    {
        const uint8* DataPtr = Payload.Num() > 0 ? Payload.GetData() : nullptr;
        FMoQSessionWrapper::HandleSubscriberDataThunk(Token, DataPtr, static_cast<size_t>(Payload.Num()));
    }

    /** Fires the raw FFI connection thunk with an arbitrary user_data value. */
    static void InvokeConnectionThunkWithToken(void* Token, MoqConnectionState State)
    {
        FMoQSessionWrapper::HandleConnectionStateThunk(Token, State);
    }

    static void* GetConnectionToken(const FMoQSessionWrapper& Wrapper)
    {
        return Wrapper.ConnectionToken;
    }
};
#endif
