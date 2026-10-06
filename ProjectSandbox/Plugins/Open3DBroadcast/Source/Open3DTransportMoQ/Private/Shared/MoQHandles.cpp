// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Shared/MoQHandles.h"

FMoQSessionHandle::FMoQSessionHandle(FMoQFfiApiRef InApi)
    : Api(MoveTemp(InApi))
{
    if (Api->ClientCreate)
    {
        Client = Api->ClientCreate();
    }

    if (Client == nullptr)
    {
        UE_LOG(LogMoQBridge, Error, TEXT("moq_client_create returned null"));
    }
}

FMoQSessionHandle::~FMoQSessionHandle()
{
    if (Client != nullptr && Api->ClientDestroy)
    {
        Api->ClientDestroy(Client);
    }
    Client = nullptr;
}

FMoQPublisherHandle::FMoQPublisherHandle(FMoQClientRef InClient, MoqPublisher* InPublisher)
    : Client(MoveTemp(InClient))
    , Publisher(InPublisher)
{
}

FMoQPublisherHandle::~FMoQPublisherHandle()
{
    Reset();
}

void FMoQPublisherHandle::Reset()
{
    if (Publisher != nullptr && Client.IsValid() && Client->GetApi().PublisherDestroy)
    {
        Client->GetApi().PublisherDestroy(Publisher);
    }
    Publisher = nullptr;
    // Released after the publisher, so the client outlives every publisher created from it.
    Client.Reset();
}

FMoQResult FMoQPublisherHandle::Publish(const uint8* Data, int64 NumBytes, MoqDeliveryMode DeliveryMode) const
{
    if (Publisher == nullptr || !Client.IsValid() || !Client->GetApi().PublishData)
    {
        return FMoQResult::FromCode(EMoQErrorCode::NotConnected, TEXT("Publisher is not valid"));
    }

    const FMoQFfiApi& Api = Client->GetApi();
    const MoqResult Raw = Api.PublishData(Publisher, Data, static_cast<size_t>(NumBytes), DeliveryMode);
    if (Raw.code == MOQ_OK)
    {
        // An OK result may still carry a message owned by moq-ffi; free it.
        MoQFfi::CopyAndFreeString(Api, Raw.message);
        return FMoQResult::Ok();
    }
    return FMoQResult::FromResult(Raw, Api);
}

FMoQSubscriberHandle::FMoQSubscriberHandle(FMoQClientRef InClient, MoqSubscriber* InSubscriber)
    : Client(MoveTemp(InClient))
    , Subscriber(InSubscriber)
{
}

FMoQSubscriberHandle::~FMoQSubscriberHandle()
{
    Reset();
}

void FMoQSubscriberHandle::Reset()
{
    if (Subscriber != nullptr)
    {
        if (Client.IsValid() && Client->GetApi().SubscriberDestroy)
        {
            Client->GetApi().SubscriberDestroy(Subscriber);
        }
        if (OnBeforeDestroy)
        {
            OnBeforeDestroy();
        }
    }
    Subscriber = nullptr;
    Client.Reset();
}

#endif // O3D_WITH_TRANSPORT_MOQ
