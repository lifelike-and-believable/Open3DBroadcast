#include "LibDataChannelConnector.h"
#include "Logging/LogMacros.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/IConsoleManager.h"
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include "Async/Async.h"
#include <deque>
#include <chrono>
#include <mutex>
#if O3DS_WITH_OPUS
#include <opus.h>
#endif

using namespace std::chrono_literals;

// Optional debug CVar to dump raw signaling JSON (only when Config.bVerbose is also enabled)
static TAutoConsoleVariable<int32> CVarO3DSLibDCDumpJson(
    TEXT("o3ds.WebRTC.LibDC.DumpJson"),
    0,
    TEXT("When 1, dump raw libdatachannel signaling JSON (send/recv). Requires connector verbose."),
    ECVF_Default);

// --- helpers ---
std::string FLibDataChannelConnector::ToStd(const FString& S)
{
    FTCHARToUTF8 C(*S);
    return std::string(C.Get(), C.Length());
}

FString FLibDataChannelConnector::ToFStr(const std::string& S)
{
    return FString(UTF8_TO_TCHAR(S.c_str()));
}

bool FLibDataChannelConnector::Start(const FO3DSWebRtcConfig& InConfig)
{
    if (bStarted.load())
    {
        Stop();
    }

    Config = InConfig;
    // Backend-specific defaulting tucked inside the connector: LibDC server requires a non-empty room.
    if (Config.Role == EO3DSWebRtcRole::Server && Config.Room.IsEmpty())
    {
        Config.Room = TEXT("server");
        if (Config.bVerbose)
        {
            UE_LOG(LogTemp, Warning, TEXT("LibDataChannelConnector: No room set for server role; defaulting to 'server'"));
        }
    }
    bOpen.store(false);
    bAudioOpen.store(false);
    bRemoteDescriptionSet = false;
    PendingCandidates.Reset();
    bSignalingConnected.store(false);
    // Stop any existing offer retry pump
    bOfferRetryPump.store(false);
    if (OfferRetryThread.joinable()) { OfferRetryThread.join(); }
    OfferRetryAttempt = 0;
    // Stop audio pump thread if running
    bAudioPump.store(false);
    if (AudioThread.joinable()) { AudioThread.join(); }

    // Init libdatachannel logger once per process is fine; calling again is benign.
    rtc::InitLogger(Config.bVerbose ? rtc::LogLevel::Info : rtc::LogLevel::Warning);

    if (Config.bVerbose)
    {
        UE_LOG(LogTemp, Log, TEXT("LibDataChannelConnector: Start role=%d url=%s room=%s audio=%d"),
            (int32)Config.Role, *Config.SignalingUrl, *Config.Room, (int32)Config.bEnableAudio);
    }

    bStarted.store(true);
    // Defer state broadcasts onto the game thread to avoid re-entrancy into engine delegate dispatch
    {
        TWeakPtr<IWebRTCConnector> Weak = AsShared();
        AsyncTask(ENamedThreads::GameThread, [Weak]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(TEXT("Starting"), false);
            }
        });
    }

    // Start signaling reconnect pump and initiate first connect
    StartSignalingReconnectPump();
    OpenWebSocket();
    return true;
}

void FLibDataChannelConnector::Stop()
{
    // Stop signaling and media cleanly; idempotent
    bStarted.store(false);

    // Stop client offer retry pump
    bOfferRetryPump.store(false);
    if (OfferRetryThread.joinable()) { OfferRetryThread.join(); }

    // Stop signaling reconnect pump
    bSignalingReconnectPump.store(false);
    if (SignalingReconnectThread.joinable()) { SignalingReconnectThread.join(); }

    // Stop audio pump
    bAudioPump.store(false);
    if (AudioThread.joinable()) { AudioThread.join(); }

    // Clear callbacks before resetting objects
    if (DC) {
        try { DC->onOpen(nullptr); DC->onClosed(nullptr); DC->onMessage(nullptr); } catch (...) {}
    }
    if (SendAudioTrack) {
        try { SendAudioTrack->onOpen(nullptr); SendAudioTrack->onClosed(nullptr); SendAudioTrack->onMessage(nullptr); } catch (...) {}
    }
    if (RecvAudioTrack) {
        try { RecvAudioTrack->onOpen(nullptr); RecvAudioTrack->onClosed(nullptr); RecvAudioTrack->onMessage(nullptr); } catch (...) {}
    }

#if O3DS_WITH_OPUS
    if (OpusEnc) { opus_encoder_destroy(OpusEnc); OpusEnc = nullptr; }
    OpusAccumPcm.Reset();
#endif

    DC.reset();
    SendAudioTrack.reset();
    RecvAudioTrack.reset();
    PC.reset();

    if (WS.IsValid()) { WS->Close(); WS.Reset(); }

    // Only broadcast Stopped if not in destructor
    if (!bInDestructor.load())
    {
        TWeakPtr<IWebRTCConnector> Weak = AsShared();
        AsyncTask(ENamedThreads::GameThread, [Weak]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(TEXT("Stopped"), false);
            }
        });
    }
}

void FLibDataChannelConnector::Tick(float /*DeltaSeconds*/)
{
    // No-op: libdatachannel operates on its own threads
}

