#include "Shared/MoQSessionWrapper.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Shared/MoQAsyncDispatcher.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "Containers/StringConv.h"
#include "HAL/UnrealMemory.h"
#include "moq_ffi.h"
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
    TO3DFfiContextRegistry<FMoQConnectionContext>& GetConnectionRegistry()
    {
        static TO3DFfiContextRegistry<FMoQConnectionContext> Registry;
        return Registry;
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
    : ConnectionContext(MakeShared<FMoQConnectionContext, ESPMode::ThreadSafe>())
{
    ConnectionToken = GetConnectionRegistry().Register(ConnectionContext);
}

FMoQSessionWrapper::~FMoQSessionWrapper()
{
    Disconnect();
    SessionHandle.Reset();
    // After this, a late connection callback resolves the token to nothing.
    GetConnectionRegistry().Unregister(ConnectionToken);
    ConnectionToken = nullptr;
}

FMoQResult FMoQSessionWrapper::Initialize(const FString& InRelayUrl)
{
    FString Normalized = NormalizeValue(InRelayUrl);
    if (Normalized.IsEmpty())
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, TEXT("Relay URL cannot be empty"));
    }

    RelayUrl = MoveTemp(Normalized);
    AnnouncedNamespaces.Reset();
    bInitialized = true;

    if (!SelfWeak.IsValid())
    {
        SelfWeak = AsShared();
    }
    ConnectionContext->Wrapper = SelfWeak;

    return SessionHandle.EnsureCreated();
}

FMoQResult FMoQSessionWrapper::EnsureClientAvailable()
{
    return SessionHandle.EnsureCreated();
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

FMoQResult FMoQSessionWrapper::Connect()
{
    FString Reason;
    if (!ValidateInitialized(Reason))
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, MoveTemp(Reason));
    }

    FMoQResult EnsureResult = EnsureClientAvailable();
    if (!EnsureResult.IsOk())
    {
        return EnsureResult;
    }

    // Reset any previous "expected disconnect" markers now that we're attempting
    // to bring the session back online.
    ConnectionContext->bExpectingDisconnect.Store(false);

    // Ensure dispatcher is initialized before connecting (callbacks may fire immediately)
    FMoQAsyncDispatcher::Get().Initialize();

    UE_LOG(LogMoQBridge, Log, TEXT("Attempting to connect to: %s"), *RelayUrl);

    // CRITICAL: Run moq_connect on background thread to avoid blocking game thread
    // The Tokio runtime inside moq-ffi may block waiting for connection
    FString RelayUrlCopy = RelayUrl;
    TWeakPtr<FMoQSessionWrapper> WeakSelf = SelfWeak;
    void* const Token = ConnectionToken;
    
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSelf, RelayUrlCopy, Token]()
    {
        TSharedPtr<FMoQSessionWrapper> StrongThis = WeakSelf.Pin();
        if (!StrongThis.IsValid())
        {
            return;
        }

        FTCHARToUTF8 UrlUtf8(*RelayUrlCopy);
        MoqResult RawResult;
        
        // Call moq_connect - may block on Tokio runtime
        try
        {
            FScopeLock Lock(&StrongThis->SessionHandle.GetMutex());
            
            UE_LOG(LogMoQBridge, Verbose, TEXT("Calling moq_connect (background thread) with URL: %s"), UTF8_TO_TCHAR(UrlUtf8.Get()));
            
            RawResult = moq_connect(
                StrongThis->SessionHandle.GetUnsafe(),
                UrlUtf8.Get(),
                &FMoQSessionWrapper::HandleConnectionStateThunk,
                Token
            );
            
            UE_LOG(LogMoQBridge, Verbose, TEXT("moq_connect returned: code=%d"), (int)RawResult.code);
        }
        catch (...)
        {
            UE_LOG(LogMoQBridge, Error, TEXT("Exception caught during moq_connect - possible Rust panic"));
            
            if (const char* LastError = moq_last_error())
            {
                UE_LOG(LogMoQBridge, Error, TEXT("FFI Last Error: %s"), UTF8_TO_TCHAR(LastError));
            }
            return;
        }

        FMoQResult Wrapped = FMoQResult::FromResult(RawResult);
        if (!Wrapped.IsOk())
        {
            UE_LOG(LogMoQBridge, Error, TEXT("moq_connect failed: %s"), *Wrapped.Message);
            
            if (const char* LastError = moq_last_error())
            {
                UE_LOG(LogMoQBridge, Error, TEXT("Additional error info: %s"), UTF8_TO_TCHAR(LastError));
            }
        }
        else
        {
            UE_LOG(LogMoQBridge, Log, TEXT("moq_connect succeeded"));
        }
    });

    // Return immediately - connection state will be reported via callback
    return FMoQResult::FromCode(EMoQErrorCode::Ok, TEXT("Connection initiated (async)"));
}

