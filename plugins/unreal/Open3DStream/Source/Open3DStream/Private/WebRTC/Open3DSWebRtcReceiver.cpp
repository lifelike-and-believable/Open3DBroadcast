// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebRTC/Open3DSWebRtcReceiver.h"
#include "O3DSStreamLogs.h"
#include "WebRTC/O3DSOpusDecoder.h"
#include "Open3DStreamSourceSettings.h"
#include "WebRTCConnectorFactory.h"
#include "Async/Async.h"
#include "O3DSAudioBus.h"
#include "O3DSUnifiedMessage.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformTime.h" // added for timing

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

// Prefer PCM callback over RTP decoding when available
static TAutoConsoleVariable<int32> CVarO3DSReceiverUsePcm(
    TEXT("o3ds.Receiver.Audio.UsePcm"),
    1,
    TEXT("When 1 and connector provides PCM callback, prefer PCM over RTP decoding for audio."),
    ECVF_Default);

// Diagnostic: log PCM arrival timing for LiveKit PCM path
static TAutoConsoleVariable<int32> CVarO3DSReceiverWebRtcLogPcmTiming(
    TEXT("o3ds.Receiver.WebRTC.LogPcmTiming"),
    0,
    TEXT("When 1, log PCM arrival timing (delta seconds) for LiveKit PCM callback."),
    ECVF_Default);

