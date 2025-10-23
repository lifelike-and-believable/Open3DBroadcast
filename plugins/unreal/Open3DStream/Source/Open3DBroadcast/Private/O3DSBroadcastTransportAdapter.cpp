// Copyright (c) Open3DStream Contributors

#include "O3DSBroadcastTransportAdapter.h"
#include "O3DSBroadcastComponent.h"
#include "O3DSBroadcastSerializer.h"
#include "Open3DBroadcast.h"
#include "IBroadcastTransport.h"
#include "HAL/IConsoleManager.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectBaseUtility.h"
#include "Transports/O3DSTcpTransport.h"
#include "Transports/O3DSUdpTransport.h"
#include "Transports/O3DSTcpServerTransport.h"
#include "Transports/O3DSNngTransport.h"
#include "Transports/O3DSWebRtcTransport.h"

static FString EnsureWebRtcRoleInUrl(const FString& InUrl, EO3DSTransportKind Kind)
{
    if (Kind != EO3DSTransportKind::WebRTCClient && Kind != EO3DSTransportKind::WebRTCServer)
    {
        return InUrl; // leave as-is
    }
    if (InUrl.Contains(TEXT("role=")))
    {
        return InUrl;
    }
    FString Out = InUrl;
    const TCHAR* RoleStr = (Kind == EO3DSTransportKind::WebRTCClient) ? TEXT("client") : TEXT("server");
    if (Out.Contains(TEXT("?")))
    {
        Out += FString::Printf(TEXT("&role=%s"), RoleStr);
    }
    else
    {
        Out += FString::Printf(TEXT("?role=%s"), RoleStr);
    }
    return Out;
}

// Helper to inject URL query parameters based on family/mode selection
static FString InjectModeIntoUrl(const FString& InUrl, EO3DSTransportFamily Family, EO3DSNngMode NngMode, EO3DSWebRtcMode WebRtcMode)
{
    FString Out = InUrl;

    if (Family == EO3DSTransportFamily::NNG)
    {
        // Only inject if not already present
        if (!Out.Contains(TEXT("mode=")))
        {
            FString ModeParam;
            switch (NngMode)
            {
                case EO3DSNngMode::Publisher:
                    ModeParam = TEXT("mode=pub");
                    break;
                case EO3DSNngMode::PairClient:
                    ModeParam = TEXT("mode=pair&role=client");
                    break;
                case EO3DSNngMode::PairServer:
                    ModeParam = TEXT("mode=pair&role=server");
                    break;
                case EO3DSNngMode::Push:
                    ModeParam = TEXT("mode=push");
                    break;
            }
            if (!ModeParam.IsEmpty())
            {
                if (Out.Contains(TEXT("?")))
                {
                    Out += TEXT("&") + ModeParam;
                }
                else
                {
                    Out += TEXT("?") + ModeParam;
                }
            }
        }
    }
    else if (Family == EO3DSTransportFamily::WebRTC)
    {
        // Inject role if not present
        if (!Out.Contains(TEXT("role=")))
        {
            const TCHAR* RoleStr = (WebRtcMode == EO3DSWebRtcMode::Client) ? TEXT("client") : TEXT("server");
            if (Out.Contains(TEXT("?")))
            {
                Out += FString::Printf(TEXT("&role=%s"), RoleStr);
            }
            else
            {
                Out += FString::Printf(TEXT("?role=%s"), RoleStr);
            }
        }
    }

    return Out;
}

// Derive a legacy-like protocol string to pass to transports
static FString GetProtocolNameLegacy(EO3DSTransportFamily Family, EO3DSTcpMode TcpMode, EO3DSWebRtcMode WebRtcMode)
{
    switch (Family)
    {
        case EO3DSTransportFamily::TCP:
            return (TcpMode == EO3DSTcpMode::Server)
                ? UEnum::GetValueAsString(EO3DSTransportKind::TCPServer)
                : UEnum::GetValueAsString(EO3DSTransportKind::TCP);
        case EO3DSTransportFamily::UDP:
            return UEnum::GetValueAsString(EO3DSTransportKind::UDP);
        case EO3DSTransportFamily::NNG:
            return UEnum::GetValueAsString(EO3DSTransportKind::NNG);
        case EO3DSTransportFamily::WebRTC:
            return (WebRtcMode == EO3DSWebRtcMode::Client)
                ? UEnum::GetValueAsString(EO3DSTransportKind::WebRTCClient)
                : UEnum::GetValueAsString(EO3DSTransportKind::WebRTCServer);
        default:
            break;
    }
    return TEXT("EO3DSTransportKind::Disabled");
}

