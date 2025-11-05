// Copyright (c) Open3DStream Contributors

#include "Transports/O3DSWebRtcTransport.h"
#include "Open3DBroadcast.h"
#include "Misc/Char.h"

// Temporarily allow muting DataChannel sends to reduce log clutter during connectivity tests
static TAutoConsoleVariable<int32> CVarO3DSBroadcastMuteWebRTCData(
    TEXT("o3ds.Broadcast.MuteWebRTCData"),
    0,
    TEXT("When 1, FO3DSWebRtcTransport::Send() is a no-op (useful for connectivity tests)."),
    ECVF_Default);

namespace
{
    static FString ToLowerCopy(const FString& S)
    {
        FString Out = S;
        Out.ToLowerInline();
        return Out;
    }
}

void FO3DSWebRtcTransport::ApplyPreStartConfig(const FO3DSWebRtcConfig& InConfig)
{
    PreConfig = InConfig;
    bHasPreConfig = true;
}

bool FO3DSWebRtcTransport::ParseBoolParam(const FString& Url, const TCHAR* Key, bool& OutVal)
{
    const FString K = FString(Key) + TEXT("=");
    int32 QIdx = Url.Find(TEXT("?"));
    if (QIdx == INDEX_NONE) return false;
    const FString Query = Url.Mid(QIdx + 1);
    TArray<FString> Parts; Query.ParseIntoArray(Parts, TEXT("&"), true);
    for (const FString& P : Parts)
    {
        if (P.StartsWith(K, ESearchCase::IgnoreCase))
        {
            const FString V = ToLowerCopy(P.Mid(K.Len()));
            OutVal = (V == TEXT("1") || V == TEXT("true") || V == TEXT("yes"));
            return true;
        }
    }
    return false;
}

bool FO3DSWebRtcTransport::ParseIntParam(const FString& Url, const TCHAR* Key, int32& OutVal)
{
    FString Str;
    if (!ParseStringParam(Url, Key, Str)) return false;
    if (Str.IsEmpty()) return false;
    OutVal = FCString::Atoi(*Str);
    return true;
}

bool FO3DSWebRtcTransport::ParseFloatParam(const FString& Url, const TCHAR* Key, float& OutVal)
{
    FString Str;
    if (!ParseStringParam(Url, Key, Str)) return false;
    if (Str.IsEmpty()) return false;
    OutVal = FCString::Atof(*Str);
    return true;
}

bool FO3DSWebRtcTransport::ParseStringParam(const FString& Url, const TCHAR* Key, FString& OutVal)
{
    const FString K = FString(Key) + TEXT("=");
    int32 QIdx = Url.Find(TEXT("?"));
    if (QIdx == INDEX_NONE) return false;
    const FString Query = Url.Mid(QIdx + 1);
    TArray<FString> Parts; Query.ParseIntoArray(Parts, TEXT("&"), true);
    for (const FString& P : Parts)
    {
        if (P.StartsWith(K, ESearchCase::IgnoreCase))
        {
            OutVal = P.Mid(K.Len());
            return true;
        }
    }
    return false;
}

EO3DSWebRtcRole FO3DSWebRtcTransport::RoleFromUrlOrProtocol(const FString& Url, const FString& Protocol)
{
    FString RoleParam;
    if (ParseStringParam(Url, TEXT("role"), RoleParam))
    {
        // Accept legacy and new terms
        if (RoleParam.Equals(TEXT("server"), ESearchCase::IgnoreCase) || RoleParam.Equals(TEXT("subscriber"), ESearchCase::IgnoreCase))
        {
            return EO3DSWebRtcRole::Server;
        }
        if (RoleParam.Equals(TEXT("client"), ESearchCase::IgnoreCase) || RoleParam.Equals(TEXT("publisher"), ESearchCase::IgnoreCase))
        {
            return EO3DSWebRtcRole::Client;
        }
        // Default to Client (Publisher)
        return EO3DSWebRtcRole::Client;
    }
    const FString P = ToLowerCopy(Protocol);
    if (P.Contains(TEXT("webrtcserver")) || P.Contains(TEXT("webrtc subscriber"))) return EO3DSWebRtcRole::Server;
    return EO3DSWebRtcRole::Client;
}

