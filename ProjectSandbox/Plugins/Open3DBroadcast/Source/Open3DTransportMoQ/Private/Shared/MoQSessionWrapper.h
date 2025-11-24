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
    void Unsubscribe(const TSharedPtr<FMoQSubscriberHandle>& SubscriberHandle);

private:
    struct FSubscriberBinding
    {
        TFunction<void(const TArray64<uint8>&)> DataHandler;
    };

    static void HandleConnectionStateThunk(void* UserData, MoqConnectionState State);
    void HandleConnectionStateInternal(MoqConnectionState State);

    static void HandleSubscriberDataThunk(void* UserData, const uint8_t* Data, size_t DataLen);

    bool ValidateInitialized(FString& OutReason) const;
    FMoQResult EnsureClientAvailable();
    void RemoveSubscriberBinding(MoqSubscriber* Subscriber);

    FMoQSessionHandle SessionHandle;
    FString RelayUrl;
    FThreadSafeBool bInitialized = false;
    TAtomic<MoqConnectionState> CurrentState;

    TSet<FString> AnnouncedNamespaces;
    mutable FCriticalSection NamespaceMutex;

    TMap<MoqSubscriber*, TUniquePtr<FSubscriberBinding>> SubscriberBindings;
    mutable FCriticalSection SubscriberMutex;

    FMoQConnectionStateDelegate ConnectionStateDelegate;
};