bool FLibDataChannelConnector::Send(const uint8* Data, int32 NumBytes)
{
    if (!Data || NumBytes <= 0) return false;
    if (!DC || !bOpen.load()) return false;
    try {
        std::vector<std::byte> Buf((size_t)NumBytes);
        std::memcpy(Buf.data(), Data, (size_t)NumBytes);
        DC->send(Buf);
        return true;
    } catch (...) {
        return false;
    }
}

bool FLibDataChannelConnector::EnableAudioSend(bool bEnable)
{
    Config.bEnableAudio = bEnable;
    return true;
}

bool FLibDataChannelConnector::SendAudioPcm16(const int16* Samples, int32 NumSamples, int32 SampleRate, int32 NumChannels)
{
    // Only accept audio when the send track is actually open to avoid negotiation-time backlog
    if (!bStarted.load() || !bAudioOpen.load() || !Samples || NumSamples <= 0 || SampleRate <= 0 || NumChannels <= 0)
    {
        return false;
    }
    // For now, only accept 48k input; others will be dropped (future: resample)
    if (SampleRate != 48000)
    {
        if (Config.bVerbose)
        {
            UE_LOG(LogTemp, Verbose, TEXT("[LibDC] Drop non-48k input chunk (SR=%d)"), SampleRate);
        }
        return false;
    }
    FAudioChunk Chunk;
    Chunk.SampleRate = SampleRate;
    Chunk.NumChannels = NumChannels;
    Chunk.Samples.SetNumUninitialized(NumSamples);
    FMemory::Memcpy(Chunk.Samples.GetData(), Samples, (SIZE_T)NumSamples * sizeof(int16));
    {
        std::lock_guard<std::mutex> L(AudioMutex);
        // Enforce a small bounded queue to preserve real-time behavior
        const int32 MaxQueueMs = 200; // keep at most ~200ms buffered
        auto queuedMs = 0;
        for (const auto& Q : AudioQueue)
        {
            if (Q.SampleRate > 0 && Q.NumChannels > 0)
            {
                queuedMs += (int32)((int64)Q.Samples.Num() * 1000 / (Q.SampleRate * Q.NumChannels));
            }
        }
        const int32 newMs = (int32)((int64)NumSamples * 1000 / (SampleRate * FMath::Max(NumChannels, 1)));
        while (!AudioQueue.empty() && queuedMs + newMs > MaxQueueMs)
        {
            const auto& Old = AudioQueue.front();
            if (Old.SampleRate > 0 && Old.NumChannels > 0)
            {
                queuedMs -= (int32)((int64)Old.Samples.Num() * 1000 / (Old.SampleRate * Old.NumChannels));
            }
            AudioQueue.pop_front();
            if (Config.bVerbose)
            {
                UE_LOG(LogTemp, Verbose, TEXT("[LibDC] Dropped oldest audio chunk to enforce %dms cap (queued ~%dms)"), MaxQueueMs, queuedMs);
            }
        }
        AudioQueue.emplace_back(MoveTemp(Chunk));
    }
    return true;
}