// CVars (runtime overrides) - file scoped to avoid template/macro conflicts in some builds
static TAutoConsoleVariable<int32> CVarO3DSBroadcastAdapterEnable(
    TEXT("o3ds.Broadcast.Enable"), 0,
    TEXT("Enable broadcast transport adapter (0/1)."), ECVF_Default);

static TAutoConsoleVariable<FString> CVarO3DSBroadcastAdapterUrl(
    TEXT("o3ds.Broadcast.Url"), TEXT("") ,
    TEXT("Override broadcast URL endpoint."), ECVF_Default);

static TAutoConsoleVariable<FString> CVarO3DSBroadcastAdapterKey(
    TEXT("o3ds.Broadcast.Key"), TEXT("") ,
    TEXT("Override broadcast session key."), ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSBroadcastAdapterMaxBytes(
    TEXT("o3ds.Broadcast.MaxQueuedBytes"), 0,
    TEXT("Override max queued bytes before dropping frames (0=use setting)."), ECVF_Default);

static TArray<UO3DSBroadcastTransportAdapter*> GTransportAdapters;

UO3DSBroadcastTransportAdapter::UO3DSBroadcastTransportAdapter()
{
    PrimaryComponentTick.bCanEverTick = true;
    SetComponentTickEnabled(false);
}

void UO3DSBroadcastTransportAdapter::BeginPlay()
{
    Super::BeginPlay();

    GTransportAdapters.AddUnique(this);

    static bool bRegisteredCmd = false;
    if (!bRegisteredCmd)
    {
        IConsoleManager::Get().RegisterConsoleCommand(
            TEXT("o3ds.Broadcast.Transport.DumpStats"),
            TEXT("Dump transport adapter stats for all instances"),
            FConsoleCommandDelegate::CreateStatic(&UO3DSBroadcastTransportAdapter::DumpAllStats),
            ECVF_Default);

        IConsoleManager::Get().RegisterConsoleCommand(
            TEXT("o3ds.Broadcast.Transport.DumpTransportStats"),
            TEXT("Dump transport-level counters for all active transports"),
            FConsoleCommandDelegate::CreateLambda([]()
            {
                UE_LOG(LogO3DSBroadcast, Display, TEXT("---- O3DS Transport Counters ----"));
                for (const UO3DSBroadcastTransportAdapter* A : GTransportAdapters)
                {
                    if (!A || !A->TransportImpl) continue;
                    const IBroadcastTransport::FCounters& C = A->TransportImpl->GetCounters();
                    const bool bConn = A->TransportImpl->IsConnected();
                    UE_LOG(LogO3DSBroadcast, Display, TEXT("%s: Connected=%s FramesSent=%llu BytesSent=%llu Dropped=%llu Reconnects=%llu"),
                        *GetNameSafe(A->GetOwner()), bConn?TEXT("true"):TEXT("false"),
                        (unsigned long long)C.FramesSent.Load(),
                        (unsigned long long)C.BytesSent.Load(),
                        (unsigned long long)C.FramesDropped.Load(),
                        (unsigned long long)C.Reconnects.Load());
                }
            }),
            ECVF_Default);

        bRegisteredCmd = true;
    }

    EnsureBound();
}

void UO3DSBroadcastTransportAdapter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Unbind();
    if (TransportImpl)
    {
        TransportImpl->Stop();
        TransportImpl.Reset();
    }

    GTransportAdapters.Remove(this);

    Super::EndPlay(EndPlayReason);
}

