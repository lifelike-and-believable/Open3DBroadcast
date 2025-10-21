// Copyright (c) Open3DStream Contributors

#include "O3DSBroadcastLoopbackAdapter.h"
#include "O3DSBroadcastComponent.h"
#include "Open3DBroadcast.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Open3DStreamSource.h"
#include "ILiveLinkClient.h"
#include "Open3DStreamSourceSettings.h"
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

    EnsureLiveLinkSource();

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
    Source.Reset();
#endif
}

void UO3DSBroadcastLoopbackAdapter::EnsureLiveLinkSource()
{
#if WITH_EDITOR
    if (Source.IsValid())
    {
        return;
    }

    // Create an in-memory source without sockets; we will call OnPackage directly
    FOpen3DStreamSettings Settings = GetDefault<UOpen3DStreamSettingsObject>()->Settings;
    Settings.Protocol = FText::FromString(TEXT("InMemory"));
    Settings.Url = FText::FromString(TEXT("mem://loopback"));

    Source = MakeShared<FOpen3DStreamSource>(Settings);

    // Acquire LiveLink client via modular features and register the source properly
    if (IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        ILiveLinkClient* LiveLinkClient = &IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
        if (LiveLinkClient)
        {
            LiveLinkClient->AddSource(Source.ToSharedRef());
            UE_LOG(LogO3DSBroadcast, Log, TEXT("[Loopback] In-memory LiveLink source added"));
        }
    }
    else
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("[Loopback] LiveLink client not available"));
    }
#endif
}

void UO3DSBroadcastLoopbackAdapter::OnSerializedFrameReceived(const FString& Subject, const TArray<uint8>& Buffer, double /*Timestamp*/)
{
#if WITH_EDITOR
    if (!Source.IsValid())
    {
        return;
    }
    UE_LOG(LogO3DSBroadcast, VeryVerbose, TEXT("[Loopback] Forwarding serialized frame for %s (%d bytes)"), *Subject, Buffer.Num());
    // Directly feed the serialized bytes into the receiver parsing path
    Source->OnPackage(Buffer);
#endif
}