void FLibDataChannelConnector::OpenWebSocket()
{
    FWebSocketsModule& WSM = FModuleManager::LoadModuleChecked<FWebSocketsModule>("WebSockets");

    FString Url = Config.SignalingUrl;
    if (Url.IsEmpty())
    {
        StateDelegate.Broadcast(TEXT("No signaling URL"), true);
        return;
    }

    // Client role: if no explicit path segment present, append "/client" to establish a stable identity.
    if (Config.Role == EO3DSWebRtcRole::Client)
    {
        FString Base = Url;
        FString Query;
        const bool bHasQuery = Base.Split(TEXT("?"), &Base, &Query);
        const int32 SchemeIdx = Base.Find(TEXT("://"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
        if (SchemeIdx != INDEX_NONE)
        {
            const int32 AfterScheme = SchemeIdx + 3;
            const int32 FirstSlash = Base.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromStart, AfterScheme);
            if (FirstSlash == INDEX_NONE)
            {
                Base = Base.EndsWith(TEXT("/")) ? (Base + TEXT("client")) : (Base + TEXT("/client"));
                Url = bHasQuery && !Query.IsEmpty() ? (Base + TEXT("?") + Query) : Base;
            }
        }
    }

    // Server role: force path identity to '/<room>' and drop any conflicting path or room= from query.
    if (Config.Role == EO3DSWebRtcRole::Server && !Config.Room.IsEmpty())
    {
        FString Base = Url;
        FString Query;
        const bool bHasQuery = Base.Split(TEXT("?"), &Base, &Query);
        const int32 SchemeIdx = Base.Find(TEXT("://"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
        if (SchemeIdx != INDEX_NONE)
        {
            const int32 AfterScheme = SchemeIdx + 3;
            int32 FirstSlash = Base.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromStart, AfterScheme);
            // BaseAuthority is up to the first slash after scheme (or full Base if none)
            const FString Authority = (FirstSlash != INDEX_NONE) ? Base.Left(FirstSlash) : Base;
            FString NewUrl = Authority;
            if (!NewUrl.EndsWith(TEXT("/"))) NewUrl += TEXT("/");
            NewUrl += Config.Room;
            if (bHasQuery && !Query.IsEmpty())
            {
                TArray<FString> Parts; Query.ParseIntoArray(Parts, TEXT("&"), true);
                TArray<FString> Kept;
                for (const FString& P : Parts)
                {
                    if (!P.StartsWith(TEXT("room="), ESearchCase::IgnoreCase) &&
                        !P.StartsWith(TEXT("role="), ESearchCase::IgnoreCase))
                    {
                        Kept.Add(P);
                    }
                }
                if (Kept.Num() > 0)
                {
                    NewUrl += TEXT("?") + FString::Join(Kept, TEXT("&"));
                }
            }
            Url = NewUrl;
        }
    }

    if (WS.IsValid()) { WS.Reset(); }
    WS = WSM.CreateWebSocket(Url);

    {
        TWeakPtr<IWebRTCConnector> Weak = AsShared();
        WS->OnConnected().AddLambda([this, Url, Weak]()
    {
        UE_LOG(LogTemp, Log, TEXT("[LibDC] WebSocket connected (%s)"), *Url);
        bSignalingConnected.store(true);
        AsyncTask(ENamedThreads::GameThread, [Weak]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(TEXT("SignalingConnected"), false);
            }
        });

        rtc::Configuration Cfg; // No STUN by default; can be extended via config later
        SetupPeerConnection(Cfg);

        if (Config.Role == EO3DSWebRtcRole::Client)
        {
            CreateClientMediaAndDC();
            StartClientOfferRetryPump();
        }
    });

    WS->OnConnectionError().AddLambda([this, Url, Weak](const FString& Err)
    {
        UE_LOG(LogTemp, Error, TEXT("[LibDC] WebSocket error: %s (url=%s)"), *Err, *Url);
        bSignalingConnected.store(false);
        const FString Msg = FString::Printf(TEXT("SignalingError: %s"), *Err);
        AsyncTask(ENamedThreads::GameThread, [Weak, Msg]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(Msg, true);
            }
        });
    });

    WS->OnClosed().AddLambda([this, Weak](int32 /*Status*/, const FString& Reason, bool /*bClean*/)
    {
        UE_LOG(LogTemp, Warning, TEXT("[LibDC] WebSocket closed: %s"), *Reason);
        // Reset peer/media so we can accept a fresh session on reconnect
        ResetPeerConnection();
        // Stop client offer retry pump while disconnected
        bOfferRetryPump.store(false);
        if (OfferRetryThread.joinable()) { OfferRetryThread.join(); }
        bSignalingConnected.store(false);
        AsyncTask(ENamedThreads::GameThread, [Weak]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(TEXT("SignalingClosed"), false);
            }
        });
    });
    }

    WS->OnMessage().AddLambda([this](const FString& Msg)
    {
        HandleIncomingJson(Msg);
    });

    UE_LOG(LogTemp, Log, TEXT("[LibDC] Connecting signaling: %s"), *Url);
    WS->Connect();
}