// New: control whether receiver coalesces even if data arrives on the GameThread
static TAutoConsoleVariable<int32> CVarO3DSReceiverCoalesceOnGameThread(
    TEXT("o3ds.Receiver.CoalesceOnGameThread"),
    1,
    TEXT("When 1, the receiver will coalesce incoming DataChannel payloads and schedule a single GT dispatch even if the callback runs on the GameThread."),
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

    // Ensure shutdown guard is cleared when starting (important for reconnect flows)
    bShuttingDown = false;

    // Ensure no stale coalesced data from previous run
    {
        FScopeLock L(&CoalesceMutex);
        PendingData.Reset();
        bDataDispatchScheduled.Store(false);
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
        // Connector implementers handle backend-specific defaults and URL normalization.
    // Optional: token for backends that require it (e.g., LiveKit)
    if (!Settings.LiveKitToken.IsEmpty())
    {
        Config.Token = Settings.LiveKitToken;
    }
    
    Config.bEnableAudio = Settings.bEnableWebRTCAudio;
    Config.SampleRate = 48000;
    Config.NumChannels = 1;
    Config.bVerbose = (CVarO3DSReceiverWebRtcLog->GetInt() != 0);

    // Store audio format for potential re-init on reconnect
    AudioSampleRate = Config.SampleRate;
    AudioNumChannels = Config.NumChannels;

    if (Config.bVerbose)
    {
        FString ElidedUrl = Config.SignalingUrl;
        UE_LOG(LogO3DSReceiverWebRTC, Log, TEXT("[WebRTC] Start backend=%d role=%d url=%s room=%s"),
            (int32)Config.Backend, (int32)Config.Role, *ElidedUrl, *Config.Room);
    }

    // Bind internal callbacks (connector delegates fire on arbitrary threads; marshal to game thread)
    Connector->OnState().AddRaw(this, &FOpen3DSWebRtcReceiver::OnConnectorState);
    Connector->OnData().AddRaw(this, &FOpen3DSWebRtcReceiver::OnConnectorData);
    // Decide audio path: PCM callback (preferred) or RTP decoding (fallback)
    bPreferPcmCallback = (CVarO3DSReceiverUsePcm->GetInt() != 0) && (Connector->OnRemoteAudioPcm() != nullptr);
    if (bPreferPcmCallback)
    {
        if (FO3DSOnWebRtcPcm16* Pcm = Connector->OnRemoteAudioPcm())
        {
            // Avoid capturing 'this' in the connector-stored lambda.
            // Keep timing log on connector thread, but publish PCM on the GameThread to satisfy delegate/thread-safety.
            Pcm->AddLambda([](const FO3DSPcm16Frame& Frame)
            {
                const double Now = FPlatformTime::Seconds();
                static double LastPcmWall = 0.0;
                if (CVarO3DSReceiverWebRtcLogPcmTiming->GetInt() != 0)
                {
                    const double Delta = (LastPcmWall > 0.0) ? (Now - LastPcmWall) : 0.0;
                    UE_LOG(LogO3DSReceiverWebRTC, Verbose, TEXT("PCM recv frames=%d ch=%d sr=%d wallDelta=%.6f"),
                        Frame.FramesPerChannel, Frame.NumChannels, Frame.SampleRate, Delta);
                }
                LastPcmWall = Now;

                // Copy samples and forward to the game thread for publishing (safe for delegates).
                TArray<int16> Copy = Frame.Samples; // copy to ensure lifetime across thread hop
                const int32 NumChannels = Frame.NumChannels;
                const int32 SampleRate  = Frame.SampleRate;

                // Dispatch to GameThread to avoid multicast-delegate thread-safety violations.
                AsyncTask(ENamedThreads::GameThread, [Copy = MoveTemp(Copy), NumChannels, SampleRate]() mutable
                {
                    O3DS::FAudioFrameMeta Meta;
                    Meta.StreamLabel = TEXT("o3ds:mix:livekit");
                    Meta.SubjectName = TEXT("WebRTC");
                    Meta.NumChannels = NumChannels;
                    Meta.SampleRate = SampleRate;
                    const uint8* Bytes = reinterpret_cast<const uint8*>(Copy.GetData());
                    const int32 NumBytes = Copy.Num() * sizeof(int16);
                    FO3DSAudioBus::PublishPcm16(Meta, Bytes, NumBytes);
                });
            });
        }
    }
    else
    {
        Connector->OnRemoteAudioRtp().AddRaw(this, &FOpen3DSWebRtcReceiver::OnConnectorAudioRtp);
    }

    // Start connector
    if (!Connector->Start(Config))
    {
        UE_LOG(LogO3DSReceiverWebRTC, Error, TEXT("failed to start connector"));
        return false;
    }

    // Create Opus decoder if audio enabled
    if (Config.bEnableAudio && !bPreferPcmCallback)
    {
        FScopeLock Lock(&OpusDecoderMutex);
        OpusDecoder = MakeShared<FO3DSOpusDecoder>();
        if (!OpusDecoder->Initialize(AudioSampleRate, AudioNumChannels))
        {
            UE_LOG(LogO3DSReceiverAudio, Error, TEXT("failed to initialize Opus decoder"));
            OpusDecoder.Reset();
            bAudioEnabled = false;
        }
        else
        {
            bAudioEnabled = true;
            UE_LOG(LogO3DSReceiverAudio, Log, TEXT("Audio enabled (sr=%d ch=%d)"), AudioSampleRate, AudioNumChannels);
        }
    }
    else if (Config.bEnableAudio && bPreferPcmCallback)
    {
        bAudioEnabled = true; // PCM path active
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

    // Prevent any in-flight tasks from dispatching after stop
    bShuttingDown = true;
    OnDataCallback = nullptr;
    OnStateCallback = nullptr;

    // Clear any pending coalesced data so we don't drain after stop
    {
        FScopeLock L(&CoalesceMutex);
        PendingData.Reset();
        bDataDispatchScheduled.Store(false);
    }

    // Shutdown decoder (safe under mutex)
    {
        FScopeLock Lock(&OpusDecoderMutex);
        if (OpusDecoder)
        {
            OpusDecoder->Shutdown();
            OpusDecoder.Reset();
        }
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
    // Marshal to game thread with weak capture to avoid UAF
    TWeakPtr<FOpen3DSWebRtcReceiver> Weak = AsShared();
    AsyncTask(ENamedThreads::GameThread, [Weak, State, bIsError]()
    {
        if (TSharedPtr<FOpen3DSWebRtcReceiver> P = Weak.Pin())
        {
            if (CVarO3DSReceiverWebRtcLog->GetInt() != 0)
            {
                UE_LOG(LogO3DSReceiverWebRTC, Log, TEXT("state=%s error=%d"), *State, bIsError ? 1 : 0);
            }

            // Reinitialize audio decoder when the data channel becomes open/connected
            if (!bIsError && !P->bPreferPcmCallback && P->bAudioEnabled &&
                (State.Contains(TEXT("DataChannelOpen")) || State.Contains(TEXT("connected")) || State.Contains(TEXT("connecting"))))
            {
                FScopeLock Lock(&P->OpusDecoderMutex);
                if (P->OpusDecoder)
                {
                    P->OpusDecoder->Shutdown();
                    P->OpusDecoder.Reset();
                }
                P->OpusDecoder = MakeShared<FO3DSOpusDecoder>();
                if (!P->OpusDecoder->Initialize(P->AudioSampleRate, P->AudioNumChannels))
                {
                    UE_LOG(LogO3DSReceiverAudio, Error, TEXT("failed to reinitialize Opus decoder on reconnect"));
                    P->OpusDecoder.Reset();
                    P->bAudioEnabled = false;
                }
                else
                {
                    P->bAudioEnabled = true;
                    UE_LOG(LogO3DSReceiverAudio, Log, TEXT("Reinitialized Opus decoder (sr=%d ch=%d)"), P->AudioSampleRate, P->AudioNumChannels);
                }
            }

            if (P->OnStateCallback)
            {
                P->OnStateCallback(State, bIsError);
            }
        }
    });
}

void FOpen3DSWebRtcReceiver::DispatchData(const TArray<uint8>& Bytes)
{
    if (bShuttingDown)
        return;

    const bool bLogContent = (CVarO3DSReceiverWebRtcLogDataContent->GetInt() != 0);
    const bool bLogCount   = (CVarO3DSReceiverWebRtcLogDataCount->GetInt() != 0);
    const bool bLogVerbose = (CVarO3DSReceiverWebRtcLog->GetInt() != 0);

    if (bLogContent)
    {
        const int32 MaxShow = 64;
        const int32 N = FMath::Min(Bytes.Num(), MaxShow);
        TArray<FString> Hex; Hex.Reserve(N);
        for (int32 i = 0; i < N; ++i) { Hex.Add(FString::Printf(TEXT("%02X"), Bytes[i])); }
        const FString Head = FString::Join(Hex, TEXT(" "));
        UE_LOG(LogO3DSReceiverWebRTC, Log, TEXT("Data: [%s]%s (%d bytes)"), *Head, (Bytes.Num() > MaxShow) ? TEXT(" ...") : TEXT(""), Bytes.Num());
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
}

void FOpen3DSWebRtcReceiver::OnConnectorData(const TArray<uint8>& Bytes)
{
    if (bShuttingDown)
        return;

    const bool bCoalesceOnGT = (CVarO3DSReceiverCoalesceOnGameThread->GetInt() != 0);

    // If caller is on GT and coalescing is disabled, dispatch immediately (old behavior)
    if (IsInGameThread() && !bCoalesceOnGT)
    {
        DispatchData(Bytes);
        return;
    }

    // Coalesce latest payload and schedule a single GT flush (works for off-thread and, when enabled, for GT arrivals).
    {
        FScopeLock L(&CoalesceMutex);
        PendingData = Bytes; // keep latest
        if (!bDataDispatchScheduled.Load())
        {
            bDataDispatchScheduled.Store(true);
            TWeakPtr<FOpen3DSWebRtcReceiver> Weak = AsShared();
            AsyncTask(ENamedThreads::GameThread, [Weak]() 
            {
                if (TSharedPtr<FOpen3DSWebRtcReceiver> P = Weak.Pin())
                {
                    // early abort if shutting down
                    if (P->bShuttingDown)
                    {
                        P->bDataDispatchScheduled.Store(false);
                        return;
                    }

                    TArray<uint8> Dispatch;
                    {
                        FScopeLock L2(&P->CoalesceMutex);
                        Dispatch = P->PendingData; // copy latest
                        P->bDataDispatchScheduled.Store(false);
                    }
                    P->DispatchData(Dispatch);
                }
            });
        }
    }
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

    // Protect decoder usage with mutex to avoid races vs re-init on reconnect
    if (!bPreferPcmCallback)
    {
        FScopeLock Lock(&OpusDecoderMutex);
        if (OpusDecoder && OpusDecoder->IsInitialized())
        {
            OpusDecoder->DecodeRtpPacket(RtpBytes);
            return;
        }
    }

    // Decoder not initialized or using PCM path
    static int32 WarnEvery = 0;
    if ((WarnEvery++ % 100) == 0)
    {
        UE_LOG(LogO3DSReceiverAudio, Warning, TEXT("Audio RTP received but decoder not initialized (audio disabled?)"));
    }
}
