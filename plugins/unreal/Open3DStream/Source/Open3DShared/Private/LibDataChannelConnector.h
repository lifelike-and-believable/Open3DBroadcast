#pragma once

#include "CoreMinimal.h"
#include "IWebRTCConnector.h"

#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "WebSocketsModule.h"

#include <rtc/rtc.hpp>
#include <memory>
#include <atomic>

// LibDataChannel-backed implementation of IWebRTCConnector.
// Signaling: Unreal WebSockets, JSON messages compatible with sample components.

class FLibDataChannelConnector : public IWebRTCConnector
{
public:
    FLibDataChannelConnector() = default;
    virtual ~FLibDataChannelConnector() override { Stop(); }

    // IWebRTCConnector
    virtual bool Start(const FO3DSWebRtcConfig& InConfig) override;
    virtual void Stop() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool IsOpen() const override { return bOpen.load(); }
    virtual bool Send(const uint8* Data, int32 NumBytes) override;
    virtual bool EnableAudioSend(bool bEnable) override;

    virtual FO3DSOnWebRtcState& OnState() override { return StateDelegate; }
    virtual FO3DSOnWebRtcData& OnData() override { return DataDelegate; }
    virtual FO3DSOnWebRtcRtp&  OnRemoteAudioRtp() override { return RtpDelegate; }

private:
    // Helpers
    static std::string ToStd(const FString& S);
    static FString     ToFStr(const std::string& S);

    void OpenWebSocket();
    void SetupPeerConnection(const rtc::Configuration& Cfg);
    void CreateClientMediaAndDC();
    void AttachRecvAudioCallbacks(const std::shared_ptr<rtc::Track>& Track);

    void SendJson(const TSharedPtr<class FJsonObject>& Obj);
    void HandleIncomingJson(const FString& JsonStr);

private:
    FO3DSWebRtcConfig Config;
    std::atomic<bool> bStarted{false};
    std::atomic<bool> bOpen{false}; // DataChannel open
    std::atomic<bool> bAudioOpen{false};

    // Signaling
    TSharedPtr<IWebSocket> WS;
    FString RemotePeerId; // used by server role to reply

    // LibDataChannel objects
    std::shared_ptr<rtc::PeerConnection> PC;
    std::shared_ptr<rtc::DataChannel>    DC;
    std::shared_ptr<rtc::Track>          SendAudioTrack; // client sendonly
    std::shared_ptr<rtc::Track>          RecvAudioTrack; // server recvonly

    // Delegates
    FO3DSOnWebRtcState StateDelegate;
    FO3DSOnWebRtcData DataDelegate;
    FO3DSOnWebRtcRtp  RtpDelegate;
};
