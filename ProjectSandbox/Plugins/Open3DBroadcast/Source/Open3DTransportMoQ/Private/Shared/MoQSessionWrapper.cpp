// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Shared/MoQSessionWrapper.h"
#include "O3DRedact.h"

#include "Misc/ScopeLock.h"
#include "Shared/MoQAsyncDispatcher.h"
#include "Templates/SharedPointer.h"
#include "Containers/StringConv.h"
#include "HAL/UnrealMemory.h"
#include "O3DFfiContextRegistry.h"

namespace
{
    FString NormalizeValue(const FString& Value)
    {
        FString Copy = Value;
        Copy.TrimStartAndEndInline();
        return Copy;
    }

    // WP-S5 (TRF-12): moq-ffi does not document that callbacks stop after moq_disconnect or
    // *_destroy returns, so user_data is an opaque token resolved here, never an address.
    // WP-S8: one token per connect attempt, so an abandoned attempt's callbacks resolve to nothing.
    TO3DFfiContextRegistry<FMoQConnectAttempt>& GetAttemptRegistry()
    {
        static TO3DFfiContextRegistry<FMoQConnectAttempt> Registry;
        return Registry;
    }

    bool IsTerminalConnectState(MoqConnectionState State)
    {
        return State == MOQ_STATE_CONNECTED || State == MOQ_STATE_FAILED;
    }
}

// Declared at namespace scope because FSubscriberBinding is private to the wrapper; only
// this translation unit names the registry type.
template <typename T>
static TO3DFfiContextRegistry<T>& GetSubscriberRegistryFor()
{
    static TO3DFfiContextRegistry<T> Registry;
    return Registry;
}

FMoQSessionWrapper::FMoQSessionWrapper()
    : FMoQSessionWrapper(FMoQFfiApi::GetProduction())
{
}

FMoQSessionWrapper::FMoQSessionWrapper(FMoQFfiApiRef InApi)
    : Api(MoveTemp(InApi))
    , ConnectionContext(MakeShared<FMoQConnectionContext, ESPMode::ThreadSafe>())
{
}

FMoQSessionWrapper::~FMoQSessionWrapper()
{
    // Disconnect() also unregisters the current attempt token, so late callbacks resolve to nothing.
    Disconnect();
}

FMoQResult FMoQSessionWrapper::Initialize(const FString& InRelayUrl)
{
    FString Normalized = NormalizeValue(InRelayUrl);
    if (Normalized.IsEmpty())
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, TEXT("Relay URL cannot be empty"));
    }

    RelayUrl = MoveTemp(Normalized);
    ClearAnnouncedNamespaces();
    bInitialized = true;

    if (!SelfWeak.IsValid())
    {
        SelfWeak = AsShared();
    }
    ConnectionContext->Wrapper = SelfWeak;

    // No FFI call here: the client is created per connect attempt (WP-S8).
    return FMoQResult::Ok();
}

bool FMoQSessionWrapper::ValidateInitialized(FString& OutReason) const
{
    if (!bInitialized)
    {
        OutReason = TEXT("Session wrapper not initialized");
        return false;
    }

    if (RelayUrl.IsEmpty())
    {
        OutReason = TEXT("Relay URL missing");
        return false;
    }

    return true;
}

FMoQClientRef FMoQSessionWrapper::GetClient() const
{
    FScopeLock Lock(&ClientMutex);
    return Client;
}

void FMoQSessionWrapper::InvalidateAttempt()
{
    {
        FScopeLock Lock(&ConnectionContext->StateMutex);
        ConnectionContext->ActiveAttemptId.store(0);
    }

    GetAttemptRegistry().Unregister(ActiveAttemptToken);
    ActiveAttemptToken = nullptr;
    ActiveAttempt.Reset();
}