void FMoQSessionWrapper::Disconnect()
{
    if (!SessionHandle.IsValid())
    {
        return;
    }

    ConnectionContext->bExpectingDisconnect.Store(true);

    {
        FScopeLock Lock(&SessionHandle.GetMutex());
        if (SessionHandle.GetUnsafe() != nullptr)
        {
            MoqResult RawResult = moq_disconnect(SessionHandle.GetUnsafe());
            if (RawResult.code != MOQ_OK)
            {
                FMoQResult Result = FMoQResult::FromResult(RawResult);
                UE_LOG(LogMoQBridge, Warning, TEXT("moq_disconnect failed: %s"), *Result.Message);
            }
        }
    }

    ConnectionContext->CurrentState = MOQ_STATE_DISCONNECTED;

    {
        // Unregister before freeing: a callback that arrives after this finds no binding.
        FScopeLock Lock(&SubscriberMutex);
        for (TPair<MoqSubscriber*, FSubscriberEntry>& Pair : SubscriberBindings)
        {
            GetSubscriberRegistryFor<FSubscriberBinding>().Unregister(Pair.Value.Token);
        }
        SubscriberBindings.Reset();
    }

    {
        FScopeLock Lock(&NamespaceMutex);
        AnnouncedNamespaces.Reset();
    }

    ConnectionContext->bExpectingDisconnect.Store(false);
}

bool FMoQSessionWrapper::IsConnected() const
{
    return ConnectionContext->CurrentState.Load() == MOQ_STATE_CONNECTED;
}

FMoQResult FMoQSessionWrapper::AnnounceNamespace(const FString& Namespace)
{
    FString Normalized = NormalizeValue(Namespace);
    if (Normalized.IsEmpty())
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, TEXT("Namespace cannot be empty"));
    }

    {
        FScopeLock Lock(&NamespaceMutex);
        if (AnnouncedNamespaces.Contains(Normalized))
        {
            return FMoQResult::Ok();
        }
    }

    FTCHARToUTF8 NamespaceUtf8(*Normalized);
    MoqResult RawResult;
    {
        FScopeLock Lock(&SessionHandle.GetMutex());
        RawResult = moq_announce_namespace(SessionHandle.GetUnsafe(), NamespaceUtf8.Get());
    }

    FMoQResult Wrapped = FMoQResult::FromResult(RawResult);
    if (Wrapped.IsOk())
    {
        FScopeLock Lock(&NamespaceMutex);
        AnnouncedNamespaces.Add(Normalized);
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

    FMoQResult AnnounceResult = AnnounceNamespace(NamespaceValue);
    if (!AnnounceResult.IsOk())
    {
        return AnnounceResult;
    }

    FTCHARToUTF8 NamespaceUtf8(*NamespaceValue);
    FTCHARToUTF8 TrackUtf8(*TrackValue);

    MoqPublisher* Publisher = nullptr;
    {
        FScopeLock Lock(&SessionHandle.GetMutex());
        Publisher = moq_create_publisher_ex(SessionHandle.GetUnsafe(), NamespaceUtf8.Get(), TrackUtf8.Get(), Config.DeliveryMode);
    }

    if (Publisher == nullptr)
    {
        return FMoQResult::FromCode(EMoQErrorCode::Internal, FString::Printf(TEXT("Failed to create publisher for %s/%s"), *NamespaceValue, *TrackValue));
    }

    OutPublisher = MakeShared<FMoQPublisherHandle>(Publisher);
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

    TSharedRef<FSubscriberBinding, ESPMode::ThreadSafe> Binding = MakeShared<FSubscriberBinding, ESPMode::ThreadSafe>();
    Binding->DataHandler = Config.OnData;
    void* const BindingToken = GetSubscriberRegistryFor<FSubscriberBinding>().Register(Binding);

    FTCHARToUTF8 NamespaceUtf8(*NamespaceValue);
    FTCHARToUTF8 TrackUtf8(*TrackValue);

    MoqSubscriber* Subscriber = nullptr;
    {
        FScopeLock Lock(&SessionHandle.GetMutex());
        Subscriber = moq_subscribe(SessionHandle.GetUnsafe(), NamespaceUtf8.Get(), TrackUtf8.Get(), &FMoQSessionWrapper::HandleSubscriberDataThunk, BindingToken);
    }

    if (Subscriber == nullptr)
    {
        GetSubscriberRegistryFor<FSubscriberBinding>().Unregister(BindingToken);

        FString ExtraMessage;
        if (const char* LastError = moq_last_error())
        {
            ExtraMessage = UTF8_TO_TCHAR(LastError);
        }

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

    TSharedPtr<FMoQSubscriberHandle> Handle = MakeShared<FMoQSubscriberHandle>(Subscriber);
    TWeakPtr<FMoQSessionWrapper> WrapperWeak = SelfWeak;
    Handle->SetOnBeforeDestroy([WrapperWeak, Subscriber]()
    {
        if (const TSharedPtr<FMoQSessionWrapper> Pinned = WrapperWeak.Pin())
        {
            Pinned->RemoveSubscriberBinding(Subscriber);
        }
    });

    OutSubscriber = MoveTemp(Handle);
    return FMoQResult::Ok();
}

FMoQResult FMoQSessionWrapper::SubscribeAsync(const FMoQSubscriptionConfig& Config, FSubscribeAsyncCallback&& Completion)
{
    if (!Completion)
    {
        return FMoQResult::FromCode(EMoQErrorCode::InvalidArgument, TEXT("Completion callback must be provided"));
    }

    FMoQAsyncDispatcher::Get().Initialize();

    if (!SelfWeak.IsValid())
    {
        SelfWeak = AsShared();
    }

    const FMoQSubscriptionConfig ConfigCopy = Config;
    TWeakPtr<FMoQSessionWrapper> WeakSelf = SelfWeak;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSelf, ConfigCopy, Completion = MoveTemp(Completion)]() mutable
    {
        TSharedPtr<FMoQSessionWrapper> StrongThis = WeakSelf.Pin();
        if (!StrongThis.IsValid())
        {
            FMoQAsyncDispatcher::Get().EnqueueGameThreadTask([Completion = MoveTemp(Completion)]() mutable
            {
                Completion(FMoQResult::FromCode(EMoQErrorCode::Internal, TEXT("Session destroyed before subscribe executed")), nullptr);
            });
            return;
        }

        TSharedPtr<FMoQSubscriberHandle> Subscriber;
        FMoQResult Result = StrongThis->Subscribe(ConfigCopy, Subscriber);

        FMoQAsyncDispatcher::Get().EnqueueGameThreadTask([Completion = MoveTemp(Completion), Result = MoveTemp(Result), Subscriber]() mutable
        {
            Completion(Result, Subscriber);
        });
    });

    return FMoQResult::FromCode(EMoQErrorCode::Ok, TEXT("Subscribe enqueued (async)"));
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
    // Runs on a moq-ffi (Tokio) thread. Never pins the wrapper here: only the context.
    if (const TSharedPtr<FMoQConnectionContext, ESPMode::ThreadSafe> Context = GetConnectionRegistry().Resolve(UserData))
    {
        RecordConnectionState(*Context, State);
    }
}

