#pragma once

#include "CoreMinimal.h"
#include "Shared/MoQFfiApi.h"
#include "Shared/MoQTypes.h"

#include "Templates/Function.h"

/**
 * Owns one MoqClient for its whole life (WP-S8). The client is created in the constructor and
 * destroyed in the destructor, so the pointer never changes while the handle exists and FFI
 * calls need no lock here (moq-ffi documents its client calls as thread-safe).
 *
 * Each connect attempt gets a fresh handle (TRF-8, TRF-11): an abandoned attempt keeps its own
 * handle alive until its blocking moq_connect returns, and publishers and subscribers keep a
 * reference to the handle they were created from, so a client is never destroyed while an FFI
 * call or a child object still uses it.
 */
class FMoQSessionHandle
{
public:
    explicit FMoQSessionHandle(FMoQFfiApiRef InApi);
    ~FMoQSessionHandle();

    FMoQSessionHandle(const FMoQSessionHandle&) = delete;
    FMoQSessionHandle& operator=(const FMoQSessionHandle&) = delete;
    FMoQSessionHandle(FMoQSessionHandle&&) = delete;
    FMoQSessionHandle& operator=(FMoQSessionHandle&&) = delete;

    /** @return true if moq_client_create returned a client */
    bool IsValid() const { return Client != nullptr; }

    FORCEINLINE MoqClient* Get() const { return Client; }
    FORCEINLINE const FMoQFfiApi& GetApi() const { return *Api; }

private:
    FMoQFfiApiRef Api;
    MoqClient* Client = nullptr;
};

using FMoQClientRef = TSharedPtr<FMoQSessionHandle, ESPMode::ThreadSafe>;

/** RAII wrapper for MoQ publisher handles */
class FMoQPublisherHandle
{
public:
    FMoQPublisherHandle() = default;
    FMoQPublisherHandle(FMoQClientRef InClient, MoqPublisher* InPublisher);
    ~FMoQPublisherHandle();

    FMoQPublisherHandle(const FMoQPublisherHandle&) = delete;
    FMoQPublisherHandle& operator=(const FMoQPublisherHandle&) = delete;
    FMoQPublisherHandle(FMoQPublisherHandle&&) = delete;
    FMoQPublisherHandle& operator=(FMoQPublisherHandle&&) = delete;

    /** Destroys the publisher (safe to call more than once). */
    void Reset();
    bool IsValid() const { return Publisher != nullptr; }
    FORCEINLINE MoqPublisher* Get() const { return Publisher; }

    /** Publishes through the table of the client this publisher belongs to. */
    FMoQResult Publish(const uint8* Data, int64 NumBytes, MoqDeliveryMode DeliveryMode) const;

private:
    FMoQClientRef Client;
    MoqPublisher* Publisher = nullptr;
};

/** RAII wrapper for MoQ subscriber handles */
class FMoQSubscriberHandle
{
public:
    FMoQSubscriberHandle() = default;
    FMoQSubscriberHandle(FMoQClientRef InClient, MoqSubscriber* InSubscriber);
    ~FMoQSubscriberHandle();

    FMoQSubscriberHandle(const FMoQSubscriberHandle&) = delete;
    FMoQSubscriberHandle& operator=(const FMoQSubscriberHandle&) = delete;
    FMoQSubscriberHandle(FMoQSubscriberHandle&&) = delete;
    FMoQSubscriberHandle& operator=(FMoQSubscriberHandle&&) = delete;

    /** Destroys the subscriber (safe to call more than once). */
    void Reset();
    bool IsValid() const { return Subscriber != nullptr; }
    FORCEINLINE MoqSubscriber* Get() const { return Subscriber; }

    void SetOnBeforeDestroy(TFunction<void()>&& Callback) { OnBeforeDestroy = MoveTemp(Callback); }

private:
    FMoQClientRef Client;
    MoqSubscriber* Subscriber = nullptr;
    TFunction<void()> OnBeforeDestroy;
};