void FMoQSessionWrapper::ReleaseClient(bool bDisconnect)
{
    FMoQClientRef Old;
    {
        FScopeLock Lock(&ClientMutex);
        Old = MoveTemp(Client);
        Client.Reset();
    }

    if (bDisconnect && Old.IsValid() && Old->IsValid() && Api->Disconnect)
    {
        // Callbacks from this client carry a token that is no longer registered, so the
        // DISCONNECTED notification moq-ffi sends from inside this call is ignored.
        const MoqResult RawResult = Api->Disconnect(Old->Get());
        if (RawResult.code != MOQ_OK)
        {
            const FMoQResult Result = FMoQResult::FromResult(RawResult, *Api);
            UE_LOG(LogMoQBridge, Warning, TEXT("moq_disconnect failed: %s"), *Result.Message);
        }
        else
        {
            MoQFfi::CopyAndFreeString(*Api, RawResult.message);
        }
    }

    // Destroyed here unless an in-flight connect task, a publisher or a subscriber still holds it.
    Old.Reset();
}

FMoQResult FMoQSessionWrapper::Connect()
{
    FString Reason;
    if (!ValidateInitialized(Reason))
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, MoveTemp(Reason));
    }

    // TRF-8/TRF-11: every attempt uses a fresh client. The previous attempt (if still running)
    // becomes stale and closes its own client when its blocking moq_connect returns.
    InvalidateAttempt();
    ReleaseClient(/*bDisconnect=*/true);
    ClearSubscriberBindings();
    ClearAnnouncedNamespaces();

    FMoQClientRef NewClient = MakeShared<FMoQSessionHandle, ESPMode::ThreadSafe>(Api);
    if (!NewClient->IsValid())
    {
        return FMoQResult::FromCode(EMoQErrorCode::Internal, TEXT("Failed to create MoQ client handle"));
    }

    {
        FScopeLock Lock(&ClientMutex);
        Client = NewClient;
    }

    const uint64 AttemptId = ++NextAttemptId;
    TSharedRef<FMoQConnectAttempt, ESPMode::ThreadSafe> Attempt = MakeShared<FMoQConnectAttempt, ESPMode::ThreadSafe>();
    Attempt->AttemptId = AttemptId;
    Attempt->Connection = ConnectionContext;
    void* const Token = GetAttemptRegistry().Register(Attempt);
    ActiveAttempt = Attempt;
    ActiveAttemptToken = Token;

    {
        FScopeLock Lock(&ConnectionContext->StateMutex);
        ConnectionContext->ActiveAttemptId.store(AttemptId);
        ConnectionContext->CurrentState = MOQ_STATE_CONNECTING;
        ConnectionContext->LastConnectError.Reset();
    }

    UE_LOG(LogMoQBridge, Log, TEXT("Attempting to connect to: %s (attempt %llu)"), *O3DRedact::Url(RelayUrl), AttemptId);

    if (!Api->LaunchBlocking)
    {
        InvalidateAttempt();
        ReleaseClient(/*bDisconnect=*/false);
        return FMoQResult::FromCode(EMoQErrorCode::Internal, TEXT("No executor for the blocking connect call"));
    }

    // moq_connect blocks (moq-ffi runs it under its runtime's block_on with its own timeout),
    // so it never runs on the game thread. No lock is held across it.
    FMoQFfiApiRef ApiRef = Api;
    TSharedPtr<FMoQConnectAttempt, ESPMode::ThreadSafe> AttemptPtr = Attempt;
    FString Url = RelayUrl;
    Api->LaunchBlocking([ApiRef, NewClient, AttemptPtr, Token, Url]() mutable
    {
        RunConnectAttempt(*ApiRef, NewClient, *AttemptPtr, Token, Url);

        // Hand the client reference back so moq_client_destroy runs on the game thread when this
        // was the last reference. If the dispatcher is shut down the lambda (and the reference) is
        // dropped right here instead.
        FMoQClientRef ToRelease = MoveTemp(NewClient);
        FMoQAsyncDispatcher::Get().EnqueueGameThreadTask([ToRelease = MoveTemp(ToRelease)]() mutable
        {
            ToRelease.Reset();
        });
    });

    return FMoQResult::FromCode(EMoQErrorCode::Ok, TEXT("Connection initiated (async)"));
}

