// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "IBroadcastTransport.h"
#include "Open3DWebRTCDataChannel.h"
#include "O3DSUnifiedMessage.h" // Unified header for audio multiplexing
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

// Debug: optional built-in sine tone generator to validate audio receive path over DataChannel
static TAutoConsoleVariable<int32> CVarO3DSWebRtcDebugToneEnable(
    TEXT("o3ds.Broadcast.WebRTC.DebugToneEnable"),
    0,
    TEXT("Enable sending a synthetic PCM16 sine tone over WebRTC DataChannel using the O3DA header (0/1)."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarO3DSWebRtcDebugToneFreq(
    TEXT("o3ds.Broadcast.WebRTC.DebugToneFreq"),
    440.0f,
    TEXT("Debug tone frequency in Hz."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarO3DSWebRtcDebugToneLevel(
    TEXT("o3ds.Broadcast.WebRTC.DebugToneLevel"),
    0.25f,
    TEXT("Debug tone level (0..1)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSWebRtcDebugToneChannels(
    TEXT("o3ds.Broadcast.WebRTC.DebugToneChannels"),
    1,
    TEXT("Debug tone channels (1=mono, 2=stereo)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSWebRtcDebugToneSampleRate(
    TEXT("o3ds.Broadcast.WebRTC.DebugToneSampleRate"),
    48000,
    TEXT("Debug tone sample rate in Hz."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSWebRtcDebugToneFrameMs(
    TEXT("o3ds.Broadcast.WebRTC.DebugToneFrameMs"),
    20,
    TEXT("Debug tone frame duration in milliseconds (typical 10 or 20)."),
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

            // Debug tone generator: send PCM16 frames with O3DA header over DataChannel
            if (CVarO3DSWebRtcDebugToneEnable->GetInt() != 0 && Channel->IsOpen())
            {
                SendDebugToneIfDue();
            }
        }
    }

    virtual const FCounters& GetCounters() const override { return Counters; }

private:
    void SendDebugToneIfDue()
    {
        const int32 SampleRate = FMath::Max(8000, CVarO3DSWebRtcDebugToneSampleRate->GetInt());
        const int32 Channels = FMath::Clamp(CVarO3DSWebRtcDebugToneChannels->GetInt(), 1, 2);
        const int32 FrameMs = FMath::Clamp(CVarO3DSWebRtcDebugToneFrameMs->GetInt(), 5, 60);
        const float Freq = FMath::Max(20.0f, CVarO3DSWebRtcDebugToneFreq->GetFloat());
        const float Level = FMath::Clamp(CVarO3DSWebRtcDebugToneLevel->GetFloat(), 0.0f, 1.0f);

        const double Now = FPlatformTime::Seconds();
        const double FrameDurSec = FrameMs / 1000.0;
        if (LastDebugToneTime > 0.0 && (Now - LastDebugToneTime) < FrameDurSec)
        {
            return; // Not yet time for next frame
        }

        const int32 Frames = (int32)FMath::RoundToInt((double)SampleRate * FrameDurSec);
        const int32 Samples = Frames * Channels;

        // Generate interleaved PCM16
        TArray<int16> PcmSamples;
        PcmSamples.SetNumUninitialized(Samples);
        const double TwoPiF = 2.0 * PI * (double)Freq;
        for (int32 i = 0; i < Frames; ++i)
        {
            const double s = FMath::Sin((float)(TwoPiF * (DebugToneFrameIndex / (double)SampleRate)));
            const int16 v = (int16)FMath::Clamp(s * Level * 32767.0, -32768.0, 32767.0);
            for (int32 ch = 0; ch < Channels; ++ch)
            {
                PcmSamples[i * Channels + ch] = v;
            }
            ++DebugToneFrameIndex;
        }

        // Build minimal metadata: [LabelLen][Label][SubjectLen][Subject]
        const ANSICHAR* LabelAnsi = "o3ds:mix"; // 8 bytes label
        const uint8 LabelLen = 8;
        const uint8 SubjectLen = 0;

        TArray<uint8> Payload;
        Payload.Reserve(2 + LabelLen + (Samples * sizeof(int16)));
        Payload.Add(LabelLen);
        Payload.Append(reinterpret_cast<const uint8*>(LabelAnsi), LabelLen);
        Payload.Add(SubjectLen);
        Payload.Append(reinterpret_cast<const uint8*>(PcmSamples.GetData()), PcmSamples.Num() * sizeof(int16));

        // Pack O3DA header (big-endian fields)
        const uint32 Magic = 0x4F334441u; // 'O3DA'
        const uint8 Version = 1;
        const uint8 Kind = static_cast<uint8>(O3DS::EUnifiedKind::Audio);
        const uint8 Codec = static_cast<uint8>(O3DS::EUnifiedCodec::PCM16);
        const uint8 Flags = 0;
        const uint64 TsUs = (uint64)(Now * 1000000.0);
        const uint32 PayloadSize = (uint32)Payload.Num();

        TArray<uint8> Buf;
        Buf.Reserve(20 + Payload.Num()); // fixed 20-byte wire header
        // Write 4-byte magic BE
        Buf.Add((uint8)((Magic >> 24) & 0xFF));
        Buf.Add((uint8)((Magic >> 16) & 0xFF));
        Buf.Add((uint8)((Magic >> 8) & 0xFF));
        Buf.Add((uint8)(Magic & 0xFF));
        // Version/Kind/Codec/Flags
        Buf.Add(Version);
        Buf.Add(Kind);
        Buf.Add(Codec);
        Buf.Add(Flags);
        // TimestampUs BE
        for (int i = 7; i >= 0; --i)
        {
            Buf.Add((uint8)((TsUs >> (i * 8)) & 0xFF));
        }
        // PayloadSize BE
        Buf.Add((uint8)((PayloadSize >> 24) & 0xFF));
        Buf.Add((uint8)((PayloadSize >> 16) & 0xFF));
        Buf.Add((uint8)((PayloadSize >> 8) & 0xFF));
        Buf.Add((uint8)(PayloadSize & 0xFF));

        // Append payload
        Buf.Append(Payload);

        const bool bOk = Channel->Send(Buf.GetData(), Buf.Num());
        if (bOk && CVarO3DSWebRtcTransportDebug->GetInt() != 0)
        {
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[WebRTC] DebugTone sent: %d bytes (%d frames, %d ch)"), Buf.Num(), Frames, Channels);
        }
        LastDebugToneTime = Now;
    }
    FString Url, Key, Protocol;
    TUniquePtr<FO3DSWebRTCDataChannel> Channel;
    FCounters Counters;
    double LastStateLogTime = 0.0;
    double LastPingTime = 0.0;
    double LastDebugToneTime = 0.0;
    int64 DebugToneFrameIndex = 0;

    // Backend selection (LibDataChannel or LiveKit)
    EO3DSWebRtcBackend Backend = EO3DSWebRtcBackend::LibDataChannel;

    // Stored audio config for future native audio track support
    FO3DSWebRTCAudioConfig AudioConfig;
};
