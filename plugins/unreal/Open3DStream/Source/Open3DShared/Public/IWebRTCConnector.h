#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

// Minimal, UE-friendly contract for backend-agnostic WebRTC connectors.
// No UE reflection required at this layer.

// Backend selector (runtime)
enum class EO3DSWebRtcBackend : uint8
{
    LibDataChannel = 0,
    LiveKit = 1,
};

// Connection role
enum class EO3DSWebRtcRole : uint8
{
    Client = 0,
    Server = 1,
};

// Runtime configuration for a connector instance
struct FO3DSWebRtcConfig
{
    EO3DSWebRtcBackend Backend = EO3DSWebRtcBackend::LibDataChannel;
    EO3DSWebRtcRole    Role    = EO3DSWebRtcRole::Client;

    FString SignalingUrl;   // ws:// or wss:// endpoint
    FString Token;          // optional bearer/room token
    FString Room;           // optional room name

    bool    bEnableAudio    = false;
    int32   SampleRate      = 48000;
    int32   NumChannels     = 1;
    int32   BitrateKbps     = 64; // nominal OPUS target
    FString AudioDeviceName;      // optional
    FString SubmixName;           // optional

    // Optional: synthesize a simple sine tone and send as RTP payloads (debug only)
    bool    bSendDebugTone  = false;
    float   ToneHz          = 440.f;
    double  ToneDurationSec = 1.0;

    bool    bVerbose        = false;
};

// Delegates for state and data callbacks
DECLARE_MULTICAST_DELEGATE_TwoParams(FO3DSOnWebRtcState, const FString& /*State*/, bool /*bIsError*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FO3DSOnWebRtcData, const TArray<uint8>& /*Bytes*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FO3DSOnWebRtcRtp, const TArray<uint8>& /*RtpBytes*/);

class OPEN3DSHARED_API IWebRTCConnector : public TSharedFromThis<IWebRTCConnector>
{
public:
    virtual ~IWebRTCConnector() {}

    // Lifecycle
    virtual bool Start(const FO3DSWebRtcConfig& Config) = 0;
    virtual void Stop() = 0;
    virtual void Tick(float DeltaSeconds) = 0; // never block; pump events only
    virtual bool IsOpen() const = 0;

    // Data channel send (reliable/unordered by default)
    virtual bool Send(const uint8* Data, int32 NumBytes) = 0;

    // Audio (negotiation is done internally; this flag switches send path on/off)
    virtual bool EnableAudioSend(bool bEnable) = 0;

    // Optional: push raw PCM16 audio to be packetized and sent over the negotiated audio track.
    // - Samples: interleaved PCM16
    // - NumSamples: total sample count across all channels
    // - SampleRate/NumChannels: format of provided Samples; connector may resample/downmix if needed
    // Returns false if audio track not available or connector not started.
    virtual bool SendAudioPcm16(const int16* Samples, int32 NumSamples, int32 SampleRate, int32 NumChannels) = 0;

    // Delegate accessors
    virtual FO3DSOnWebRtcState& OnState() = 0;
    virtual FO3DSOnWebRtcData& OnData() = 0;
    virtual FO3DSOnWebRtcRtp&  OnRemoteAudioRtp() = 0;
};