bool FMoQSessionWrapper::RecordConnectionState(FMoQConnectionContext& Context, MoqConnectionState State)
{
    Context.CurrentState = State;

    // Log state transitions for debugging
    UE_LOG(LogMoQBridge, Log, TEXT("Connection state changed: %s"), *LexToString(State));

    const bool bUnexpectedDisconnect =
        (State == MOQ_STATE_DISCONNECTED || State == MOQ_STATE_FAILED) &&
        !Context.bExpectingDisconnect.Load();

    if (bUnexpectedDisconnect)
    {
        if (const char* LastError = moq_last_error())
        {
            UE_LOG(LogMoQBridge, Warning, TEXT("Unexpected MoQ session state %s: %s"), *LexToString(State), UTF8_TO_TCHAR(LastError));
        }
        else
        {
            UE_LOG(LogMoQBridge, Warning, TEXT("Unexpected MoQ session state %s (no additional FFI error provided)"), *LexToString(State));
        }
    }

    TWeakPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> WrapperWeak = Context.Wrapper;
    if (!WrapperWeak.IsValid())
    {
        return false;
    }

    // The wrapper is pinned only on the game thread, so its destructor (which calls into
    // moq-ffi) never runs on an FFI thread.
    FMoQAsyncDispatcher::Get().EnqueueGameThreadTask([WrapperWeak, State]()
    {
        if (const TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Pinned = WrapperWeak.Pin())
        {
            Pinned->ConnectionStateDelegate.Broadcast(State);
        }
    });
    return true;
}

void FMoQSessionWrapper::HandleConnectionStateInternal(MoqConnectionState State)
{
    if (!RecordConnectionState(*ConnectionContext, State))
    {
        UE_LOG(LogMoQBridge, VeryVerbose, TEXT("Connection state callback received without a valid self-reference (state=%s)."), *LexToString(State));
        ConnectionStateDelegate.Broadcast(State);
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