void FLibDataChannelConnector::SetupPeerConnection(const rtc::Configuration& Cfg)
{
    PC = std::make_shared<rtc::PeerConnection>(Cfg);

    TWeakPtr<IWebRTCConnector> Weak = AsShared();

    PC->onStateChange([this, Weak](rtc::PeerConnection::State S)
    {
        const FString Msg = FString::Printf(TEXT("PeerConnectionState:%d"), (int)S);
        AsyncTask(ENamedThreads::GameThread, [Weak, Msg]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(Msg, false);
            }
        });
    });

    PC->onLocalDescription([this](rtc::Description Desc)
    {
        // Routing: client targets server by room (path id); server targets "client"
        const FString Id = (Config.Role == EO3DSWebRtcRole::Client)
            ? (!Config.Room.IsEmpty() ? Config.Room : FString(TEXT("server")))
            : FString(TEXT("client"));

        TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
        if (!Id.IsEmpty())
        {
            Msg->SetStringField(TEXT("id"), Id);
        }
        Msg->SetStringField(TEXT("type"), UTF8_TO_TCHAR(Desc.typeString().c_str()));
        Msg->SetStringField(TEXT("description"), UTF8_TO_TCHAR(std::string(Desc).c_str()));
        SendJson(Msg);
    });

    PC->onLocalCandidate([this](rtc::Candidate Cand)
    {
        const FString Id = (Config.Role == EO3DSWebRtcRole::Client)
            ? (!Config.Room.IsEmpty() ? Config.Room : FString(TEXT("server")))
            : FString(TEXT("client"));

        TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
        if (!Id.IsEmpty())
        {
            Msg->SetStringField(TEXT("id"), Id);
        }
        Msg->SetStringField(TEXT("type"), TEXT("candidate"));
        Msg->SetStringField(TEXT("candidate"), UTF8_TO_TCHAR(std::string(Cand).c_str()));
        Msg->SetStringField(TEXT("mid"), UTF8_TO_TCHAR(Cand.mid().c_str()));
        SendJson(Msg);
    });

    // Server role: prepare to accept data channel and remote audio
    if (Config.Role == EO3DSWebRtcRole::Server)
    {
        // Pre-provision a recvonly audio transceiver so answer includes audio m-line (if enabled)
        if (Config.bEnableAudio)
        {
            try
            {
                rtc::Description::Audio RecvMedia("audio", rtc::Description::Direction::RecvOnly);
                RecvMedia.addOpusCodec(111);
                RecvAudioTrack = PC->addTrack(RecvMedia);
                AttachRecvAudioCallbacks(RecvAudioTrack);
            }
            catch (...)
            {
                UE_LOG(LogTemp, Warning, TEXT("[LibDC] Failed to pre-add RecvOnly audio track"));
            }
        }

        PC->onDataChannel([this, Weak](std::shared_ptr<rtc::DataChannel> InDC)
        {
            DC = InDC;
            DC->onOpen([this, Weak]()
            {
                bOpen.store(true);
                AsyncTask(ENamedThreads::GameThread, [Weak]()
                {
                    if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                    {
                        P->OnState().Broadcast(TEXT("DataChannelOpen"), false);
                    }
                });
            });
            DC->onClosed([this, Weak]()
            {
                bOpen.store(false);
                AsyncTask(ENamedThreads::GameThread, [Weak]()
                {
                    if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                    {
                        P->OnState().Broadcast(TEXT("DataChannelClosed"), false);
                    }
                });
            });
            DC->onMessage([this, Weak](rtc::message_variant M)
            {
                if (std::holds_alternative<rtc::binary>(M))
                {
                    const auto& Bin = std::get<rtc::binary>(M);
                    TArray<uint8> Bytes; Bytes.SetNumUninitialized((int32)Bin.size());
                    FMemory::Memcpy(Bytes.GetData(), Bin.data(), Bytes.Num());
                    AsyncTask(ENamedThreads::GameThread, [Weak, Bytes = MoveTemp(Bytes)]() mutable
                    {
                        if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                        {
                            P->OnData().Broadcast(Bytes);
                        }
                    });
                }
                else if (std::holds_alternative<std::string>(M))
                {
                    const auto& S = std::get<std::string>(M);
                    TArray<uint8> Bytes; Bytes.SetNumUninitialized((int32)S.size());
                    FMemory::Memcpy(Bytes.GetData(), S.data(), Bytes.Num());
                    AsyncTask(ENamedThreads::GameThread, [Weak, Bytes = MoveTemp(Bytes)]() mutable
                    {
                        if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                        {
                            P->OnData().Broadcast(Bytes);
                        }
                    });
                }
            });
        });

        PC->onTrack([this](std::shared_ptr<rtc::Track> Track)
        {
            RecvAudioTrack = Track;
            AttachRecvAudioCallbacks(RecvAudioTrack);
        });
    }
}

void FLibDataChannelConnector::ResetPeerConnection()
{
    // Stop audio pump
    bAudioPump.store(false);
    if (AudioThread.joinable()) { AudioThread.join(); }

    // Clear media/data callbacks to break cycles before reset
    if (DC) {
        try { DC->onOpen(nullptr); DC->onClosed(nullptr); DC->onMessage(nullptr); } catch (...) {}
    }
    if (SendAudioTrack) {
        try { SendAudioTrack->onOpen(nullptr); SendAudioTrack->onClosed(nullptr); SendAudioTrack->onMessage(nullptr); } catch (...) {}
    }
    if (RecvAudioTrack) {
        try { RecvAudioTrack->onOpen(nullptr); RecvAudioTrack->onClosed(nullptr); RecvAudioTrack->onMessage(nullptr); } catch (...) {}
    }

#if O3DS_WITH_OPUS
    if (OpusEnc) { opus_encoder_destroy(OpusEnc); OpusEnc = nullptr; }
    OpusAccumPcm.Reset();
#endif

    bOpen.store(false);
    bAudioOpen.store(false);
    bRemoteDescriptionSet = false;
    PendingCandidates.Reset();

    DC.reset();
    SendAudioTrack.reset();
    RecvAudioTrack.reset();
    PC.reset();
}