bool FO3DSWebRtcTransport::Start(const FString& InUrl, const FString& InProtocol, const FString& InKey)
{
    // Build connector config
    FO3DSWebRtcConfig Cfg;
    Cfg.Backend = EO3DSWebRtcBackend::LibDataChannel;
    Cfg.Role = RoleFromUrlOrProtocol(InUrl, InProtocol);
    Cfg.SignalingUrl = InUrl; // server path + optional query are accepted by connector

    // Optional room/token from query
    FString RoomParam; if (ParseStringParam(InUrl, TEXT("room"), RoomParam)) { Cfg.Room = RoomParam; }
    FString TokenParam; if (ParseStringParam(InUrl, TEXT("token"), TokenParam)) { Cfg.Token = TokenParam; }
    // Optional backend selection (backend=livekit or backend=libdatachannel/p2p)
    FString BackendParam; if (ParseStringParam(InUrl, TEXT("backend"), BackendParam))
    {
        if (BackendParam.Equals(TEXT("livekit"), ESearchCase::IgnoreCase))
        {
            Cfg.Backend = EO3DSWebRtcBackend::LiveKit;
        }
        else if (BackendParam.Equals(TEXT("libdatachannel"), ESearchCase::IgnoreCase) || BackendParam.Equals(TEXT("p2p"), ESearchCase::IgnoreCase))
        {
            Cfg.Backend = EO3DSWebRtcBackend::LibDataChannel;
        }
    }

    // Optional verbosity and audio params from query
    bool bVerboseParam = false; if (ParseBoolParam(InUrl, TEXT("verbose"), bVerboseParam)) { Cfg.bVerbose = bVerboseParam; }
    bool bAudioParam = false; if (ParseBoolParam(InUrl, TEXT("audio"), bAudioParam)) { Cfg.bEnableAudio = bAudioParam; }
    int32 SR = 0; if (ParseIntParam(InUrl, TEXT("samplerate"), SR)) { Cfg.SampleRate = SR; }
    int32 Ch = 0; if (ParseIntParam(InUrl, TEXT("channels"), Ch)) { Cfg.NumChannels = Ch; }
    int32 BR = 0; if (ParseIntParam(InUrl, TEXT("bitrate"), BR)) { Cfg.BitrateKbps = BR; }
    FString Dev; if (ParseStringParam(InUrl, TEXT("device"), Dev)) { Cfg.AudioDeviceName = Dev; }
    FString Sub; if (ParseStringParam(InUrl, TEXT("submix"), Sub)) { Cfg.SubmixName = Sub; }
    bool bTone = false; if (ParseBoolParam(InUrl, TEXT("tone"), bTone)) { Cfg.bSendDebugTone = bTone; }
    float ToneHz = 0.f; if (ParseFloatParam(InUrl, TEXT("tonehz"), ToneHz)) { Cfg.ToneHz = ToneHz; }
    float ToneDur = 0.f; if (ParseFloatParam(InUrl, TEXT("tonedur"), ToneDur)) { Cfg.ToneDurationSec = ToneDur; }

    // Optional send reliability: reliability=lossy|reliable (default reliable)
    FString ReliabilityParam; if (ParseStringParam(InUrl, TEXT("reliability"), ReliabilityParam))
    {
        if (ReliabilityParam.Equals(TEXT("lossy"), ESearchCase::IgnoreCase))
        {
            DefaultReliability = IWebRTCConnector::EO3DSReliability::Lossy;
        }
        else
        {
            DefaultReliability = IWebRTCConnector::EO3DSReliability::Reliable;
        }
    }

    // Merge pre-config (authoritative for backend/role/room/token/audio/verbosity)
    if (bHasPreConfig)
    {
        Cfg.Backend = PreConfig.Backend;
        Cfg.Role = PreConfig.Role;
        Cfg.Room = PreConfig.Room;
        Cfg.Token = PreConfig.Token;
        Cfg.bEnableAudio = PreConfig.bEnableAudio;
        Cfg.SampleRate = PreConfig.SampleRate;
        Cfg.NumChannels = PreConfig.NumChannels;
        Cfg.BitrateKbps = PreConfig.BitrateKbps;
        Cfg.AudioDeviceName = PreConfig.AudioDeviceName;
        Cfg.SubmixName = PreConfig.SubmixName;
        Cfg.bSendDebugTone = PreConfig.bSendDebugTone;
        Cfg.ToneHz = PreConfig.ToneHz;
        Cfg.ToneDurationSec = PreConfig.ToneDurationSec;
        Cfg.bVerbose = PreConfig.bVerbose;
    }

    // Note: Any LibDataChannel-specific URL normalization (e.g., default client path)
    // is handled inside the LibDataChannelConnector to keep backend logic encapsulated.

    Connector = FWebRTCConnectorFactory::Create(Cfg.Backend);
    if (!Connector.IsValid())
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("WebRTC transport: no connector backend available"));
        return false;
    }

    // Bind delegates for state and connection tracking
    Connector->OnState().AddLambda([this](const FString& State, bool bIsError)
    {
        if (bIsError)
        {
            UE_LOG(LogO3DSBroadcast, Warning, TEXT("[WebRTC] State: %s (error)"), *State);
        }
        else if (State.Contains(TEXT("DataChannelOpen")))
        {
            UE_LOG(LogO3DSBroadcast, Log, TEXT("[WebRTC] DataChannel open"));
            // If data is muted for connectivity testing, still send one small
            // test message directly via the connector so the receiver can
            // confirm the DC is functional without flooding logs.
            if (CVarO3DSBroadcastMuteWebRTCData.GetValueOnAnyThread() != 0 && Connector.IsValid())
            {
                static const char* kTest = "o3ds:mute-test";
                const bool bSent = Connector->Send(reinterpret_cast<const uint8*>(kTest), (int32)strlen(kTest));
                UE_LOG(LogO3DSBroadcast, Log, TEXT("[WebRTC] Sent mute-test message (%s)"), bSent ? TEXT("ok") : TEXT("failed"));
            }
        }
        else if (State.Contains(TEXT("DataChannelClosed")))
        {
            UE_LOG(LogO3DSBroadcast, Log, TEXT("[WebRTC] DataChannel closed"));
        }
    });

    const bool bStarted = Connector->Start(Cfg);
    if (!bStarted)
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("WebRTC transport: connector failed to start"));
        return false;
    }

    // Enable audio send if requested
    if (Cfg.bEnableAudio)
    {
        Connector->EnableAudioSend(true);
    }

    return true;
}

