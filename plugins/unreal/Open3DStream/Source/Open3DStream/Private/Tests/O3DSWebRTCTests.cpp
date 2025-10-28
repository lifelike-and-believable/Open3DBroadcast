// Copyright (c) Open3DStream Contributors

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Misc/Paths.h"
#include "Misc/OutputDeviceNull.h"
#include "HAL/ThreadSafeBool.h"
#include "Containers/StringConv.h"
#include "Math/UnrealMathUtility.h"

#include "IWebRTCConnector.h"
#include "Open3DStreamSourceSettings.h" // for EO3DSWebRtcBackendReceiver

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    // Get a signaling URL from environment; tests will skip if not provided.
    FString GetSignalingUrlFromEnv()
    {
        // Prefer O3DS_WEBRTC_URL, fallback to O3DS_SIGNALING_URL
        FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("O3DS_WEBRTC_URL"));
        if (!Url.IsEmpty())
        {
            return Url;
        }
        return FPlatformMisc::GetEnvironmentVariable(TEXT("O3DS_SIGNALING_URL"));
    }

    // Simple pump loop helper: ticks connectors until timeout, returns elapsed seconds
    double PumpUntil(double TimeoutSec, TArray<TSharedPtr<IWebRTCConnector>>& Connectors, TFunctionRef<bool()> Condition)
    {
        const double Start = FPlatformTime::Seconds();
        while ((FPlatformTime::Seconds() - Start) < TimeoutSec)
        {
            for (TSharedPtr<IWebRTCConnector>& C : Connectors)
            {
                if (C)
                {
                    C->Tick();
                }
            }
            if (Condition())
            {
                break;
            }
            // Small sleep to avoid hogging CPU
            FPlatformProcess::Sleep(0.01f);
        }
        return FPlatformTime::Seconds() - Start;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSWebRTC_ConnectAndMessage,
    "Open3DStream.WebRTC.ConnectAndMessage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSWebRTC_ConnectAndMessage::RunTest(const FString& Params)
{
    const FString Url = GetSignalingUrlFromEnv();
    if (Url.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("O3DS WebRTC test skipped: set O3DS_WEBRTC_URL to run (e.g., webrtc://localhost:8080?room=test)"));
        return true; // skip gracefully
    }

    // Create libdatachannel backend connectors
    TSharedPtr<IWebRTCConnector> Server = CreateWebRTCConnector(EO3DSWebRtcBackendReceiver::LibDataChannel);
    TSharedPtr<IWebRTCConnector> Client = CreateWebRTCConnector(EO3DSWebRtcBackendReceiver::LibDataChannel);
    if (!Server || !Client)
    {
        AddError(TEXT("Failed to create WebRTC connectors"));
        return false;
    }

    // Wire server receive to capture message
    FThreadSafeBool bGotMessage(false);
    Server->SetDataReceivedCallback([&bGotMessage](const uint8* Data, int32 Size)
    {
        if (Size > 0)
        {
            bGotMessage = true;
        }
    });

    // Start server/client; role is controlled via bIsServer flag
    const bool bServerOk = Server->Start(Url, /*bIsServer*/true);
    const bool bClientOk = Client->Start(Url, /*bIsServer*/false);
    TestTrue(TEXT("Server start() returned true"), bServerOk);
    TestTrue(TEXT("Client start() returned true"), bClientOk);

    // Pump until both connected (up to 10s)
    TArray<TSharedPtr<IWebRTCConnector>> All{Server, Client};
    PumpUntil(10.0, All, [&]()
    {
        return Server->IsConnected() && Client->IsConnected();
    });

    if (!Server->IsConnected() || !Client->IsConnected())
    {
        AddError(TEXT("Timed out waiting for WebRTC connection (ensure signaling server is running and URL is correct)"));
        Server->Stop();
        Client->Stop();
        return false;
    }

    // Send a small payload from client to server
    const char* Msg = "O3DS-HELLO";
    const bool bSent = Client->SendDataLossy(reinterpret_cast<const uint8*>(Msg), (int32)FCStringAnsi::Strlen(Msg));
    TestTrue(TEXT("Client SendDataLossy returned true"), bSent);

    // Pump up to 3s for receive
    PumpUntil(3.0, All, [&]()
    {
        return (bool)bGotMessage;
    });

    TestTrue(TEXT("Server received data message"), (bool)bGotMessage);

    Server->Stop();
    Client->Stop();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSWebRTC_AudioSendReceive,
    "Open3DStream.WebRTC.AudioSendReceive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSWebRTC_AudioSendReceive::RunTest(const FString& Params)
{
    const FString Url = GetSignalingUrlFromEnv();
    if (Url.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("O3DS WebRTC audio test skipped: set O3DS_WEBRTC_URL to run (e.g., webrtc://localhost:8080?room=test)"));
        return true;
    }

    TSharedPtr<IWebRTCConnector> Server = CreateWebRTCConnector(EO3DSWebRtcBackendReceiver::LibDataChannel);
    TSharedPtr<IWebRTCConnector> Client = CreateWebRTCConnector(EO3DSWebRtcBackendReceiver::LibDataChannel);
    if (!Server || !Client)
    {
        AddError(TEXT("Failed to create WebRTC connectors"));
        return false;
    }

    // Capture remote audio on server
    FThreadSafeBool bGotAudio(false);
    int32 RxFrames = 0, RxCh = 0, RxSr = 0;
    FString RxStreamLabel, RxSubject;
    Server->OnRemoteAudio().AddLambda([&](const FString& StreamLabel, const FString& SubjectName, const float* PCM, int32 NumFrames, int32 NumChannels, int32 SampleRate)
    {
        if (PCM && NumFrames > 0)
        {
            bGotAudio = true;
            RxFrames = NumFrames;
            RxCh = NumChannels;
            RxSr = SampleRate;
            RxStreamLabel = StreamLabel;
            RxSubject = SubjectName;
        }
    });

    // Start both ends
    const bool bServerOk = Server->Start(Url, /*bIsServer*/true);
    const bool bClientOk = Client->Start(Url, /*bIsServer*/false);
    TestTrue(TEXT("Server start() returned true"), bServerOk);
    TestTrue(TEXT("Client start() returned true"), bClientOk);

    // Wait until connected
    TArray<TSharedPtr<IWebRTCConnector>> All{Server, Client};
    PumpUntil(10.0, All, [&]()
    {
        return Server->IsConnected() && Client->IsConnected();
    });
    if (!Server->IsConnected() || !Client->IsConnected())
    {
        AddError(TEXT("Timed out waiting for WebRTC connection (audio)"));
        Server->Stop();
        Client->Stop();
        return false;
    }

    // Enable audio on client; skip test if not supported by build
    IWebRTCConnector::FAudioSendConfig Cfg;
    Cfg.bEnable = true;
    Cfg.SampleRate = 48000;
    Cfg.NumChannels = 1;
    Cfg.BitrateKbps = 32;
    Cfg.StreamLabel = TEXT("o3ds:mix");
    Cfg.SubjectName = TEXT("TestSubject");
    Cfg.SourceType = TEXT("test");
    const bool bAudioEnabled = Client->EnableAudioSend(Cfg);
    if (!bAudioEnabled)
    {
        UE_LOG(LogTemp, Warning, TEXT("O3DS WebRTC audio not enabled in this build (skipping audio send/receive test)"));
        Server->Stop();
        Client->Stop();
        return true; // skip gracefully, no failure
    }

    // Generate 100ms of a 440Hz sine wave at 48kHz mono float
    const int32 Ms = 100;
    const int32 NumFrames = (Cfg.SampleRate * Ms) / 1000;
    TArray<float> Pcm;
    Pcm.SetNumUninitialized(NumFrames * Cfg.NumChannels);
    const float TwoPiF = 2.0f * PI * 440.0f;
    for (int32 i = 0; i < NumFrames; ++i)
    {
        const float t = (float)i / (float)Cfg.SampleRate;
        const float s = FMath::Sin(TwoPiF * t) * 0.2f; // -0.2..0.2
        Pcm[i] = s;
    }

    const bool bPushed = Client->PushPcm(Cfg.StreamLabel, Pcm.GetData(), NumFrames, Cfg.NumChannels, Cfg.SampleRate, /*ts*/0.0);
    TestTrue(TEXT("Client PushPcm returned true"), bPushed);

    // Wait up to 5s for audio to arrive
    PumpUntil(5.0, All, [&]()
    {
        return (bool)bGotAudio;
    });

    // Validate reception if present
    if (!(bool)bGotAudio)
    {
        AddError(TEXT("Timed out waiting for remote audio"));
    }
    else
    {
        TestTrue(TEXT("Remote audio frames > 0"), RxFrames > 0);
        TestTrue(TEXT("Remote audio channels == 1"), RxCh == 1);
        TestTrue(TEXT("Remote audio sample rate == 48000"), RxSr == 48000);
    }

    Server->Stop();
    Client->Stop();
    return (bool)bGotAudio;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSWebRTC_AudioAnnounce,
    "Open3DStream.WebRTC.AudioAnnounce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSWebRTC_AudioAnnounce::RunTest(const FString& Params)
{
    const FString Url = GetSignalingUrlFromEnv();
    if (Url.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("O3DS WebRTC audio announce test skipped: set O3DS_WEBRTC_URL"));
        return true;
    }

    TSharedPtr<IWebRTCConnector> Server = CreateWebRTCConnector(EO3DSWebRtcBackendReceiver::LibDataChannel);
    TSharedPtr<IWebRTCConnector> Client = CreateWebRTCConnector(EO3DSWebRtcBackendReceiver::LibDataChannel);
    if (!Server || !Client)
    {
        AddError(TEXT("Failed to create WebRTC connectors"));
        return false;
    }

    // Listen for JSON announce on server data channel
    FThreadSafeBool bGotAnnounce(false);
    Server->SetDataReceivedCallback([&](const uint8* Data, int32 Size)
    {
        if (Size <= 0) return;
        FString Json;
        FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Data), Size);
        Json = FString(Conv.Length(), Conv.Get());
        if (Json.Contains(TEXT("\"type\"")) && Json.Contains(TEXT("o3ds.audio.announce")))
        {
            bGotAnnounce = true;
        }
    });

    const bool bServerOk = Server->Start(Url, /*bIsServer*/true);
    const bool bClientOk = Client->Start(Url, /*bIsServer*/false);
    TestTrue(TEXT("Server start() returned true"), bServerOk);
    TestTrue(TEXT("Client start() returned true"), bClientOk);

    TArray<TSharedPtr<IWebRTCConnector>> All{Server, Client};
    PumpUntil(10.0, All, [&]()
    {
        return Server->IsConnected() && Client->IsConnected();
    });
    if (!Server->IsConnected() || !Client->IsConnected())
    {
        AddError(TEXT("Timed out waiting for WebRTC connection (announce)"));
        Server->Stop();
        Client->Stop();
        return false;
    }

    // Enable audio; if unsupported, skip
    IWebRTCConnector::FAudioSendConfig Cfg;
    Cfg.bEnable = true;
    Cfg.SampleRate = 48000;
    Cfg.NumChannels = 1;
    Cfg.BitrateKbps = 32;
    Cfg.StreamLabel = TEXT("o3ds:mix");
    Cfg.SubjectName = TEXT("AnnounceSubject");
    Cfg.SourceType = TEXT("test");
    const bool bAudioEnabled = Client->EnableAudioSend(Cfg);
    if (!bAudioEnabled)
    {
        UE_LOG(LogTemp, Warning, TEXT("O3DS WebRTC audio not enabled in this build (skipping announce test)"));
        Server->Stop();
        Client->Stop();
        return true;
    }

    // Pump up to 3s to receive the announce message
    PumpUntil(3.0, All, [&]()
    {
        return (bool)bGotAnnounce;
    });
    TestTrue(TEXT("Received audio announce JSON on server"), (bool)bGotAnnounce);

    Server->Stop();
    Client->Stop();
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
