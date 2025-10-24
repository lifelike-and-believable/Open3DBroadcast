// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "IBroadcastTransport.h"
#include "Open3DWebRTCDataChannel.h"

// Debug cvar for transport send logging
static TAutoConsoleVariable<int32> CVarO3DSWebRtcTransportDebug(
    TEXT("o3ds.Broadcast.WebRTC.Debug"),
    1,
    TEXT("Enable debug logging for WebRTC transport start/send (0/1)."),
    ECVF_Default);

// Optional: emit a small ping every second when channel is open to validate data path
static TAutoConsoleVariable<int32> CVarO3DSWebRtcTransportDebugPing(
    TEXT("o3ds.Broadcast.WebRTC.DebugPing"),
    0,
    TEXT("When enabled, send a 4-byte heartbeat every second over the data channel to validate send path (0/1)."),
    ECVF_Default);

// WebRTC DataChannel transport (optional/beta). Uses libdatachannel via Open3DStream wrapper.
class FO3DSWebRtcTransport : public IBroadcastTransport
{
public:
    FO3DSWebRtcTransport() = default;
    virtual ~FO3DSWebRtcTransport() override { Stop(); }

    virtual bool Start(const FString& InUrl, const FString& InProtocol, const FString& InKey) override
    {
        Url = InUrl; Key = InKey; Protocol = InProtocol;
        Channel = MakeUnique<FO3DSWebRTCDataChannel>();

        // Ensure complementary WebRTC role is encoded in the URL query (client/server)
        // LiveLink receiver passes explicit role to its connector; this side must mirror via ?role=
        FString EffectiveUrl = Url;
        const bool bUrlHasRole = EffectiveUrl.Contains(TEXT("role="), ESearchCase::IgnoreCase);
        const bool bIsServer = Url.Contains(TEXT("role=server"), ESearchCase::IgnoreCase) || Protocol.Contains(TEXT("WebRTC Server")) || Protocol.Contains(TEXT("WebRTCServer"));
        if (!bUrlHasRole)
        {
            const TCHAR* RoleKV = bIsServer ? TEXT("role=server") : TEXT("role=client");
            if (EffectiveUrl.Contains(TEXT("?")))
            {
                EffectiveUrl += TEXT("&");
                EffectiveUrl += RoleKV;
            }
            else
            {
                EffectiveUrl += TEXT("?");
                EffectiveUrl += RoleKV;
            }
        }

        const bool bStarted = Channel->Start(EffectiveUrl);
        if (CVarO3DSWebRtcTransportDebug->GetInt() != 0)
        {
            UE_LOG(LogO3DSBroadcast, Log, TEXT("[WebRTC] Transport Start url=%s result=%s"), *EffectiveUrl, bStarted?TEXT("true"):TEXT("false"));
        }
        if (!bStarted)
        {
            return false;
        }
        LastStateLogTime = 0.0;
        LastPingTime = 0.0;
        return true;
    }

    virtual void Stop() override
    {
        if (Channel)
        {
            Channel->Stop();
            Channel.Reset();
        }
        LastStateLogTime = 0.0;
        LastPingTime = 0.0;
    }

    virtual bool Send(const uint8* Data, int32 Size, double /*TimestampSeconds*/) override
    {
        const bool bHas = !!Channel;
        const bool bOpen = bHas && Channel->IsOpen();
        if (!bHas || !bOpen)
        {
            if (CVarO3DSWebRtcTransportDebug->GetInt() != 0)
            {
                UE_LOG(LogO3DSBroadcast, Log, TEXT("[WebRTC] Transport Send(%d) skipped bHas=%d bOpen=%d"), Size, bHas?1:0, bOpen?1:0);
            }
            Counters.FramesDropped.Store(Counters.FramesDropped.Load() + 1);
            return false;
        }
        const bool bOk = Channel->Send(Data, Size);
        if (bOk)
        {
            Counters.FramesSent.Store(Counters.FramesSent.Load() + 1);
            Counters.BytesSent.Store(Counters.BytesSent.Load() + (uint64)Size);
            if (CVarO3DSWebRtcTransportDebug->GetInt() != 0)
            {
                UE_LOG(LogO3DSBroadcast, Log, TEXT("[WebRTC] Transport Send OK (%d bytes)"), Size);
            }
        }
        else
        {
            if (CVarO3DSWebRtcTransportDebug->GetInt() != 0)
            {
                UE_LOG(LogO3DSBroadcast, Warning, TEXT("[WebRTC] Transport Send FAILED (%d bytes)"), Size);
            }
            Counters.FramesDropped.Store(Counters.FramesDropped.Load() + 1);
        }
        return bOk;
    }

    virtual bool IsConnected() const override
    {
        return Channel && Channel->IsConnected() && Channel->IsOpen();
    }

    virtual void Tick(float DeltaTime) override
    {
        if (Channel)
        {
            Channel->Tick();
            if (CVarO3DSWebRtcTransportDebug->GetInt() != 0)
            {
                const double Now = FPlatformTime::Seconds();
                if (LastStateLogTime == 0.0 || (Now - LastStateLogTime) > 1.0)
                {
                    const bool bConn = Channel->IsConnected();
                    const bool bOpen = Channel->IsOpen();
                    UE_LOG(LogO3DSBroadcast, Log, TEXT("[WebRTC] State connected=%d open=%d"), bConn?1:0, bOpen?1:0);
                    LastStateLogTime = Now;
                }
            }
            if (CVarO3DSWebRtcTransportDebugPing->GetInt() != 0 && Channel->IsOpen())
            {
                const double Now = FPlatformTime::Seconds();
                if (LastPingTime == 0.0 || (Now - LastPingTime) > 1.0)
                {
                    const uint32 Ping = 0xDEADBEEF;
                    Channel->Send(reinterpret_cast<const uint8*>(&Ping), sizeof(Ping));
                    UE_LOG(LogO3DSBroadcast, Log, TEXT("[WebRTC] DebugPing sent (4 bytes)"));
                    LastPingTime = Now;
                }
            }
        }
    }

    virtual const FCounters& GetCounters() const override { return Counters; }

private:
    FString Url, Key, Protocol;
    TUniquePtr<FO3DSWebRTCDataChannel> Channel;
    FCounters Counters;
    double LastStateLogTime = 0.0;
    double LastPingTime = 0.0;
};
