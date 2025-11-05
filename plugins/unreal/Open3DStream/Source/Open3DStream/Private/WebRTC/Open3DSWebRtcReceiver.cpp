// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebRTC/Open3DSWebRtcReceiver.h"
#include "O3DSStreamLogs.h"
#include "WebRTC/O3DSOpusDecoder.h"
#include "Open3DStreamSourceSettings.h"
#include "WebRTCConnectorFactory.h"
#include "Async/Async.h"
#include "O3DSAudioBus.h"
#include "O3DSUnifiedMessage.h"

// CVars for receiver-side logging
static TAutoConsoleVariable<int32> CVarO3DSReceiverWebRtcLog(
    TEXT("o3ds.Receiver.WebRTC.Log"),
    0,
    TEXT("Enable verbose logging for WebRTC receiver adapter (0/1)."),
    ECVF_Default);
static TAutoConsoleVariable<int32> CVarO3DSReceiverWebRtcLogDataContent(
    TEXT("o3ds.Receiver.WebRTC.LogDataContent"),
    0,
    TEXT("When 1, log the received DataChannel payload as hex (first 64 bytes)."),
    ECVF_Default);
static TAutoConsoleVariable<int32> CVarO3DSReceiverWebRtcLogDataCount(
    TEXT("o3ds.Receiver.WebRTC.LogDataCount"),
    0,
    TEXT("When 1, log only the number of bytes received on the DataChannel."),
    ECVF_Default);
static TAutoConsoleVariable<int32> CVarO3DSReceiverWebRtcLogAudioRtp(
    TEXT("o3ds.Receiver.WebRTC.LogAudioRtp"),
    0,
    TEXT("When 1, log received RTP audio packet sizes."),
    ECVF_Default);

FOpen3DSWebRtcReceiver::FOpen3DSWebRtcReceiver()
{
}

FOpen3DSWebRtcReceiver::~FOpen3DSWebRtcReceiver()
{
    Stop();
}

bool FOpen3DSWebRtcReceiver::Start(const FOpen3DStreamSettings& Settings)
{
    if (bStarted)
    {
        UE_LOG(LogO3DSReceiverWebRTC, Warning, TEXT("already started"));
        return false;
    }

    // Create connector via factory based on backend selection
    EO3DSWebRtcBackend Backend = (Settings.WebRtcBackend == EO3DSWebRtcBackendReceiver::LibDataChannel)
        ? EO3DSWebRtcBackend::LibDataChannel
        : EO3DSWebRtcBackend::LiveKit;

    Connector = FWebRTCConnectorFactory::Create(Backend);
    if (!Connector)
    {
        UE_LOG(LogO3DSReceiverWebRTC, Error, TEXT("failed to create connector for backend"));
        return false;
    }

    // Configure connector in server role
    FO3DSWebRtcConfig Config;
    Config.Backend = Backend;
    Config.Role = EO3DSWebRtcRole::Server;
    
    // For server role, ensure the URL is normalized per LibDataChannelConnector expectations:
    // The Room will be moved into the WebSocket path (e.g., ws://host:port/<Room>)
    // and query parameters will be stripped by the connector's OpenWebSocket() logic.
    // Just pass the base signaling URL and let the connector handle path construction.
    Config.SignalingUrl = Settings.Url.ToString();
    Config.Room = Settings.WebRtcRoom;
    // Optional: token for backends that require it (e.g., LiveKit)
    if (!Settings.LiveKitToken.IsEmpty())
    {
        Config.Token = Settings.LiveKitToken;
    }
    
    Config.bEnableAudio = Settings.bEnableWebRTCAudio;
    Config.SampleRate = 48000;
    Config.NumChannels = 1;
    Config.bVerbose = (CVarO3DSReceiverWebRtcLog->GetInt() != 0);

    // Bind internal callbacks (connector delegates fire on arbitrary threads; marshal to game thread)
    Connector->OnState().AddRaw(this, &FOpen3DSWebRtcReceiver::OnConnectorState);
    Connector->OnData().AddRaw(this, &FOpen3DSWebRtcReceiver::OnConnectorData);
    Connector->OnRemoteAudioRtp().AddRaw(this, &FOpen3DSWebRtcReceiver::OnConnectorAudioRtp);
    if (FO3DSOnWebRtcPcm16* Pcm = Connector->OnRemoteAudioPcm())
    {
        Pcm->AddLambda([this](const FO3DSPcm16Frame& Frame)
        {
            // Publish PCM16 directly to audio bus on the game thread
            TArray<int16> Copy = Frame.Samples; // copy to ensure lifetime across thread hop
            AsyncTask(ENamedThreads::GameThread, [Copy = MoveTemp(Copy), Frame]()
            {
                O3DS::FAudioFrameMeta Meta;
                Meta.StreamLabel = TEXT("o3ds:mix:livekit");
                Meta.SubjectName = TEXT("WebRTC");
                Meta.NumChannels = Frame.NumChannels;
                Meta.SampleRate = Frame.SampleRate;
                // Timestamp unknown here; leave default 0
                const uint8* Bytes = reinterpret_cast<const uint8*>(Copy.GetData());
                const int32 NumBytes = Copy.Num() * sizeof(int16);
                FO3DSAudioBus::PublishPcm16(Meta, Bytes, NumBytes);
            });
        });
    }

    // Start connector
    if (!Connector->Start(Config))
    {
        UE_LOG(LogO3DSReceiverWebRTC, Error, TEXT("failed to start connector"));
        return false;
    }

    // Create Opus decoder if audio enabled
    if (Config.bEnableAudio)
    {
        OpusDecoder = MakeShared<FO3DSOpusDecoder>();
        if (!OpusDecoder->Initialize(Config.SampleRate, Config.NumChannels))
        {
            UE_LOG(LogO3DSReceiverAudio, Error, TEXT("failed to initialize Opus decoder"));
            OpusDecoder.Reset();
        }
        else
        {
            bAudioEnabled = true;
            // Always log once so users can confirm audio decode path enabled
            UE_LOG(LogO3DSReceiverAudio, Log, TEXT("Audio enabled (sr=%d ch=%d)"), Config.SampleRate, Config.NumChannels);
        }
    }

    bStarted = true;

    if (CVarO3DSReceiverWebRtcLog->GetInt() != 0)
    {
        UE_LOG(LogO3DSReceiverWebRTC, Log, TEXT("started (audio=%s)"),
            bAudioEnabled ? TEXT("enabled") : TEXT("disabled"));
    }

    return true;
}