void FLibDataChannelConnector::StartSignalingReconnectPump()
{
    // Ensure single pump
    bSignalingReconnectPump.store(false);
    if (SignalingReconnectThread.joinable()) { SignalingReconnectThread.join(); }
    SignalingReconnectAttempt = 0;
    bSignalingReconnectPump.store(true);
    SignalingReconnectThread = std::thread([this]()
    {
        while (bSignalingReconnectPump.load())
        {
            if (!bStarted.load()) break;
            if (!bSignalingConnected.load())
            {
                // Backoff before attempting reconnection to avoid hammering
                ++SignalingReconnectAttempt;
                int delay = FMath::Min(1 << FMath::Min(SignalingReconnectAttempt, 4), 16); // 1,2,4,8,16 cap
                UE_LOG(LogTemp, Log, TEXT("[LibDC] Signaling reconnect attempt %d in %ds"), SignalingReconnectAttempt, delay);
                for (int i = 0; i < delay && bSignalingReconnectPump.load() && !bSignalingConnected.load() && bStarted.load(); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                if (!bSignalingReconnectPump.load() || bSignalingConnected.load() || !bStarted.load()) continue;
                // Ask game thread to open a new WebSocket
                AsyncTask(ENamedThreads::GameThread, [this]()
                {
                    if (bStarted.load())
                    {
                        OpenWebSocket();
                    }
                });
                // After scheduling, wait a bit for connection callbacks to fire
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            else
            {
                // Connected; reset attempt counter and sleep a little
                SignalingReconnectAttempt = 0;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        bSignalingReconnectPump.store(false);
    });
}

void FLibDataChannelConnector::StopSignalingReconnectPump()
{
    bSignalingReconnectPump.store(false);
    if (SignalingReconnectThread.joinable()) { SignalingReconnectThread.join(); }
}

void FLibDataChannelConnector::StartClientOfferRetryPump()
{
    if (Config.Role != EO3DSWebRtcRole::Client) return;
    // Ensure any prior pump is stopped
    bOfferRetryPump.store(false);
    if (OfferRetryThread.joinable()) { OfferRetryThread.join(); }
    OfferRetryAttempt = 0;
    bOfferRetryPump.store(true);
    OfferRetryThread = std::thread([this]()
    {
        // Retry with modest backoff until an answer is set or stopped
        while (bOfferRetryPump.load())
        {
            // Wait a bit before checking for an answer
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!bOfferRetryPump.load()) break;

            // If answer arrived, stop
            if (bRemoteDescriptionSet) break;

            // Only retry when signaling is present and connector running
            if (!bStarted.load() || !WS.IsValid()) continue;

            ++OfferRetryAttempt;
            UE_LOG(LogTemp, Log, TEXT("[LibDC] Client re-offering (no answer yet), attempt %d"), OfferRetryAttempt);
            ForceClientReoffer();

            // Backoff: 3s, 5s, 7s, up to 11s
            int backoff = 3 + FMath::Min(OfferRetryAttempt * 2, 8);
            for (int i = 0; i < backoff && bOfferRetryPump.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (bRemoteDescriptionSet) break;
            }
            if (bRemoteDescriptionSet) break;
        }
        bOfferRetryPump.store(false);
    });
}

void FLibDataChannelConnector::StopClientOfferRetryPump()
{
    bOfferRetryPump.store(false);
    if (OfferRetryThread.joinable()) { OfferRetryThread.join(); }
}

void FLibDataChannelConnector::ForceClientReoffer()
{
    if (Config.Role != EO3DSWebRtcRole::Client) return;
    // Recreate PC and media; this triggers a fresh offer via onLocalDescription
    ResetPeerConnection();
    rtc::Configuration Cfg;
    SetupPeerConnection(Cfg);
    CreateClientMediaAndDC();
}

void FLibDataChannelConnector::CreateClientMediaAndDC()
{
    if (!PC) return;

    TWeakPtr<IWebRTCConnector> Weak = AsShared();

    // Audio track first so offer contains audio m-line
    if (Config.bEnableAudio)
    {
        try
        {
            const uint32 SSRC = 1234;
            rtc::Description::Audio Media("audio", rtc::Description::Direction::SendOnly);
            Media.addOpusCodec(111);
            Media.addSSRC(SSRC, "o3ds-audio");
            SendAudioTrack = PC->addTrack(Media);
            SendAudioTrack->onOpen([this, Weak]() { 
                bAudioOpen.store(true); 
                AsyncTask(ENamedThreads::GameThread, [Weak]()
                {
                    if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                    {
                        P->OnState().Broadcast(TEXT("AudioOpen"), false);
                    }
                });
#if O3DS_WITH_OPUS
                // Create Opus encoder with current config
                OpusChannels = FMath::Clamp(Config.NumChannels, 1, 2);
                OpusSampleRate = 48000; // Opus internal sample rate fixed at 48k API side
                OpusFrameSamplesPerChannel = OpusSampleRate / 50; // 20ms
                int OpusErr = OPUS_OK;
                if (OpusEnc)
                {
                    opus_encoder_destroy(OpusEnc);
                    OpusEnc = nullptr;
                }
                OpusEnc = opus_encoder_create(OpusSampleRate, OpusChannels, OPUS_APPLICATION_AUDIO, &OpusErr);
                if (OpusErr != OPUS_OK || !OpusEnc)
                {
                    UE_LOG(LogTemp, Warning, TEXT("[LibDC] Opus encoder create failed: %d"), OpusErr);
                }
                else
                {
                    opus_encoder_ctl(OpusEnc, OPUS_SET_BITRATE(Config.BitrateKbps * 1000));
                    opus_encoder_ctl(OpusEnc, OPUS_SET_COMPLEXITY(8));
                }
                OpusAccumPcm.Reset();
                OpusOutBuf.SetNumUninitialized(4000); // generous enough for 20ms stereo frame
#endif
                // Start audio pump thread for queued PCM -> RTP
                bAudioPump.store(true);
                AudioSeq = 0;
                AudioThread = std::thread([this]()
                {
                    auto nextSend = std::chrono::steady_clock::now(); // Initialize nextSend for pacing
                    while (bAudioPump.load())
                    {
                        FAudioChunk Chunk;
                        bool bHas = false;
                        {
                            std::lock_guard<std::mutex> L(AudioMutex);
                            if (!AudioQueue.empty())
                            {
                                Chunk = MoveTemp(AudioQueue.front());
                                AudioQueue.pop_front();
                                bHas = true;
                            }
                        }
                        if (!bHas)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            continue;
                        }
                        // Accumulate PCM and emit 20ms Opus frames
#if O3DS_WITH_OPUS
                        if (!OpusEnc)
                        {
                            continue; // no encoder available
                        }
                        if (Chunk.SampleRate != 48000)
                        {
                            // For now, only accept 48k input. Future: resample here.
                            UE_LOG(LogTemp, Verbose, TEXT("[LibDC] Skipping non-48k PCM chunk (SR=%d)"), Chunk.SampleRate);
                            continue;
                        }
                        if (Chunk.NumChannels != OpusChannels)
                        {
                            // Simple downmix to mono if needed
                            if (OpusChannels == 1 && Chunk.NumChannels > 1)
                            {
                                const int frames = Chunk.Samples.Num() / Chunk.NumChannels;
                                TArray<int16> Down; Down.SetNumUninitialized(frames);
                                for (int i = 0; i < frames; ++i)
                                {
                                    int32 acc = 0;
                                    for (int c = 0; c < Chunk.NumChannels; ++c) acc += Chunk.Samples[i * Chunk.NumChannels + c];
                                    Down[i] = (int16)(acc / Chunk.NumChannels);
                                }
                                OpusAccumPcm.Append(Down.GetData(), Down.Num());
                            }
                            else
                            {
                                // Channels mismatch and not supported -> skip
                                continue;
                            }
                        }
                        else
                        {
                            OpusAccumPcm.Append(Chunk.Samples.GetData(), Chunk.Samples.Num());
                        }

                        const int frameStride = OpusFrameSamplesPerChannel * OpusChannels;
                        int32 available = OpusAccumPcm.Num();
                        int32 consumeOffset = 0;
                        while (available - consumeOffset >= frameStride && SendAudioTrack && SendAudioTrack->isOpen())
                        {
                            const opus_int16* framePtr = reinterpret_cast<const opus_int16*>(OpusAccumPcm.GetData() + consumeOffset);
                            const int maxBytes = OpusOutBuf.Num();
                            int bytes = 0;
                            if (OpusEnc)
                            {
                                bytes = opus_encode(OpusEnc, framePtr, OpusFrameSamplesPerChannel, OpusOutBuf.GetData(), maxBytes);
                            }
                            if (bytes > 0)
                            {
                                if (Config.bVerbose)
                                {
                                    UE_LOG(LogTemp, Log, TEXT("[LibDC] Opus frame: %d bytes payload (seq=%d)"), bytes, AudioSeq);
                                }
                                std::vector<std::byte> rtp(12 + bytes);
                                // RTP header: V=2, PT=111 (Opus)
                                rtp[0] = std::byte(0x80);
                                rtp[1] = std::byte(111);
                                rtp[2] = std::byte((AudioSeq >> 8) & 0xFF);
                                rtp[3] = std::byte(AudioSeq & 0xFF);
                                const uint32 ts = (uint32)(AudioSeq * OpusFrameSamplesPerChannel);
                                rtp[4] = std::byte((ts >> 24) & 0xFF);
                                rtp[5] = std::byte((ts >> 16) & 0xFF);
                                rtp[6] = std::byte((ts >> 8) & 0xFF);
                                rtp[7] = std::byte(ts & 0xFF);
                                const uint32 SSRC = 1234;
                                rtp[8] = std::byte((SSRC >> 24) & 0xFF);
                                rtp[9] = std::byte((SSRC >> 16) & 0xFF);
                                rtp[10] = std::byte((SSRC >> 8) & 0xFF);
                                rtp[11] = std::byte(SSRC & 0xFF);
                                std::memcpy(reinterpret_cast<uint8*>(&rtp[12]), OpusOutBuf.GetData(), (size_t)bytes);
                                try { SendAudioTrack->send(rtp); } catch (...) { break; }
                                AudioSeq++;
                                // Pace to 20ms per frame in wall-clock time
                                nextSend += std::chrono::milliseconds(20);
                                std::this_thread::sleep_until(nextSend);
                            }
                            consumeOffset += frameStride;
                        }
                        if (consumeOffset > 0)
                        {
                            // slide remaining samples to front
                            const int32 remaining = available - consumeOffset;
                            if (remaining > 0)
                            {
                                FMemory::Memmove(OpusAccumPcm.GetData(), OpusAccumPcm.GetData() + consumeOffset, remaining * sizeof(int16));
                            }
                            OpusAccumPcm.SetNumUninitialized(remaining);
                        }
#else
                        // Fallback: drop if Opus not enabled
#endif
                    }
                });
                // Optionally synthesize a debug tone by pushing PCM into the queue
                if (Config.bSendDebugTone) {
                    std::thread([this]() {
                        const float Hz = FMath::Max(1.f, Config.ToneHz);
                        const double Dur = FMath::Max(0.1, Config.ToneDurationSec);
                        const int SR = 48000; // ensure 48k for Opus path
                        const int NumSamples = static_cast<int>(Dur * SR);
                        TArray<int16> Pcm; Pcm.SetNumUninitialized(NumSamples);
                        for (int i = 0; i < NumSamples; ++i) {
                            const double t = static_cast<double>(i) / static_cast<double>(SR);
                            const double v = std::sin(2.0 * 3.141592653589793 * Hz * t) * 0.5; // 50% volume
                            Pcm[i] = (int16)FMath::RoundToInt(v * 32767.0);
                        }
                            // Preempt any backlog (e.g., mic silence) so tone starts immediately
                        {
                            std::lock_guard<std::mutex> L(AudioMutex);
                            AudioQueue.clear();
                        }
                        SendAudioPcm16(Pcm.GetData(), Pcm.Num(), SR, 1);
                        TWeakPtr<IWebRTCConnector> WeakInner = AsShared();
                        AsyncTask(ENamedThreads::GameThread, [WeakInner]()
                        {
                            if (TSharedPtr<IWebRTCConnector> P = WeakInner.Pin())
                            {
                                P->OnState().Broadcast(TEXT("DebugToneQueued"), false);
                            }
                        });
                    }).detach();
                }
            });
            SendAudioTrack->onClosed([this, Weak]() { 
                bAudioOpen.store(false); 
                // Stop pump thread
                bAudioPump.store(false);
                if (AudioThread.joinable()) { AudioThread.join(); }
#if O3DS_WITH_OPUS
                if (OpusEnc) { opus_encoder_destroy(OpusEnc); OpusEnc = nullptr; }
                OpusAccumPcm.Reset();
#endif
                AsyncTask(ENamedThreads::GameThread, [Weak]()
                {
                    if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                    {
                        P->OnState().Broadcast(TEXT("AudioClosed"), false);
                    }
                });
            });
        }
        catch (...)
        {
            UE_LOG(LogTemp, Warning, TEXT("[LibDC] Failed to add SendOnly audio track"));
        }
    }

    // Create DC (offerer)
    DC = PC->createDataChannel("o3ds");
    DC->onOpen([this, Weak]()
    {
        bOpen.store(true);
        AsyncTask(ENamedThreads::GameThread, [Weak]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(TEXT("DataChannelOpen"), false);
            }
        });
    });
    DC->onClosed([this, Weak]()
    {
        bOpen.store(false);
        AsyncTask(ENamedThreads::GameThread, [Weak]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(TEXT("DataChannelClosed"), false);
            }
        });
        // If signaling is still connected but DC closed, attempt a client re-offer
        if (Config.Role == EO3DSWebRtcRole::Client && bStarted.load() && bSignalingConnected.load())
        {
            UE_LOG(LogTemp, Log, TEXT("[LibDC] DataChannel closed while signaling connected; forcing client re-offer"));
            ForceClientReoffer();
            StartClientOfferRetryPump();
        }
    });
    DC->onMessage([this, Weak](auto M)
    {
        if (std::holds_alternative<rtc::binary>(M))
        {
            const auto& Bin = std::get<rtc::binary>(M);
            TArray<uint8> Bytes; Bytes.SetNumUninitialized((int32)Bin.size());
            FMemory::Memcpy(Bytes.GetData(), Bin.data(), Bytes.Num());
            AsyncTask(ENamedThreads::GameThread, [Weak, Bytes = MoveTemp(Bytes)]() mutable
            {
                if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                {
                    P->OnData().Broadcast(Bytes);
                }
            });
        }
        else if (std::holds_alternative<std::string>(M))
        {
            const auto& S = std::get<std::string>(M);
            TArray<uint8> Bytes; Bytes.SetNumUninitialized((int32)S.size());
            FMemory::Memcpy(Bytes.GetData(), S.data(), Bytes.Num());
            AsyncTask(ENamedThreads::GameThread, [Weak, Bytes = MoveTemp(Bytes)]() mutable
            {
                if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                {
                    P->OnData().Broadcast(Bytes);
                }
            });
        }
    });
}

