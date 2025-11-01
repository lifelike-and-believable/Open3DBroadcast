// Copyright (c) Open3DStream Contributors

#include "O3DSBroadcastLoopbackAdapter.h"
#include "O3DSBroadcastComponent.h"
#include "Open3DBroadcast.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#if O3DS_HAVE_STREAM_MODULE
#include "Open3DStreamSource.h"
#endif
#include "ILiveLinkClient.h"
#include "O3DSLoopback.h"
#include "Features/IModularFeatures.h"

static TAutoConsoleVariable<int32> CVarO3DSBroadcastLoopback(
    TEXT("o3ds.Broadcast.Loopback"),
    0,
    TEXT("Enable dev-only in-memory loopback to LiveLink (0/1). Editor-only."),
    ECVF_Default);

UO3DSBroadcastLoopbackAdapter::UO3DSBroadcastLoopbackAdapter()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UO3DSBroadcastLoopbackAdapter::OnRegister()
{
    Super::OnRegister();
#if WITH_EDITOR
    if (GetLoopbackEnabled() != 0)
    {
        UE_LOG(LogO3DSBroadcast, Log, TEXT("[Loopback] OnRegister on %s"), *GetNameSafe(GetOwner()));
        EnsureHooked();
    }
#endif
}

void UO3DSBroadcastLoopbackAdapter::BeginPlay()
{
    Super::BeginPlay();
#if WITH_EDITOR
    EnsureHooked();
#endif
}

void UO3DSBroadcastLoopbackAdapter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
#if WITH_EDITOR
    // Let consumer handle its own teardown by going out of scope
#endif
    Unhook();
    Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
int32 UO3DSBroadcastLoopbackAdapter::GetLoopbackEnabled()
{
    return CVarO3DSBroadcastLoopback.GetValueOnAnyThread();
}
#endif

void UO3DSBroadcastLoopbackAdapter::EnsureHooked()
{
#if WITH_EDITOR
    if (GetLoopbackEnabled() == 0)
    {
        return;
    }

    if (!BroadcastComponent.IsValid())
    {
        BroadcastComponent = GetOwner() ? GetOwner()->FindComponentByClass<UO3DSBroadcastComponent>() : nullptr;
        if (!BroadcastComponent.IsValid())
        {
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[Loopback] No UO3DSBroadcastComponent found on %s"), *GetNameSafe(GetOwner()));
            return;
        }
    }

    EnsureConsumer();

    if (!SerializedFrameHandle.IsValid())
    {
        SerializedFrameHandle = BroadcastComponent->OnSerializedFrame.AddUObject(
            this, &UO3DSBroadcastLoopbackAdapter::OnSerializedFrameReceived);
        UE_LOG(LogO3DSBroadcast, Log, TEXT("[Loopback] Hooked serialized frame delegate on %s"), *GetNameSafe(GetOwner()));
    }
#endif
}

void UO3DSBroadcastLoopbackAdapter::Unhook()
{
#if WITH_EDITOR
    if (BroadcastComponent.IsValid() && SerializedFrameHandle.IsValid())
    {
        BroadcastComponent->OnSerializedFrame.Remove(SerializedFrameHandle);
        SerializedFrameHandle.Reset();
    }
    BroadcastComponent.Reset();
    Consumer.Reset();
#endif
}

void UO3DSBroadcastLoopbackAdapter::EnsureConsumer()
{
#if WITH_EDITOR
    if (Consumer.IsValid())
    {
        return;
    }
    Consumer = FSerializedFrameConsumerRegistry::Create();
    if (!Consumer.IsValid())
    {
        UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[Loopback] No registered frame consumer; loopback disabled."));
    }
#endif
}

void UO3DSBroadcastLoopbackAdapter::OnSerializedFrameReceived(const FString& Subject, const TArray<uint8>& Buffer, double /*Timestamp*/)
{
#if WITH_EDITOR
    if (!Consumer.IsValid())
    {
        return;
    }
    UE_LOG(LogO3DSBroadcast, VeryVerbose, TEXT("[Loopback] Forwarding serialized frame for %s (%d bytes)"), *Subject, Buffer.Num());
    Consumer->SubmitFrame(Subject, Buffer, 0.0);
#endif
}
