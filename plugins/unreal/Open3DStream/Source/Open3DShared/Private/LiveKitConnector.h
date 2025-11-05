#pragma once

#include "IWebRTCConnector.h"

// LiveKit-backed connector using the livekit_ffi C ABI.
class FLiveKitConnector : public IWebRTCConnector
{
public:
    // IWebRTCConnector
    virtual bool Start(const FO3DSWebRtcConfig& InConfig) override;
    virtual void Stop() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool IsOpen() const override;

    virtual bool Send(const uint8* Data, int32 NumBytes) override;
    virtual bool SendEx(const uint8* Data, int32 NumBytes, EO3DSReliability Mode) override;
    virtual bool EnableAudioSend(bool bEnable) override;
    virtual bool SendAudioPcm16(const int16* Samples, int32 NumSamples, int32 SampleRate, int32 NumChannels) override;

    virtual FO3DSOnWebRtcState& OnState() override { return StateDelegate; }
    virtual FO3DSOnWebRtcData& OnData() override { return DataDelegate; }
    virtual FO3DSOnWebRtcRtp&  OnRemoteAudioRtp() override { return RtpDelegate; }
    virtual FO3DSOnWebRtcPcm16* OnRemoteAudioPcm() override { return &PcmDelegate; }

    // Token support (LiveKit requires a token)
    virtual bool SupportsToken() const override { return true; }
    virtual const TCHAR* TokenFieldHint() const override { return TEXT("client token"); }

private:
    // FFI bridge static callbacks
    struct FCallbacks;

private:
    FO3DSWebRtcConfig Config;
    bool bStarted = false;
    bool bOpen = false;
    bool bAudioSendEnabled = false;

    // Opaque FFI client handle; defined in livekit_ffi.h. Kept as void* to avoid header include here.
    void* ClientHandle = nullptr;

    FO3DSOnWebRtcState StateDelegate;
    FO3DSOnWebRtcData DataDelegate;
    FO3DSOnWebRtcRtp  RtpDelegate;
    FO3DSOnWebRtcPcm16 PcmDelegate;
};
