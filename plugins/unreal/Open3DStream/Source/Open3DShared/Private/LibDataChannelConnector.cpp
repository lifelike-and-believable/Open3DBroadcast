#include "LibDataChannelConnector.h"
#include "Logging/LogMacros.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

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
    bOpen.store(false);
    bAudioOpen.store(false);

    // Init libdatachannel logger once per process is fine; calling again is benign.
    rtc::InitLogger(Config.bVerbose ? rtc::LogLevel::Info : rtc::LogLevel::Warning);

    if (Config.bVerbose)
    {
        UE_LOG(LogTemp, Log, TEXT("LibDataChannelConnector: Start role=%d url=%s room=%s audio=%d"),
            (int32)Config.Role, *Config.SignalingUrl, *Config.Room, (int32)Config.bEnableAudio);
    }

    bStarted.store(true);
    StateDelegate.Broadcast(TEXT("Starting"), false);
    OpenWebSocket();
    return true;
}

void FLibDataChannelConnector::Stop()
{
    if (!bStarted.load()) return;

    bStarted.store(false);
    bOpen.store(false);
    bAudioOpen.store(false);

    // Close WS first to stop inflight signaling
    if (WS.IsValid())
    {
        WS->Close();
        WS.Reset();
    }

    // Release libdatachannel objects
    DC.reset();
    SendAudioTrack.reset();
    RecvAudioTrack.reset();
    PC.reset();
    RemotePeerId.Reset();

    if (Config.bVerbose)
    {
        UE_LOG(LogTemp, Log, TEXT("LibDataChannelConnector: Stopped"));
    }
    StateDelegate.Broadcast(TEXT("Stopped"), false);
}

void FLibDataChannelConnector::Tick(float /*DeltaSeconds*/)
{
    // libdatachannel and UE WebSockets are internally pumped. No blocking here.
}

