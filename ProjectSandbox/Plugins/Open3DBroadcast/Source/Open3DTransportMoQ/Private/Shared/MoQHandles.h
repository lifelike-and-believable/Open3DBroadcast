#pragma once

#include "CoreMinimal.h"
#include "Shared/MoQTypes.h"

#include "Templates/Function.h"

/** RAII wrapper for the MoQ client session handle */
class FMoQSessionHandle
{
public:
    FMoQSessionHandle();
    ~FMoQSessionHandle();

    FMoQSessionHandle(FMoQSessionHandle&& Other) noexcept;
    FMoQSessionHandle& operator=(FMoQSessionHandle&& Other) noexcept;

    FMoQSessionHandle(const FMoQSessionHandle&) = delete;
    FMoQSessionHandle& operator=(const FMoQSessionHandle&) = delete;

    /** Lazily create the underlying session if needed */
    FMoQResult EnsureCreated();

    /** Destroy the underlying session (safe to call multiple times) */
    void Reset();

    /** @return true if the underlying session handle is valid */
    bool IsValid() const;

    /** Direct access to the raw pointer (callers should synchronize externally) */
    FORCEINLINE MoqClient* GetUnsafe() const { return Client; }

    /** Provides access to the mutex guarding the underlying pointer */
    FORCEINLINE FCriticalSection& GetMutex() const { return Mutex; }

private:
    MoqClient* Client = nullptr;
    mutable FCriticalSection Mutex;
};

/** RAII wrapper for MoQ publisher handles */
class FMoQPublisherHandle
{
public:
    FMoQPublisherHandle() = default;
    explicit FMoQPublisherHandle(MoqPublisher* InPublisher);
    ~FMoQPublisherHandle();

    FMoQPublisherHandle(FMoQPublisherHandle&& Other) noexcept;
    FMoQPublisherHandle& operator=(FMoQPublisherHandle&& Other) noexcept;

    FMoQPublisherHandle(const FMoQPublisherHandle&) = delete;
    FMoQPublisherHandle& operator=(const FMoQPublisherHandle&) = delete;

    void Reset(MoqPublisher* InPublisher = nullptr);
    bool IsValid() const { return Publisher != nullptr; }
    FORCEINLINE MoqPublisher* Get() const { return Publisher; }

private:
    MoqPublisher* Publisher = nullptr;
};

/** RAII wrapper for MoQ subscriber handles */
class FMoQSubscriberHandle
{
public:
    FMoQSubscriberHandle() = default;
    explicit FMoQSubscriberHandle(MoqSubscriber* InSubscriber);
    ~FMoQSubscriberHandle();

    FMoQSubscriberHandle(FMoQSubscriberHandle&& Other) noexcept;
    FMoQSubscriberHandle& operator=(FMoQSubscriberHandle&& Other) noexcept;

    FMoQSubscriberHandle(const FMoQSubscriberHandle&) = delete;
    FMoQSubscriberHandle& operator=(const FMoQSubscriberHandle&) = delete;

    void Reset(MoqSubscriber* InSubscriber = nullptr);
    bool IsValid() const { return Subscriber != nullptr; }
    FORCEINLINE MoqSubscriber* Get() const { return Subscriber; }

    void SetOnBeforeDestroy(TFunction<void()>&& Callback) { OnBeforeDestroy = MoveTemp(Callback); }

private:
    MoqSubscriber* Subscriber = nullptr;
    TFunction<void()> OnBeforeDestroy;
};