void FMoQSessionWrapper::RunConnectAttempt(const FMoQFfiApi& InApi, const FMoQClientRef& InClient, FMoQConnectAttempt& Attempt, void* Token, const FString& Url)
{
    // TRF-39: no try/catch here. A Rust panic is not a C++ exception; moq-ffi wraps every export
    // in catch_unwind and reports panics through the MoqResult instead.
    FTCHARToUTF8 UrlUtf8(*Url);
    const MoqResult RawResult = InApi.Connect
        ? InApi.Connect(InClient->Get(), UrlUtf8.Get(), &FMoQSessionWrapper::HandleConnectionStateThunk, Token)
        : MoqResult{MOQ_ERROR_INTERNAL, nullptr};

    const FMoQResult Wrapped = FMoQResult::FromResult(RawResult, InApi);
    // Same thread as the failing call, read at once: the only valid use of moq_last_error().
    const FString LastError = Wrapped.IsOk() ? FString() : MoQFfi::CopyLastErrorOnThisThread(InApi);

    const TSharedPtr<FMoQConnectionContext, ESPMode::ThreadSafe> Connection = Attempt.Connection.Pin();
    const bool bStale = !Connection.IsValid() || Connection->ActiveAttemptId.load() != Attempt.AttemptId;

    if (!Wrapped.IsOk())
    {
        FString Detail = Wrapped.Message;
        if (!LastError.IsEmpty() && LastError != Wrapped.Message)
        {
            Detail = FString::Printf(TEXT("%s (%s)"), *Wrapped.Message, *LastError);
        }
        UE_LOG(LogMoQBridge, Warning, TEXT("moq_connect failed (attempt %llu%s): %s"), Attempt.AttemptId, bStale ? TEXT(", abandoned") : TEXT(""), *Detail);

        if (!bStale)
        {
            {
                FScopeLock Lock(&Connection->StateMutex);
                Connection->LastConnectError = Detail;
            }
            // TRF-11: moq-ffi does not report FAILED on every error path (for example a bad URL
            // scheme), so publish a terminal state here if the callback did not.
            if (!Attempt.bTerminalReported.load())
            {
                ReportAttemptState(Attempt, MOQ_STATE_FAILED);
            }
        }
        return;
    }

    if (bStale)
    {
        // Connected after the attempt was abandoned (timeout, Disconnect or a newer Connect).
        UE_LOG(LogMoQBridge, Log, TEXT("moq_connect attempt %llu completed after it was abandoned; closing it"), Attempt.AttemptId);
        if (InApi.Disconnect)
        {
            const MoqResult DisconnectResult = InApi.Disconnect(InClient->Get());
            MoQFfi::CopyAndFreeString(InApi, DisconnectResult.message);
        }
        return;
    }

    if (!Attempt.bTerminalReported.load())
    {
        // OK without a CONNECTED callback: report it so the owner is not left waiting.
        ReportAttemptState(Attempt, MOQ_STATE_CONNECTED);
    }
    UE_LOG(LogMoQBridge, Log, TEXT("moq_connect succeeded (attempt %llu)"), Attempt.AttemptId);
}

void FMoQSessionWrapper::AbandonConnect()
{
    InvalidateAttempt();
    // The in-flight task still holds the client; it closes it when moq_connect returns.
    ReleaseClient(/*bDisconnect=*/false);
    ClearAnnouncedNamespaces();
    {
        FScopeLock Lock(&ConnectionContext->StateMutex);
        ConnectionContext->CurrentState = MOQ_STATE_DISCONNECTED;
    }
}