void FOpen3DSWebRtcReceiver::Stop()
{
    if (!bStarted)
        return;

    // Shutdown decoder
    if (OpusDecoder)
    {
        OpusDecoder->Shutdown();
        OpusDecoder.Reset();
    }

    // Stop connector
    if (Connector)
    {
        Connector->Stop();
        Connector.Reset();
    }

    bStarted = false;
    bAudioEnabled = false;

    if (CVarO3DSReceiverWebRtcLog->GetInt() != 0)
    {
        UE_LOG(LogO3DSReceiverWebRTC, Log, TEXT("stopped"));
    }
}

void FOpen3DSWebRtcReceiver::Tick(float DeltaSeconds)
{
    if (Connector && bStarted)
    {
        Connector->Tick(DeltaSeconds);
    }
}

bool FOpen3DSWebRtcReceiver::IsOpen() const
{
    return bStarted && Connector && Connector->IsOpen();
}

void FOpen3DSWebRtcReceiver::SetOnDataCallback(TFunction<void(const TArray<uint8>&)> Callback)
{
    OnDataCallback = Callback;
}

void FOpen3DSWebRtcReceiver::SetOnStateCallback(TFunction<void(const FString&, bool)> Callback)
{
    OnStateCallback = Callback;
}

void FOpen3DSWebRtcReceiver::OnConnectorState(const FString& State, bool bIsError)
{
    // Marshal to game thread
    AsyncTask(ENamedThreads::GameThread, [this, State, bIsError]()
    {
        if (CVarO3DSReceiverWebRtcLog->GetInt() != 0)
        {
            UE_LOG(LogO3DSReceiverWebRTC, Log, TEXT("state=%s error=%d"), *State, bIsError ? 1 : 0);
        }

        if (OnStateCallback)
        {
            OnStateCallback(State, bIsError);
        }
    });
}

void FOpen3DSWebRtcReceiver::OnConnectorData(const TArray<uint8>& Bytes)
{
    // Marshal to game thread and forward to source parsing
    AsyncTask(ENamedThreads::GameThread, [this, Bytes]()
    {
        const bool bLogContent = (CVarO3DSReceiverWebRtcLogDataContent->GetInt() != 0);
        const bool bLogCount = (CVarO3DSReceiverWebRtcLogDataCount->GetInt() != 0);
        const bool bLogVerbose = (CVarO3DSReceiverWebRtcLog->GetInt() != 0);
        
        if (bLogContent)
        {
            // Show the first 64 byte values (hex) for FlatBuffer-style payloads
            const int32 MaxShow = 64;
            const int32 N = FMath::Min(Bytes.Num(), MaxShow);
            TArray<FString> Hex; Hex.Reserve(N);
            for (int32 i = 0; i < N; ++i)
            {
                Hex.Add(FString::Printf(TEXT("%02X"), Bytes[i]));
            }
            const FString Head = FString::Join(Hex, TEXT(" "));
            const bool bTrunc = (Bytes.Num() > MaxShow);
            UE_LOG(LogO3DSReceiverWebRTC, Log, TEXT("Data: [%s]%s (%d bytes)"), *Head, bTrunc ? TEXT(" ...") : TEXT(""), Bytes.Num());
        }
        else if (bLogCount)
        {
            UE_LOG(LogO3DSReceiverWebRTC, Verbose, TEXT("Data: %d bytes"), Bytes.Num());
        }
        else if (bLogVerbose)
        {
            UE_LOG(LogO3DSReceiverWebRTC, Verbose, TEXT("data bytes=%d"), Bytes.Num());
        }

        if (OnDataCallback)
        {
            OnDataCallback(Bytes);
        }
    });
}

void FOpen3DSWebRtcReceiver::OnConnectorAudioRtp(const TArray<uint8>& RtpBytes)
{
    // Log first packet once so users can confirm audio RTP path is live
    {
        static bool bFirstRtpSeen = false;
        if (!bFirstRtpSeen)
        {
            bFirstRtpSeen = true;
            UE_LOG(LogO3DSReceiverAudio, Log, TEXT("First audio RTP received (%d bytes)"), RtpBytes.Num());
        }
    }
    if (CVarO3DSReceiverWebRtcLogAudioRtp->GetInt() != 0)
    {
        UE_LOG(LogO3DSReceiverAudio, Log, TEXT("RTP: %d bytes"), RtpBytes.Num());
    }
    
    // Decode off the game thread (already on connector thread)
    if (OpusDecoder && OpusDecoder->IsInitialized())
    {
        OpusDecoder->DecodeRtpPacket(RtpBytes);
    }
    else
    {
        static int32 WarnEvery = 0;
        if ((WarnEvery++ % 100) == 0)
        {
            UE_LOG(LogO3DSReceiverAudio, Warning, TEXT("Audio RTP received but decoder not initialized (audio disabled?)"));
        }
    }
}