void FLibDataChannelConnector::AttachRecvAudioCallbacks(const std::shared_ptr<rtc::Track>& Track)
{
    if (!Track) return;
    TWeakPtr<IWebRTCConnector> Weak = AsShared();
    Track->onOpen([this, Weak]() 
    { 
        AsyncTask(ENamedThreads::GameThread, [Weak]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(TEXT("RecvAudioOpen"), false);
            }
        });
    });
    Track->onClosed([this, Weak]() 
    { 
        AsyncTask(ENamedThreads::GameThread, [Weak]()
        {
            if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
            {
                P->OnState().Broadcast(TEXT("RecvAudioClosed"), false);
            }
        });
    });
    Track->onMessage([this, Weak](auto M)
    {
        if (std::holds_alternative<rtc::binary>(M))
        {
            const auto& Bin = std::get<rtc::binary>(M);
            TArray<uint8> Bytes; Bytes.SetNumUninitialized((int32)Bin.size());
            FMemory::Memcpy(Bytes.GetData(), Bin.data(), Bytes.Num());
            if (Config.bVerbose)
            {
                UE_LOG(LogTemp, Log, TEXT("[LibDC] Recv audio RTP packet: %d bytes"), Bytes.Num());
            }
            AsyncTask(ENamedThreads::GameThread, [Weak, Bytes = MoveTemp(Bytes)]() mutable
            {
                if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                {
                    P->OnRemoteAudioRtp().Broadcast(Bytes);
                }
            });
        }
    });
}