void FMoQSessionWrapper::Disconnect()
{
    InvalidateAttempt();
    {
        FScopeLock Lock(&ConnectionContext->StateMutex);
        ConnectionContext->CurrentState = MOQ_STATE_DISCONNECTED;
    }
    ReleaseClient(/*bDisconnect=*/true);
    ClearSubscriberBindings();
    ClearAnnouncedNamespaces();
}

bool FMoQSessionWrapper::IsConnected() const
{
    return ConnectionContext->CurrentState.Load() == MOQ_STATE_CONNECTED;
}

FString FMoQSessionWrapper::GetLastConnectError() const
{
    FScopeLock Lock(&ConnectionContext->StateMutex);
    return ConnectionContext->LastConnectError;
}

void FMoQSessionWrapper::ClearAnnouncedNamespaces()
{
    FScopeLock Lock(&NamespaceMutex);
    AnnouncedNamespaces.Reset();
    AnnouncedClient = nullptr;
}

void FMoQSessionWrapper::ClearSubscriberBindings()
{
    // Unregister before freeing: a callback that arrives after this finds no binding.
    FScopeLock Lock(&SubscriberMutex);
    for (TPair<MoqSubscriber*, FSubscriberEntry>& Pair : SubscriberBindings)
    {
        GetSubscriberRegistryFor<FSubscriberBinding>().Unregister(Pair.Value.Token);
    }
    SubscriberBindings.Reset();
}

FMoQResult FMoQSessionWrapper::AnnounceNamespace(const FString& Namespace)
{
    FString Normalized = NormalizeValue(Namespace);
    if (Normalized.IsEmpty())
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, TEXT("Namespace cannot be empty"));
    }

    return AnnounceOnClient(GetClient(), Normalized);
}

FMoQResult FMoQSessionWrapper::AnnounceOnClient(const FMoQClientRef& ClientRef, const FString& Normalized)
{
    if (!ClientRef.IsValid() || !ClientRef->IsValid())
    {
        return FMoQResult::FromCode(EMoQErrorCode::NotConnected, TEXT("No MoQ client (not connected)"));
    }

    {
        // TRF-8: the cache belongs to one client. After a reconnect (new client) every namespace
        // is announced again, once.
        FScopeLock Lock(&NamespaceMutex);
        if (AnnouncedClient != ClientRef.Get())
        {
            AnnouncedNamespaces.Reset();
            AnnouncedClient = ClientRef.Get();
        }
        if (AnnouncedNamespaces.Contains(Normalized))
        {
            return FMoQResult::Ok();
        }
    }

    FTCHARToUTF8 NamespaceUtf8(*Normalized);
    const MoqResult RawResult = Api->AnnounceNamespace
        ? Api->AnnounceNamespace(ClientRef->Get(), NamespaceUtf8.Get())
        : MoqResult{MOQ_ERROR_INTERNAL, nullptr};

    FMoQResult Wrapped = FMoQResult::FromResult(RawResult, *Api);
    if (Wrapped.IsOk())
    {
        FScopeLock Lock(&NamespaceMutex);
        if (AnnouncedClient == ClientRef.Get())
        {
            AnnouncedNamespaces.Add(Normalized);
        }
    }
    else
    {
        UE_LOG(LogMoQBridge, Warning, TEXT("moq_announce_namespace failed for '%s': %s"), *Normalized, *Wrapped.Message);
    }

    return Wrapped;
}