bool FLibDataChannelConnector::Send(const uint8* Data, int32 NumBytes)
{
    if (!bStarted.load() || !DC || !bOpen.load() || !Data || NumBytes <= 0) return false;
    try
    {
        rtc::binary Bin;
        Bin.resize((size_t)NumBytes);
        FMemory::Memcpy(Bin.data(), Data, (size_t)NumBytes);
        DC->send(std::move(Bin));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool FLibDataChannelConnector::EnableAudioSend(bool bEnable)
{
    // This toggles intent; for LibDC we add the sendonly track during client setup.
    // For dynamic toggling, we could renegotiate; for now, return the desired state.
    return bEnable;
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

    // Normalize URL for libdatachannel sample signaling server (server role only):
    // If acting as server and a room is specified via query (?room=<id>) with no path id,
    // move it into the path: ws://host:port/<id>?<rest>
    // The client role should not claim the room id in the path; it targets the server via the 'id' field.
    if (Config.Role == EO3DSWebRtcRole::Server && !Config.Room.IsEmpty())
    {
        // Extract base (scheme://host:port[/path]) and query
        FString Base = Url;
        FString Query;
        if (Url.Split(TEXT("?"), &Base, &Query))
        {
            // Remove any room=... from the query
            TArray<FString> Parts; Query.ParseIntoArray(Parts, TEXT("&"), true);
            TArray<FString> Kept;
            for (const FString& P : Parts)
            {
                if (!P.StartsWith(TEXT("room="), ESearchCase::IgnoreCase))
                {
                    Kept.Add(P);
                }
            }

            // If base currently has no explicit path segment, append /<room>
            // Very conservative: only append if Base ends at the authority with optional trailing '/'
            // (avoid appending if Base already contains a non-empty path segment)
            int SchemeIdx = -1; int HostStart = -1; int PathStart = -1;
            if (Base.FindChar(':', SchemeIdx))
            {
                HostStart = Base.Find(TEXT("//"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SchemeIdx) + 2;
                PathStart = Base.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromStart, HostStart);
            }
            const bool bBaseHasPath = (PathStart != INDEX_NONE) && (PathStart > HostStart);
            FString NewUrl = Base;
            if (!bBaseHasPath || Base.EndsWith(TEXT("/")))
            {
                if (!Base.EndsWith(TEXT("/"))) NewUrl += TEXT("/");
                NewUrl += Config.Room;
            }

            if (Kept.Num() > 0)
            {
                NewUrl += TEXT("?") + FString::Join(Kept, TEXT("&"));
            }
            Url = NewUrl;
        }
    }

    WS = WSM.CreateWebSocket(Url);

    WS->OnConnected().AddLambda([this, Url]()
    {
        UE_LOG(LogTemp, Log, TEXT("[LibDC] WebSocket connected (%s)"), *Url);
        StateDelegate.Broadcast(TEXT("SignalingConnected"), false);

        rtc::Configuration Cfg; // No STUN by default; can be extended via config later
        SetupPeerConnection(Cfg);

        if (Config.Role == EO3DSWebRtcRole::Client)
        {
            CreateClientMediaAndDC();
        }
    });

    WS->OnConnectionError().AddLambda([this, Url](const FString& Err)
    {
        UE_LOG(LogTemp, Error, TEXT("[LibDC] WebSocket error: %s (url=%s)"), *Err, *Url);
        StateDelegate.Broadcast(FString::Printf(TEXT("SignalingError: %s"), *Err), true);
    });

    WS->OnClosed().AddLambda([this](int32 /*Status*/, const FString& Reason, bool /*bClean*/)
    {
        UE_LOG(LogTemp, Warning, TEXT("[LibDC] WebSocket closed: %s"), *Reason);
        StateDelegate.Broadcast(TEXT("SignalingClosed"), false);
    });

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

    PC->onStateChange([this](rtc::PeerConnection::State S)
    {
        StateDelegate.Broadcast(FString::Printf(TEXT("PeerConnectionState:%d"), (int)S), false);
    });

    PC->onLocalDescription([this](rtc::Description Desc)
    {
        // For client role, we use Config.Room as routing id. For server, prefer RemotePeerId if provided.
        const FString Id = (Config.Role == EO3DSWebRtcRole::Client)
            ? (Config.Room.IsEmpty() ? FString(TEXT("default")) : Config.Room)
            : RemotePeerId;

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
            ? (Config.Room.IsEmpty() ? FString(TEXT("default")) : Config.Room)
            : RemotePeerId;

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

        PC->onDataChannel([this](std::shared_ptr<rtc::DataChannel> InDC)
        {
            DC = InDC;
            DC->onOpen([this]()
            {
                bOpen.store(true);
                StateDelegate.Broadcast(TEXT("DataChannelOpen"), false);
            });
            DC->onClosed([this]()
            {
                bOpen.store(false);
                StateDelegate.Broadcast(TEXT("DataChannelClosed"), false);
            });
            DC->onMessage([this](rtc::message_variant M)
            {
                if (std::holds_alternative<rtc::binary>(M))
                {
                    const auto& Bin = std::get<rtc::binary>(M);
                    TArray<uint8> Bytes; Bytes.SetNumUninitialized((int32)Bin.size());
                    FMemory::Memcpy(Bytes.GetData(), Bin.data(), Bytes.Num());
                    DataDelegate.Broadcast(Bytes);
                }
                else if (std::holds_alternative<std::string>(M))
                {
                    const auto& S = std::get<std::string>(M);
                    TArray<uint8> Bytes; Bytes.SetNumUninitialized((int32)S.size());
                    FMemory::Memcpy(Bytes.GetData(), S.data(), Bytes.Num());
                    DataDelegate.Broadcast(Bytes);
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

void FLibDataChannelConnector::CreateClientMediaAndDC()
{
    if (!PC) return;

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
            SendAudioTrack->onOpen([this]() { 
                bAudioOpen.store(true); 
                StateDelegate.Broadcast(TEXT("AudioOpen"), false);
                // Optionally kick a debug tone when audio is open
                if (Config.bSendDebugTone) {
                    std::thread([this]() {
                        // small delay to ensure remote is ready
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        // Synthesize and send RTP tone packets
                        const float Hz = FMath::Max(1.f, Config.ToneHz);
                        const double Dur = FMath::Max(0.1, Config.ToneDurationSec);
                        const int SR = FMath::Clamp(Config.SampleRate, 8000, 48000);

                        // Generate PCM16 mono
                        const int numSamples = static_cast<int>(Dur * SR);
                        std::vector<uint8> pcm; pcm.resize(numSamples * 2);
                        for (int i = 0; i < numSamples; ++i) {
                            const double t = static_cast<double>(i) / static_cast<double>(SR);
                            const double v = std::sin(2.0 * 3.141592653589793 * Hz * t) * 0.5; // 50% volume
                            const int16 s = (int16)FMath::RoundToInt(v * 32767.0);
                            pcm[2 * i + 0] = (uint8)(s & 0xFF);
                            pcm[2 * i + 1] = (uint8)((s >> 8) & 0xFF);
                        }

                        // Chunk into ~20ms frames ~960 samples at 48k (scale with SR)
                        const int samplesPerPacket = (SR / 50); // 20ms
                        const size_t payloadBytes = (size_t)(samplesPerPacket * 2);
                        const uint32 SSRC = 1234;
                        int seq = 0;
                        int packets = 0;
                        size_t totalBytes = 0;
                        size_t off = 0;
                        while (off < pcm.size() && SendAudioTrack && SendAudioTrack->isOpen()) {
                            const size_t remain = pcm.size() - off;
                            const size_t chunk = FMath::Min<size_t>(payloadBytes, remain);
                            std::vector<std::byte> rtp(12 + chunk);
                            // RTP header: V=2, PT=111 (Opus), simple seq/ts progression
                            rtp[0] = std::byte(0x80);
                            rtp[1] = std::byte(111);
                            rtp[2] = std::byte((seq >> 8) & 0xFF);
                            rtp[3] = std::byte(seq & 0xFF);
                            const uint32 ts = (uint32)(seq * samplesPerPacket);
                            rtp[4] = std::byte((ts >> 24) & 0xFF);
                            rtp[5] = std::byte((ts >> 16) & 0xFF);
                            rtp[6] = std::byte((ts >> 8) & 0xFF);
                            rtp[7] = std::byte(ts & 0xFF);
                            rtp[8] = std::byte((SSRC >> 24) & 0xFF);
                            rtp[9] = std::byte((SSRC >> 16) & 0xFF);
                            rtp[10] = std::byte((SSRC >> 8) & 0xFF);
                            rtp[11] = std::byte(SSRC & 0xFF);
                            // copy payload
                            std::memcpy(reinterpret_cast<uint8*>(&rtp[12]), pcm.data() + off, chunk);
                            try { SendAudioTrack->send(rtp); } catch (...) { break; }
                            off += chunk; seq++; packets++; totalBytes += chunk;
                            if (Config.bVerbose)
                            {
                                UE_LOG(LogTemp, Log, TEXT("[LibDC] DebugTone RTP sent: seq=%d bytes=%d (accum=%d)"), seq-1, (int)chunk, (int)totalBytes);
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                        StateDelegate.Broadcast(TEXT("DebugToneSent"), false);
                        if (Config.bVerbose)
                        {
                            UE_LOG(LogTemp, Log, TEXT("[LibDC] DebugTone summary: packets=%d totalBytes=%d"), packets, (int)totalBytes);
                        }
                    }).detach();
                }
            });
            SendAudioTrack->onClosed([this]() { bAudioOpen.store(false); StateDelegate.Broadcast(TEXT("AudioClosed"), false); });
        }
        catch (...)
        {
            UE_LOG(LogTemp, Warning, TEXT("[LibDC] Failed to add SendOnly audio track"));
        }
    }

    // Create DC (offerer)
    DC = PC->createDataChannel("o3ds");
    DC->onOpen([this]()
    {
        bOpen.store(true);
        StateDelegate.Broadcast(TEXT("DataChannelOpen"), false);
    });
    DC->onClosed([this]()
    {
        bOpen.store(false);
        StateDelegate.Broadcast(TEXT("DataChannelClosed"), false);
    });
    DC->onMessage([this](auto M)
    {
        if (std::holds_alternative<rtc::binary>(M))
        {
            const auto& Bin = std::get<rtc::binary>(M);
            TArray<uint8> Bytes; Bytes.SetNumUninitialized((int32)Bin.size());
            FMemory::Memcpy(Bytes.GetData(), Bin.data(), Bytes.Num());
            DataDelegate.Broadcast(Bytes);
        }
        else if (std::holds_alternative<std::string>(M))
        {
            const auto& S = std::get<std::string>(M);
            TArray<uint8> Bytes; Bytes.SetNumUninitialized((int32)S.size());
            FMemory::Memcpy(Bytes.GetData(), S.data(), Bytes.Num());
            DataDelegate.Broadcast(Bytes);
        }
    });
}

void FLibDataChannelConnector::AttachRecvAudioCallbacks(const std::shared_ptr<rtc::Track>& Track)
{
    if (!Track) return;
    Track->onOpen([this]() { StateDelegate.Broadcast(TEXT("RecvAudioOpen"), false); });
    Track->onClosed([this]() { StateDelegate.Broadcast(TEXT("RecvAudioClosed"), false); });
    Track->onMessage([this](auto M)
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
            RtpDelegate.Broadcast(Bytes);
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
    }
    WS->Send(Out);
}

void FLibDataChannelConnector::HandleIncomingJson(const FString& JsonStr)
{
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
            if (PC) PC->setRemoteDescription(rtc::Description(ToStd(Sdp), "answer"));
        }
        else if (Type == TEXT("candidate"))
        {
            const FString Cand = Obj->GetStringField(TEXT("candidate"));
            const FString Mid = Obj->GetStringField(TEXT("mid"));
            if (PC) PC->addRemoteCandidate(rtc::Candidate(ToStd(Cand), ToStd(Mid)));
        }
    }
    else // Server role
    {
        if (Type == TEXT("offer"))
        {
            RemotePeerId = Obj->GetStringField(TEXT("id"));
            if (!PC)
            {
                rtc::Configuration Cfg;
                SetupPeerConnection(Cfg);
            }
            const FString Sdp = Obj->GetStringField(TEXT("description"));
            PC->setRemoteDescription(rtc::Description(ToStd(Sdp), "offer"));
            PC->createAnswer();
        }
        else if (Type == TEXT("candidate"))
        {
            if (!PC) return;
            const FString Cand = Obj->GetStringField(TEXT("candidate"));
            const FString Mid = Obj->GetStringField(TEXT("mid"));
            PC->addRemoteCandidate(rtc::Candidate(ToStd(Cand), ToStd(Mid)));
        }
        else if (Type == TEXT("answer"))
        {
            if (!PC) return;
            const FString Sdp = Obj->GetStringField(TEXT("description"));
            PC->setRemoteDescription(rtc::Description(ToStd(Sdp), "answer"));
        }
    }
}