void UO3DSBroadcastTransportAdapter::Unbind()
{
    if (BroadcastComponent.IsValid() && SerializedHandle.IsValid())
    {
        BroadcastComponent->OnSerializedFrame.Remove(SerializedHandle);
        SerializedHandle.Reset();
    }
    SetComponentTickEnabled(false);
}

void UO3DSBroadcastTransportAdapter::EnsureBound()
{
    // Require explicit enable (or runtime override) to avoid unintended localhost broadcasts
    const bool bExplicitEnable = bEnable || (CVarO3DSBroadcastAdapterEnable.GetValueOnGameThread() > 0);
    if (!bExplicitEnable)
    {
        UE_LOG(LogO3DSBroadcast, Log, TEXT("Transport adapter disabled (toggle bEnable or set o3ds.Broadcast.Enable=1)"));
        return;
    }

    if (!BroadcastComponent.IsValid())
    {
        BroadcastComponent = GetOwner() ? GetOwner()->FindComponentByClass<UO3DSBroadcastComponent>() : nullptr;
        if (!BroadcastComponent.IsValid())
        {
            UE_LOG(LogO3DSBroadcast, Warning, TEXT("Transport adapter: no UO3DSBroadcastComponent found on %s"), *GetNameSafe(GetOwner()));
            return;
        }
    }

    if (!TransportImpl)
    {
        TransportImpl = CreateTransport();
        if (!TransportImpl)
        {
            UE_LOG(LogO3DSBroadcast, Warning, TEXT("Transport adapter: no transport impl for selection"));
            return;
        }

        FString EffectiveUrl = Url;
        const FString UrlOverride = CVarO3DSBroadcastAdapterUrl.GetValueOnGameThread();
        if (!UrlOverride.IsEmpty()) { EffectiveUrl = UrlOverride; }
        FString EffectiveKey = Key;
        const FString KeyOverride = CVarO3DSBroadcastAdapterKey.GetValueOnGameThread();
        if (!KeyOverride.IsEmpty()) { EffectiveKey = KeyOverride; }

        const int32 CVarMax = CVarO3DSBroadcastAdapterMaxBytes.GetValueOnGameThread();
        if (CVarMax > 0) { MaxQueuedBytes = CVarMax; }

        // Inject role= when using explicit WebRTC kinds and URL lacks a role (legacy path)
        EffectiveUrl = EnsureWebRtcRoleInUrl(EffectiveUrl, Transport);

        // If using new family/mode UX and Transport is Disabled, inject mode parameters
        if (Transport == EO3DSTransportKind::Disabled)
        {
            EffectiveUrl = InjectModeIntoUrl(EffectiveUrl, TransportFamily, NngMode, WebRtcMode);
        }

        // Derive protocol name compatible with legacy expectations
        FString ProtocolName;
        if (Transport != EO3DSTransportKind::Disabled)
        {
            ProtocolName = UEnum::GetValueAsString(Transport);
        }
        else
        {
            ProtocolName = GetProtocolNameLegacy(TransportFamily, TcpMode, WebRtcMode);
        }

        if (!TransportImpl->Start(EffectiveUrl, ProtocolName, EffectiveKey))
        {
            UE_LOG(LogO3DSBroadcast, Warning, TEXT("Transport failed to start: %s %s"), *ProtocolName, *EffectiveUrl);
        }
        else
        {
            UE_LOG(LogO3DSBroadcast, Log, TEXT("Transport started: %s %s"), *ProtocolName, *EffectiveUrl);
        }
    }

    if (!SerializedHandle.IsValid())
    {
        SerializedHandle = BroadcastComponent->OnSerializedFrame.AddUObject(this, &UO3DSBroadcastTransportAdapter::OnSerializedFrame);
        SetComponentTickEnabled(true);
        UE_LOG(LogO3DSBroadcast, Log, TEXT("Transport adapter bound to broadcast component"));
    }
}