FMoQResult FMoQSessionWrapper::CreatePublisher(const FMoQPublisherConfig& Config, TSharedPtr<FMoQPublisherHandle>& OutPublisher)
{
    FString NamespaceValue = NormalizeValue(Config.Namespace);
    FString TrackValue = NormalizeValue(Config.TrackName);

    if (NamespaceValue.IsEmpty() || TrackValue.IsEmpty())
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, TEXT("Namespace and track name are required"));
    }

    if (!IsConnected())
    {
        return FMoQResult::FromCode(EMoQErrorCode::NotConnected, TEXT("Cannot create publisher when disconnected"));
    }

    const FMoQClientRef ClientRef = GetClient();
    FMoQResult AnnounceResult = AnnounceOnClient(ClientRef, NamespaceValue);
    if (!AnnounceResult.IsOk())
    {
        return AnnounceResult;
    }

    FTCHARToUTF8 NamespaceUtf8(*NamespaceValue);
    FTCHARToUTF8 TrackUtf8(*TrackValue);

    MoqPublisher* Publisher = Api->CreatePublisherEx
        ? Api->CreatePublisherEx(ClientRef->Get(), NamespaceUtf8.Get(), TrackUtf8.Get(), Config.DeliveryMode)
        : nullptr;

    if (Publisher == nullptr)
    {
        // Null return is the failure signal; moq_last_error() on this thread explains it.
        const FString LastError = MoQFfi::CopyLastErrorOnThisThread(*Api);
        return FMoQResult::FromCode(EMoQErrorCode::Internal, LastError.IsEmpty()
            ? FString::Printf(TEXT("Failed to create publisher for %s/%s"), *NamespaceValue, *TrackValue)
            : FString::Printf(TEXT("Failed to create publisher for %s/%s (%s)"), *NamespaceValue, *TrackValue, *LastError));
    }

    OutPublisher = MakeShared<FMoQPublisherHandle, ESPMode::ThreadSafe>(ClientRef, Publisher);
    return FMoQResult::Ok();
}

FMoQResult FMoQSessionWrapper::Subscribe(const FMoQSubscriptionConfig& Config, TSharedPtr<FMoQSubscriberHandle>& OutSubscriber)
{
    FString NamespaceValue = NormalizeValue(Config.Namespace);
    FString TrackValue = NormalizeValue(Config.TrackName);

    if (NamespaceValue.IsEmpty() || TrackValue.IsEmpty())
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, TEXT("Namespace and track are required for subscription"));
    }

    if (!Config.OnData)
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, TEXT("OnData callback must be provided"));
    }

    if (!IsConnected())
    {
        return FMoQResult::FromCode(EMoQErrorCode::NotConnected, TEXT("Cannot subscribe while disconnected"));
    }

    const FMoQClientRef ClientRef = GetClient();
    if (!ClientRef.IsValid() || !ClientRef->IsValid())
    {
        return FMoQResult::FromCode(EMoQErrorCode::NotConnected, TEXT("No MoQ client (not connected)"));
    }

    TSharedRef<FSubscriberBinding, ESPMode::ThreadSafe> Binding = MakeShared<FSubscriberBinding, ESPMode::ThreadSafe>();
    Binding->DataHandler = Config.OnData;
    void* const BindingToken = GetSubscriberRegistryFor<FSubscriberBinding>().Register(Binding);

    FTCHARToUTF8 NamespaceUtf8(*NamespaceValue);
    FTCHARToUTF8 TrackUtf8(*TrackValue);

    MoqSubscriber* Subscriber = Api->Subscribe
        ? Api->Subscribe(ClientRef->Get(), NamespaceUtf8.Get(), TrackUtf8.Get(), &FMoQSessionWrapper::HandleSubscriberDataThunk, BindingToken)
        : nullptr;

    if (Subscriber == nullptr)
    {
        GetSubscriberRegistryFor<FSubscriberBinding>().Unregister(BindingToken);

        // Null return is the failure signal; moq_last_error() on this thread explains it.
        const FString ExtraMessage = MoQFfi::CopyLastErrorOnThisThread(*Api);
        if (!ExtraMessage.IsEmpty())
        {
            return FMoQResult::FromCode(EMoQErrorCode::Internal, FString::Printf(TEXT("Failed to subscribe to %s/%s (%s)"), *NamespaceValue, *TrackValue, *ExtraMessage));
        }

        return FMoQResult::FromCode(EMoQErrorCode::Internal, FString::Printf(TEXT("Failed to subscribe to %s/%s"), *NamespaceValue, *TrackValue));
    }

    {
        FScopeLock Lock(&SubscriberMutex);
        FSubscriberEntry Entry;
        Entry.Binding = Binding;
        Entry.Token = BindingToken;
        SubscriberBindings.Add(Subscriber, MoveTemp(Entry));
    }

    TSharedPtr<FMoQSubscriberHandle> Handle = MakeShared<FMoQSubscriberHandle, ESPMode::ThreadSafe>(ClientRef, Subscriber);
    TWeakPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> WrapperWeak = SelfWeak;
    Handle->SetOnBeforeDestroy([WrapperWeak, Subscriber]()
    {
        if (const TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Pinned = WrapperWeak.Pin())
        {
            Pinned->RemoveSubscriberBinding(Subscriber);
        }
    });

    OutSubscriber = MoveTemp(Handle);
    return FMoQResult::Ok();
}