void FO3DSWebRtcTransport::Stop()
{
    if (Connector.IsValid())
    {
        Connector->Stop();
        Connector.Reset();
    }
}

bool FO3DSWebRtcTransport::Send(const uint8* Data, int32 Size, double /*TimestampSeconds*/)
{
    if (CVarO3DSBroadcastMuteWebRTCData.GetValueOnAnyThread() != 0)
    {
        // Pretend success but do not send to avoid flooding logs during connectivity checks
        return true;
    }
    if (!Connector.IsValid() || !Connector->IsOpen() || !Data || Size <= 0)
    {
        Counters.FramesDropped.Store(Counters.FramesDropped.Load() + 1);
        return false;
    }
    const bool bOk = Connector->SendEx(Data, Size, DefaultReliability);
    if (bOk)
    {
        Counters.FramesSent.Store(Counters.FramesSent.Load() + 1);
        Counters.BytesSent.Store(Counters.BytesSent.Load() + (uint64)Size);
    }
    else
    {
        Counters.FramesDropped.Store(Counters.FramesDropped.Load() + 1);
    }
    return bOk;
}

bool FO3DSWebRtcTransport::IsConnected() const
{
    return Connector.IsValid() && Connector->IsOpen();
}

void FO3DSWebRtcTransport::Tick(float DeltaTime)
{
    if (Connector.IsValid())
    {
        Connector->Tick(DeltaTime);
    }
}
