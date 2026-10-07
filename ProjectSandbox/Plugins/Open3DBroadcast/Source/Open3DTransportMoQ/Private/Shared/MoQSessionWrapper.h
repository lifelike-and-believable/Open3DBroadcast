// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Containers/Array.h"
#include "Containers/Set.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/ScopeLock.h"
#include "Templates/Atomic.h"
#include "Templates/Function.h"
#include "MoQFfiApi.h"
#include "Shared/MoQHandles.h"
#include "Shared/MoQTypes.h"

#include <atomic>

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
 * Connection state shared with FFI threads (WP-S5, TRF-12). Holds plain data, atomics and a
 * weak wrapper reference only, so any thread can drop the last reference safely.
 */
struct FMoQConnectionContext
{
    TAtomic<MoqConnectionState> CurrentState{MOQ_STATE_DISCONNECTED};
    /** Id of the connect attempt whose callbacks are current; 0 when none is (WP-S8). */
    std::atomic<uint64> ActiveAttemptId{0};
    /** Serialises "is this attempt current?" with the state write it guards. */
    FCriticalSection StateMutex;
    /** Error text of the last failed connect attempt, copied on the thread that failed. */
    FString LastConnectError;
    /** Set on the game thread in Initialize() before any connect; not changed afterwards. */
    TWeakPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Wrapper;
};

/**
 * One moq_connect call (WP-S8, TRF-8/TRF-11). The FFI receives an opaque token that resolves
 * to this object. Once the wrapper abandons the attempt (timeout, Disconnect, a newer Connect)
 * the token is unregistered and the attempt id is no longer current, so its late callbacks do
 * nothing.
 */
struct FMoQConnectAttempt
{
    uint64 AttemptId = 0;
    TWeakPtr<FMoQConnectionContext, ESPMode::ThreadSafe> Connection;
    /** Set once CONNECTED or FAILED has been reported for this attempt. */
    std::atomic<bool> bTerminalReported{false};
};

class FMoQSessionWrapper : public TSharedFromThis<FMoQSessionWrapper, ESPMode::ThreadSafe>
{
public:
    /** Uses the production moq-ffi table. */
    FMoQSessionWrapper();
    /** Uses the given table (tests pass a fake, ADR 0006 F2). */
    explicit FMoQSessionWrapper(FMoQFfiApiRef InApi);
    ~FMoQSessionWrapper();

    FMoQSessionWrapper(const FMoQSessionWrapper&) = delete;
    FMoQSessionWrapper& operator=(const FMoQSessionWrapper&) = delete;

    FMoQResult Initialize(const FString& InRelayUrl);

    /**
     * Game thread. Starts a new connect attempt on a fresh MoqClient and returns at once; the
     * outcome arrives through OnConnectionStateChanged. Any earlier attempt is abandoned first.
     * Every attempt ends in CONNECTED or FAILED unless it is abandoned (TRF-11).
     */
    FMoQResult Connect();

    /**
     * Game thread. Gives up on the in-flight connect attempt (for example after a timeout):
     * its callbacks are ignored from now on and its client is closed and released when its
     * blocking moq_connect returns. The session reads as disconnected afterwards.
     */
    void AbandonConnect();

    void Disconnect();

    bool IsConnected() const;

    /** Last connect error recorded for this session (empty if none). Any thread. */
    FString GetLastConnectError() const;

    const FMoQFfiApi& GetApi() const { return *Api; }

    using FMoQConnectionStateDelegate = TMulticastDelegate<void(MoqConnectionState)>;
    FMoQConnectionStateDelegate& OnConnectionStateChanged() { return ConnectionStateDelegate; }

    FMoQResult AnnounceNamespace(const FString& Namespace);

    FMoQResult CreatePublisher(const FMoQPublisherConfig& Config, TSharedPtr<FMoQPublisherHandle>& OutPublisher);

    FMoQResult Subscribe(const FMoQSubscriptionConfig& Config, TSharedPtr<FMoQSubscriberHandle>& OutSubscriber);
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
    /**
     * Records State for Attempt if it is still current and queues the game-thread handling.
     * Returns false if the attempt is stale or no wrapper is bound.
     */
    static bool ReportAttemptState(FMoQConnectAttempt& Attempt, MoqConnectionState State);
    /** Body of the background connect task. */
    static void RunConnectAttempt(const FMoQFfiApi& InApi, const FMoQClientRef& InClient, FMoQConnectAttempt& Attempt, void* Token, const FString& Url);
    /** Game thread: applies a state change of attempt AttemptId (0 = no attempt) and broadcasts it. */
    void HandleConnectionStateOnGameThread(uint64 AttemptId, MoqConnectionState State);
    /** Test entry point: records State for the current attempt and dispatches it. */
    void HandleConnectionStateInternal(MoqConnectionState State);

    static void HandleSubscriberDataThunk(void* UserData, const uint8_t* Data, size_t DataLen);
#if WITH_DEV_AUTOMATION_TESTS
    static void InvokeSubscriberThunkForTest(const TFunction<void(const TArray64<uint8>&)>& Callback, const TArray64<uint8>& Payload);
#endif

    bool ValidateInitialized(FString& OutReason) const;
    FMoQClientRef GetClient() const;
    /** Makes the current attempt stale and unregisters its token. Game thread. */
    void InvalidateAttempt();
    /** Takes the current client out of the session; disconnects it first if bDisconnect. */
    void ReleaseClient(bool bDisconnect);
    FMoQResult AnnounceOnClient(const FMoQClientRef& ClientRef, const FString& Normalized);
    void RemoveSubscriberBinding(MoqSubscriber* Subscriber);
    void ClearSubscriberBindings();
    void ClearAnnouncedNamespaces();

    FMoQFfiApiRef Api;
    FString RelayUrl;
    FThreadSafeBool bInitialized = false;

    /** Current client; swapped on the game thread, read (snapshot) from any thread. */
    FMoQClientRef Client;
    mutable FCriticalSection ClientMutex;

    TSharedRef<FMoQConnectionContext, ESPMode::ThreadSafe> ConnectionContext;
    /** Game thread only. */
    TSharedPtr<FMoQConnectAttempt, ESPMode::ThreadSafe> ActiveAttempt;
    void* ActiveAttemptToken = nullptr;
    uint64 NextAttemptId = 0;

    /** Namespaces announced on AnnouncedClient; a different client starts from an empty set (TRF-8). */
    TSet<FString> AnnouncedNamespaces;
    const FMoQSessionHandle* AnnouncedClient = nullptr;
    mutable FCriticalSection NamespaceMutex;

    TMap<MoqSubscriber*, FSubscriberEntry> SubscriberBindings;
    mutable FCriticalSection SubscriberMutex;

    FMoQConnectionStateDelegate ConnectionStateDelegate;
    TWeakPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> SelfWeak;

    // Unconditional: a friend declaration must not depend on WITH_DEV_AUTOMATION_TESTS. Defined in
    // Private/Testing/MoQTesting.cpp; tests reach it through Public/Testing/MoQTesting.h (WP-T2).
    friend class FMoQSessionWrapperTestHelper;
};
