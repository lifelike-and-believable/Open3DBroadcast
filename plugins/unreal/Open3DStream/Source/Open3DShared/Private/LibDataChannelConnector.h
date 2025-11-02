#pragma once

#include "CoreMinimal.h"
#include "IWebRTCConnector.h"

#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "WebSocketsModule.h"

#include <rtc/rtc.hpp>
#include <memory>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#if O3DS_WITH_OPUS
#include <opus.h>
#endif

// LibDataChannel-backed implementation of IWebRTCConnector.
// Signaling: Unreal WebSockets, JSON messages compatible with sample components.

class FLibDataChannelConnector : public IWebRTCConnector
{
public:
    FLibDataChannelConnector() = default;
    virtual ~FLibDataChannelConnector() override { bInDestructor.store(true); Stop(); }

    // IWebRTCConnector
    virtual bool Start(const FO3DSWebRtcConfig& InConfig) override;
    virtual void Stop() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool IsOpen() const override { return bOpen.load(); }
    virtual bool Send(const uint8* Data, int32 NumBytes) override;
    virtual bool EnableAudioSend(bool bEnable) override;
    virtual bool SendAudioPcm16(const int16* Samples, int32 NumSamples, int32 SampleRate, int32 NumChannels) override;

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
    std::atomic<bool> bInDestructor{false};

    // Signaling
    TSharedPtr<IWebSocket> WS;
    FString RemotePeerId; // used by server role to reply

    // LibDataChannel objects
    std::shared_ptr<rtc::PeerConnection> PC;
    std::shared_ptr<rtc::DataChannel>    DC;
    std::shared_ptr<rtc::Track>          SendAudioTrack; // client sendonly
    std::shared_ptr<rtc::Track>          RecvAudioTrack; // server recvonly

    // Audio send queue (PCM16 frames), drained on a worker thread into RTP packets
    struct FAudioChunk { TArray<int16> Samples; int32 SampleRate = 0; int32 NumChannels = 0; };
    std::mutex AudioMutex;
    std::deque<FAudioChunk> AudioQueue;
    std::atomic<bool> bAudioPump{false};
    std::thread AudioThread;
    int AudioSeq = 0; // RTP seq counter for audio

#if O3DS_WITH_OPUS
    // Opus encoder state (created when send track opens, destroyed on close)
    OpusEncoder* OpusEnc = nullptr;
    int OpusSampleRate = 48000;
    int OpusChannels = 1;
    int OpusFrameSamplesPerChannel = 960; // 20ms @ 48k
    TArray<int16> OpusAccumPcm; // accumulation buffer to form exact frames
    TArray<uint8> OpusOutBuf;   // output bytes for one frame
#endif

    // Delegates
    FO3DSOnWebRtcState StateDelegate;
    FO3DSOnWebRtcData DataDelegate;
    FO3DSOnWebRtcRtp  RtpDelegate;
     // Signaling state: guard candidate application until remote description is set
     struct FQueuedCand { std::string Cand; std::string Mid; };
     bool bRemoteDescriptionSet = false;
     TArray<FQueuedCand> PendingCandidates; // accessed on game thread only
};
