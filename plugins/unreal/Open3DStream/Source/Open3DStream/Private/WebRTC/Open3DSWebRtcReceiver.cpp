// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebRTC/Open3DSWebRtcReceiver.h"
#include "WebRTC/O3DSOpusDecoder.h"
#include "Open3DStreamSourceSettings.h"
#include "WebRTCConnectorFactory.h"
#include "Async/Async.h"

// CVars for receiver-side logging
static TAutoConsoleVariable<int32> CVarO3DSReceiverWebRtcLog(
    TEXT("o3ds.Receiver.WebRTC.Log"),
    0,
    TEXT("Enable verbose logging for WebRTC receiver adapter (0/1)."),
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
        UE_LOG(LogTemp, Warning, TEXT("O3DS WebRTC Receiver: already started"));
        return false;
    }

    // Create connector via factory based on backend selection
    EO3DSWebRtcBackend Backend = (Settings.WebRtcBackend == EO3DSWebRtcBackendReceiver::LibDataChannel)
        ? EO3DSWebRtcBackend::LibDataChannel
        : EO3DSWebRtcBackend::LiveKit;

    Connector = FWebRTCConnectorFactory::Create(Backend);
    if (!Connector)
    {
        UE_LOG(LogTemp, Error, TEXT("O3DS WebRTC Receiver: failed to create connector for backend"));
        return false;
    }

    // Configure connector in server role
    FO3DSWebRtcConfig Config;
    Config.Backend = EO3DSWebRtcBackend::LibDataChannel;
    Config.Role = EO3DSWebRtcRole::Server;
    Config.SignalingUrl = Settings.Url.ToString();
    Config.Room = Settings.WebRtcRoom;
    Config.bEnableAudio = Settings.bEnableWebRTCAudio;
    Config.SampleRate = 48000;
    Config.NumChannels = 1;
    Config.bVerbose = (CVarO3DSReceiverWebRtcLog->GetInt() != 0);

    // Bind internal callbacks (connector delegates fire on arbitrary threads; marshal to game thread)
    Connector->OnState().AddRaw(this, &FOpen3DSWebRtcReceiver::OnConnectorState);
    Connector->OnData().AddRaw(this, &FOpen3DSWebRtcReceiver::OnConnectorData);
    Connector->OnRemoteAudioRtp().AddRaw(this, &FOpen3DSWebRtcReceiver::OnConnectorAudioRtp);

    // Start connector
    if (!Connector->Start(Config))
    {
        UE_LOG(LogTemp, Error, TEXT("O3DS WebRTC Receiver: failed to start connector"));
        return false;
    }

    // Create Opus decoder if audio enabled
    if (Config.bEnableAudio)
    {
        OpusDecoder = MakeShared<FO3DSOpusDecoder>();
        if (!OpusDecoder->Initialize(Config.SampleRate, Config.NumChannels))
        {
            UE_LOG(LogTemp, Error, TEXT("O3DS WebRTC Receiver: failed to initialize Opus decoder"));
            OpusDecoder.Reset();
        }
        else
        {
            bAudioEnabled = true;
        }
    }

    bStarted = true;

    if (CVarO3DSReceiverWebRtcLog->GetInt() != 0)
    {
        UE_LOG(LogTemp, Log, TEXT("O3DS WebRTC Receiver: started (audio=%s)"),
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
        UE_LOG(LogTemp, Log, TEXT("O3DS WebRTC Receiver: stopped"));
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
            UE_LOG(LogTemp, Log, TEXT("O3DS WebRTC Receiver: state=%s error=%d"), *State, bIsError ? 1 : 0);
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
        if (CVarO3DSReceiverWebRtcLog->GetInt() != 0)
        {
            UE_LOG(LogTemp, Verbose, TEXT("O3DS WebRTC Receiver: data bytes=%d"), Bytes.Num());
        }

        if (OnDataCallback)
        {
            OnDataCallback(Bytes);
        }
    });
}

void FOpen3DSWebRtcReceiver::OnConnectorAudioRtp(const TArray<uint8>& RtpBytes)
{
    // Decode off the game thread (already on connector thread)
    if (OpusDecoder && OpusDecoder->IsInitialized())
    {
        OpusDecoder->DecodeRtpPacket(RtpBytes);
    }
}