void FMoQSessionWrapper::Unsubscribe(const TSharedPtr<FMoQSubscriberHandle>& SubscriberHandle)
{
    if (!SubscriberHandle.IsValid())
    {
        return;
    }

    SubscriberHandle->Reset();
}

void FMoQSessionWrapper::HandleConnectionStateThunk(void* UserData, MoqConnectionState State)
{
    // Runs on a moq-ffi thread (for connect, the thread that called moq_connect). Never pins
    // the wrapper here: only the attempt and its connection context.
    if (const TSharedPtr<FMoQConnectAttempt, ESPMode::ThreadSafe> Attempt = GetAttemptRegistry().Resolve(UserData))
    {
        ReportAttemptState(*Attempt, State);
    }
}

bool FMoQSessionWrapper::ReportAttemptState(FMoQConnectAttempt& Attempt, MoqConnectionState State)
{
    const TSharedPtr<FMoQConnectionContext, ESPMode::ThreadSafe> Connection = Attempt.Connection.Pin();
    if (!Connection.IsValid())
    {
        return false;
    }

    if (IsTerminalConnectState(State) && Attempt.bTerminalReported.exchange(true))
    {
        // CONNECTED or FAILED was already reported for this attempt.
        return false;
    }

    {
        FScopeLock Lock(&Connection->StateMutex);
        if (Connection->ActiveAttemptId.load() != Attempt.AttemptId)
        {
            return false;
        }
        Connection->CurrentState = State;
    }

    UE_LOG(LogMoQBridge, Log, TEXT("Connection state changed: %s (attempt %llu)"), *LexToString(State), Attempt.AttemptId);

    if (State == MOQ_STATE_DISCONNECTED || State == MOQ_STATE_FAILED)
    {
        // TRF-39: moq_last_error() is thread-local and this callback may run on a different
        // thread from the call that failed, so it is not consulted here. RunConnectAttempt
        // records connect errors on the failing thread (see GetLastConnectError()).
        UE_LOG(LogMoQBridge, Warning, TEXT("MoQ session state %s (attempt %llu)"), *LexToString(State), Attempt.AttemptId);
    }

    TWeakPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> WrapperWeak = Connection->Wrapper;
    if (!WrapperWeak.IsValid())
    {
        return false;
    }

    // The wrapper is pinned only on the game thread, so its destructor (which calls into
    // moq-ffi) never runs on an FFI thread.
    const uint64 AttemptId = Attempt.AttemptId;
    return FMoQAsyncDispatcher::Get().EnqueueGameThreadTask([WrapperWeak, AttemptId, State]()
    {
        if (const TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Pinned = WrapperWeak.Pin())
        {
            Pinned->HandleConnectionStateOnGameThread(AttemptId, State);
        }
    });
}