TUniquePtr<IBroadcastTransport> UO3DSBroadcastTransportAdapter::CreateTransport() const
{
    // Prefer legacy Transport enum if set to non-Disabled
    if (Transport != EO3DSTransportKind::Disabled)
    {
        switch (Transport)
        {
            case EO3DSTransportKind::TCP:
                return MakeUnique<FO3DSTcpTransport>();
            case EO3DSTransportKind::TCPServer:
                return MakeUnique<FO3DSTcpServerTransport>();
            case EO3DSTransportKind::UDP:
                return MakeUnique<FO3DSUdpTransport>();
            case EO3DSTransportKind::NNG:
                return MakeUnique<FO3DSNngTransport>();
            case EO3DSTransportKind::WebRTCClient:
            case EO3DSTransportKind::WebRTCServer:
                return MakeUnique<FO3DSWebRtcTransport>();
            default:
                break;
        }
    }

    // New family/mode UX
    switch (TransportFamily)
    {
        case EO3DSTransportFamily::NNG:
            return MakeUnique<FO3DSNngTransport>();
        case EO3DSTransportFamily::TCP:
            if (TcpMode == EO3DSTcpMode::Server)
            {
                return MakeUnique<FO3DSTcpServerTransport>();
            }
            return MakeUnique<FO3DSTcpTransport>();
        case EO3DSTransportFamily::UDP:
            return MakeUnique<FO3DSUdpTransport>();
        case EO3DSTransportFamily::WebRTC:
            return MakeUnique<FO3DSWebRtcTransport>();
        default:
            break;
    }

    return TUniquePtr<IBroadcastTransport>();
}

void UO3DSBroadcastTransportAdapter::OnSerializedFrame(const FString& /*Subject*/, const TArray<uint8>& Buffer, double Timestamp)
{
    const uint64 NewQueued = QueuedBytes.Load() + (uint64)Buffer.Num();
    if (MaxQueuedBytes > 0 && NewQueued > (uint64)MaxQueuedBytes)
    {
        DroppedFrames.Store(DroppedFrames.Load() + 1);
        return;
    }

    FItem Item;
    Item.Data = Buffer; // copy
    Item.Ts = Timestamp;
    Queue.Enqueue(MoveTemp(Item));
    QueuedBytes.Store(NewQueued);
    EnqueuedFrames.Store(EnqueuedFrames.Load() + 1);
}

void UO3DSBroadcastTransportAdapter::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    // Drive transport maintenance (accept/connect retries)
    if (TransportImpl)
    {
        TransportImpl->Tick(DeltaTime);
    }

    // Drain a bounded number per tick to avoid long stalls
    constexpr int32 MaxPerTick = 32;
    int32 Count = 0;
    while (Count < MaxPerTick)
    {
        FItem Item;
        if (!Queue.Dequeue(Item))
        {
            break;
        }
        QueuedBytes.Store(QueuedBytes.Load() - (uint64)Item.Data.Num());
        if (TransportImpl)
        {
            TransportImpl->Send(Item.Data.GetData(), Item.Data.Num(), Item.Ts);
        }
        ++Count;
    }
}

void UO3DSBroadcastTransportAdapter::DumpStatsInstance() const
{
    const uint64 QBytes = QueuedBytes.Load();
    const uint64 Enq = EnqueuedFrames.Load();
    const uint64 Drop = DroppedFrames.Load();
    UE_LOG(LogO3DSBroadcast, Display, TEXT("TransportAdapter on %s: QueuedBytes=%llu Enqueued=%llu Dropped=%llu"),
        *GetNameSafe(GetOwner()), (unsigned long long)QBytes, (unsigned long long)Enq, (unsigned long long)Drop);
}

void UO3DSBroadcastTransportAdapter::DumpAllStats()
{
    UE_LOG(LogO3DSBroadcast, Display, TEXT("---- O3DS Broadcast Transport Adapter Stats ----"));
    if (GTransportAdapters.Num() == 0)
    {
        UE_LOG(LogO3DSBroadcast, Display, TEXT("(no active transport adapters)"));
        return;
    }
    for (const UO3DSBroadcastTransportAdapter* A : GTransportAdapters)
    {
        if (A)
        {
            A->DumpStatsInstance();
        }
    }
}
