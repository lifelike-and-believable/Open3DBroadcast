#include "Shared/MoQHandles.h"

#include "Misc/ScopeLock.h"
#include "moq_ffi.h"

FMoQSessionHandle::FMoQSessionHandle()
{
    EnsureCreated();
}

FMoQSessionHandle::~FMoQSessionHandle()
{
    Reset();
}

FMoQSessionHandle::FMoQSessionHandle(FMoQSessionHandle&& Other) noexcept
{
    FScopeLock Lock(&Other.Mutex);
    Client = Other.Client;
    Other.Client = nullptr;
}

FMoQSessionHandle& FMoQSessionHandle::operator=(FMoQSessionHandle&& Other) noexcept
{
    if (this != &Other)
    {
        Reset();
        FScopeLock Lock(&Other.Mutex);
        Client = Other.Client;
        Other.Client = nullptr;
    }
    return *this;
}

FMoQResult FMoQSessionHandle::EnsureCreated()
{
    FScopeLock Lock(&Mutex);
    if (Client != nullptr)
    {
        return FMoQResult::Ok();
    }

    Client = moq_client_create();
    if (Client == nullptr)
    {
        UE_LOG(LogMoQBridge, Error, TEXT("moq_client_create returned null"));
        return FMoQResult::FromCode(EMoQErrorCode::Internal, TEXT("Failed to create MoQ client handle"));
    }

    return FMoQResult::Ok();
}

void FMoQSessionHandle::Reset()
{
    MoqClient* OldClient = nullptr;
    {
        FScopeLock Lock(&Mutex);
        if (Client != nullptr)
        {
            OldClient = Client;
            Client = nullptr;
        }
    }

    if (OldClient != nullptr)
    {
        moq_client_destroy(OldClient);
    }
}

bool FMoQSessionHandle::IsValid() const
{
    FScopeLock Lock(&Mutex);
    return Client != nullptr;
}

FMoQPublisherHandle::FMoQPublisherHandle(MoqPublisher* InPublisher)
    : Publisher(InPublisher)
{
}

FMoQPublisherHandle::~FMoQPublisherHandle()
{
    Reset();
}

FMoQPublisherHandle::FMoQPublisherHandle(FMoQPublisherHandle&& Other) noexcept
{
    Publisher = Other.Publisher;
    Other.Publisher = nullptr;
}

FMoQPublisherHandle& FMoQPublisherHandle::operator=(FMoQPublisherHandle&& Other) noexcept
{
    if (this != &Other)
    {
        Reset();
        Publisher = Other.Publisher;
        Other.Publisher = nullptr;
    }
    return *this;
}

void FMoQPublisherHandle::Reset(MoqPublisher* InPublisher)
{
    if (Publisher != nullptr)
    {
        moq_publisher_destroy(Publisher);
    }
    Publisher = InPublisher;
}

FMoQSubscriberHandle::FMoQSubscriberHandle(MoqSubscriber* InSubscriber)
    : Subscriber(InSubscriber)
{
}

FMoQSubscriberHandle::~FMoQSubscriberHandle()
{
    Reset();
}

FMoQSubscriberHandle::FMoQSubscriberHandle(FMoQSubscriberHandle&& Other) noexcept
{
    Subscriber = Other.Subscriber;
    Other.Subscriber = nullptr;
}

FMoQSubscriberHandle& FMoQSubscriberHandle::operator=(FMoQSubscriberHandle&& Other) noexcept
{
    if (this != &Other)
    {
        Reset();
        Subscriber = Other.Subscriber;
        Other.Subscriber = nullptr;
    }
    return *this;
}

void FMoQSubscriberHandle::Reset(MoqSubscriber* InSubscriber)
{
    if (Subscriber != nullptr)
    {
        if (OnBeforeDestroy)
        {
            OnBeforeDestroy();
        }
        moq_subscriber_destroy(Subscriber);
    }
    Subscriber = InSubscriber;
}