void FLibDataChannelConnector::SendJson(const TSharedPtr<FJsonObject>& Obj)
{
    if (!WS.IsValid()) return;
    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
    if (Config.bVerbose)
    {
        const FString Type = Obj->GetStringField(TEXT("type"));
        const FString Id   = Obj->HasField(TEXT("id")) ? Obj->GetStringField(TEXT("id")) : TEXT("");
        UE_LOG(LogTemp, Log, TEXT("[LibDC] signaling send: role=%s id='%s' type=%s len=%d"),
            (Config.Role == EO3DSWebRtcRole::Client ? TEXT("client") : TEXT("server")), *Id, *Type, Out.Len());
        if (CVarO3DSLibDCDumpJson.GetValueOnAnyThread() != 0)
        {
            UE_LOG(LogTemp, VeryVerbose, TEXT("[LibDC] JSON_OUT: %s"), *Out);
        }
    }
    WS->Send(Out);
}

void FLibDataChannelConnector::HandleIncomingJson(const FString& JsonStr)
{
    if (!bStarted.load())
    {
        return; // ignore stray signaling after Stop()
    }
    if (Config.bVerbose && CVarO3DSLibDCDumpJson.GetValueOnAnyThread() != 0)
    {
        UE_LOG(LogTemp, VeryVerbose, TEXT("[LibDC] JSON_IN: %s"), *JsonStr);
    }
    TSharedPtr<FJsonObject> Obj;
    TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return;

    const FString Type = Obj->GetStringField(TEXT("type"));
    if (Config.bVerbose)
    {
        const FString Id = Obj->HasField(TEXT("id")) ? Obj->GetStringField(TEXT("id")) : TEXT("");
        UE_LOG(LogTemp, Log, TEXT("[LibDC] signaling recv: role=%s id='%s' type=%s"),
            (Config.Role == EO3DSWebRtcRole::Client ? TEXT("client") : TEXT("server")), *Id, *Type);
    }

    if (Config.Role == EO3DSWebRtcRole::Client)
    {
        if (Type == TEXT("answer"))
        {
            const FString Sdp = Obj->GetStringField(TEXT("description"));
            if (PC)
            {
                PC->setRemoteDescription(rtc::Description(ToStd(Sdp), "answer"));
                bRemoteDescriptionSet = true;
                StopClientOfferRetryPump();
                for (const FQueuedCand& C : PendingCandidates)
                {
                    try { PC->addRemoteCandidate(rtc::Candidate(C.Cand, C.Mid)); } catch (...) {}
                }
                PendingCandidates.Reset();
            }
        }
        else if (Type == TEXT("candidate"))
        {
            const FString Cand = Obj->GetStringField(TEXT("candidate"));
            const FString Mid = Obj->GetStringField(TEXT("mid"));
            if (PC)
            {
                if (Cand.IsEmpty() || Mid.IsEmpty()) return;
                if (!bRemoteDescriptionSet)
                {
                    FQueuedCand QC{ ToStd(Cand), ToStd(Mid) };
                    PendingCandidates.Add(MoveTemp(QC));
                }
                else
                {
                    try { PC->addRemoteCandidate(rtc::Candidate(ToStd(Cand), ToStd(Mid))); } catch (...) {}
                }
            }
        }
    }
    else // Server role
    {
        if (Type == TEXT("offer"))
        {
            RemotePeerId = Obj->GetStringField(TEXT("id"));
            // Always recreate a fresh PeerConnection for a new offer to avoid invalid states after disconnects
            ResetPeerConnection();
            {
                rtc::Configuration Cfg;
                SetupPeerConnection(Cfg);
            }
            const FString Sdp = Obj->GetStringField(TEXT("description"));
            if (PC)
            {
                try { PC->setRemoteDescription(rtc::Description(ToStd(Sdp), "offer")); } catch (...) { return; }
                bRemoteDescriptionSet = true;
                for (const FQueuedCand& C : PendingCandidates)
                {
                    try { PC->addRemoteCandidate(rtc::Candidate(C.Cand, C.Mid)); } catch (...) {}
                }
                PendingCandidates.Reset();
                try { PC->createAnswer(); } catch (...) { /* swallow and wait for next offer */ }
            }
        }
        else if (Type == TEXT("candidate"))
        {
            if (!PC) return;
            const FString Cand = Obj->GetStringField(TEXT("candidate"));
            const FString Mid = Obj->GetStringField(TEXT("mid"));
            if (Cand.IsEmpty() || Mid.IsEmpty()) return;
            if (!bRemoteDescriptionSet)
            {
                FQueuedCand QC{ ToStd(Cand), ToStd(Mid) };
                PendingCandidates.Add(MoveTemp(QC));
            }
            else
            {
                try { PC->addRemoteCandidate(rtc::Candidate(ToStd(Cand), ToStd(Mid))); } catch (...) {}
            }
        }
        else if (Type == TEXT("answer"))
        {
            if (!PC) return;
            const FString Sdp = Obj->GetStringField(TEXT("description"));
            PC->setRemoteDescription(rtc::Description(ToStd(Sdp), "answer"));
            bRemoteDescriptionSet = true;
            for (const FQueuedCand& C : PendingCandidates)
            {
                try { PC->addRemoteCandidate(rtc::Candidate(C.Cand, C.Mid)); } catch (...) {}
            }
            PendingCandidates.Reset();
        }
    }
}
