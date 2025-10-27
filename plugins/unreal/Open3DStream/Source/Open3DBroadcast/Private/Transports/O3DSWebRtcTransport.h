// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "IBroadcastTransport.h"
#include "Open3DWebRTCDataChannel.h"
#include "Open3DStreamSourceSettings.h" // for EO3DSWebRtcBackendReceiver enum

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

// Temporary audio configuration while migrating to libwebrtc native audio tracks
struct FO3DSWebRTCAudioConfig
{
    bool bEnable = false;
    FString DeviceHint;    // substring filter for device name (optional)
    int32 SampleRate = 48000;
    int32 NumChannels = 1; // 1=mono, 2=stereo
    int32 BitrateKbps = 32;
    int32 PlayoutDelayMs = 0; // extra receiver-side buffering target
};

// WebRTC DataChannel transport (optional/beta). Uses libdatachannel via Open3DStream wrapper.
class FO3DSWebRtcTransport : public IBroadcastTransport
{
public:
    FO3DSWebRtcTransport() = default;
    explicit FO3DSWebRtcTransport(EO3DSWebRtcBackend InBackend) : Backend(InBackend) {}
    virtual ~FO3DSWebRtcTransport() override { Stop(); }

    // Configure (future) native audio track parameters prior to Start.
    // Note: Currently a no-op placeholder until libwebrtc migration lands.
    void SetAudioConfig(const FO3DSWebRTCAudioConfig& In)
    {
        AudioConfig = In;
    }

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

        // Map broadcast backend enum to receiver enum for the shared data channel API
        auto ToReceiverBackend = [](EO3DSWebRtcBackend In){
            switch (In)
            {
            case EO3DSWebRtcBackend::LibDataChannel: return EO3DSWebRtcBackendReceiver::LibDataChannel;
            case EO3DSWebRtcBackend::LiveKit: return EO3DSWebRtcBackendReceiver::LiveKit;
            default: return EO3DSWebRtcBackendReceiver::LibDataChannel;
            }
        };
        const EO3DSWebRtcBackendReceiver ReceiverBackend = ToReceiverBackend(Backend);
        const bool bStarted = Channel->Start(EffectiveUrl, ReceiverBackend);
        if (CVarO3DSWebRtcTransportDebug->GetInt() != 0)
        {
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[WebRTC] Transport Start url=%s backend=%d result=%s"), 
                *EffectiveUrl, (int)Backend, bStarted?TEXT("true"):TEXT("false"));
        }
        if (!bStarted)
        {
            return false;
        }
        LastStateLogTime = 0.0;
        LastPingTime = 0.0;

        // Warn that audio config is not yet active until libwebrtc-based audio track support lands
        if (AudioConfig.bEnable)
        {
            UE_LOG(LogO3DSBroadcast, Warning, TEXT("[WebRTC] Native audio track requested (device='%s', %d Hz, %d ch, %d kbps, delay=%d ms) but not yet implemented in this transport. Audio will be silent."),
                *AudioConfig.DeviceHint, AudioConfig.SampleRate, AudioConfig.NumChannels, AudioConfig.BitrateKbps, AudioConfig.PlayoutDelayMs);
        }
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
                UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[WebRTC] Transport Send(%d) skipped bHas=%d bOpen=%d"), Size, bHas?1:0, bOpen?1:0);
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
                UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[WebRTC] Transport Send OK (%d bytes)"), Size);
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
                    UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[WebRTC] State connected=%d open=%d"), bConn?1:0, bOpen?1:0);
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
                    UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[WebRTC] DebugPing sent (4 bytes)"));
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

    // Backend selection (LibDataChannel or LiveKit)
    EO3DSWebRtcBackend Backend = EO3DSWebRtcBackend::LibDataChannel;

    // Stored audio config for future native audio track support
    FO3DSWebRTCAudioConfig AudioConfig;
};