void FMoQSessionWrapper::HandleConnectionStateOnGameThread(uint64 AttemptId, MoqConnectionState State)
{
    if (ConnectionContext->ActiveAttemptId.load() != AttemptId)
    {
        // Superseded between the FFI callback and now (Disconnect, timeout or a newer Connect).
        return;
    }

    if (State == MOQ_STATE_DISCONNECTED || State == MOQ_STATE_FAILED)
    {
        // TRF-8: nothing announced on this connection survives it.
        ClearAnnouncedNamespaces();
    }

    ConnectionStateDelegate.Broadcast(State);
}

void FMoQSessionWrapper::HandleConnectionStateInternal(MoqConnectionState State)
{
    const uint64 AttemptId = ConnectionContext->ActiveAttemptId.load();
    {
        FScopeLock Lock(&ConnectionContext->StateMutex);
        ConnectionContext->CurrentState = State;
    }

    const TWeakPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> WrapperWeak = ConnectionContext->Wrapper;
    const bool bQueued = WrapperWeak.IsValid() && FMoQAsyncDispatcher::Get().EnqueueGameThreadTask([WrapperWeak, AttemptId, State]()
    {
        if (const TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Pinned = WrapperWeak.Pin())
        {
            Pinned->HandleConnectionStateOnGameThread(AttemptId, State);
        }
    });

    if (!bQueued)
    {
        UE_LOG(LogMoQBridge, VeryVerbose, TEXT("Connection state delivered directly (state=%s)."), *LexToString(State));
        HandleConnectionStateOnGameThread(AttemptId, State);
    }
}

void FMoQSessionWrapper::HandleSubscriberDataThunk(void* UserData, const uint8_t* Data, size_t DataLen)
{
    // Runs on a moq-ffi thread. UserData is an opaque token; an unregistered one resolves to null.
    const TSharedPtr<FSubscriberBinding, ESPMode::ThreadSafe> Binding = GetSubscriberRegistryFor<FSubscriberBinding>().Resolve(UserData);
    if (!Binding.IsValid() || !Binding->DataHandler)
    {
        return;
    }

    TArray64<uint8> Payload;
    Payload.SetNumUninitialized(static_cast<int64>(DataLen));
    if (DataLen > 0)
    {
        FMemory::Memcpy(Payload.GetData(), Data, DataLen);
    }

    FMoQAsyncDispatcher::Get().EnqueueGameThreadTask([Handler = Binding->DataHandler, Payload = MoveTemp(Payload)]() mutable
    {
        if (Handler)
        {
            Handler(Payload);
        }
    });
}

void FMoQSessionWrapper::RemoveSubscriberBinding(MoqSubscriber* Subscriber)
{
    if (Subscriber == nullptr)
    {
        return;
    }

    FScopeLock Lock(&SubscriberMutex);
    if (FSubscriberEntry* Entry = SubscriberBindings.Find(Subscriber))
    {
        GetSubscriberRegistryFor<FSubscriberBinding>().Unregister(Entry->Token);
    }
    SubscriberBindings.Remove(Subscriber);
}

#if WITH_DEV_AUTOMATION_TESTS
void FMoQSessionWrapper::InvokeSubscriberThunkForTest(const TFunction<void(const TArray64<uint8>&)>& Callback, const TArray64<uint8>& Payload)
{
    TSharedRef<FSubscriberBinding, ESPMode::ThreadSafe> Binding = MakeShared<FSubscriberBinding, ESPMode::ThreadSafe>();
    Binding->DataHandler = Callback;
    void* Token = GetSubscriberRegistryFor<FSubscriberBinding>().Register(Binding);
    const uint8* DataPtr = Payload.Num() > 0 ? Payload.GetData() : nullptr;
    HandleSubscriberDataThunk(Token, DataPtr, static_cast<size_t>(Payload.Num()));
    GetSubscriberRegistryFor<FSubscriberBinding>().Unregister(Token);
}
#endif

#endif // O3D_WITH_TRANSPORT_MOQ
