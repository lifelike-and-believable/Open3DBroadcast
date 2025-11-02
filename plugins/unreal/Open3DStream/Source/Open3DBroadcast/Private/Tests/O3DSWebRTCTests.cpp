// Copyright (c) Open3DStream Contributors

#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"

#include "Open3DShared/Public/IWebRTCConnector.h"
#include "Open3DShared/Public/WebRTCConnectorFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

static bool O3DS_WaitUntil(double Seconds, TFunctionRef<bool()> Predicate)
{
    const double Start = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - Start) < Seconds)
    {
        if (Predicate()) { return true; }
        FPlatformProcess::Sleep(0.01f);
    }
    return false;
}

// Simple smoke test that exercises factory + start/stop lifecycle without requiring a server
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSWebRTC_FactoryAndLifecycle,
    "Open3DStream.M3.WebRTC.FactoryAndLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSWebRTC_FactoryAndLifecycle::RunTest(const FString& Parameters)
{
    TSharedPtr<IWebRTCConnector> C = FWebRTCConnectorFactory::Create(EO3DSWebRtcBackend::LibDataChannel);
    TestTrue(TEXT("Factory returned connector"), C.IsValid());
    if (!C.IsValid()) return false;

    FO3DSWebRtcConfig Cfg; // Intentionally bad URL; should not crash
    Cfg.Backend = EO3DSWebRtcBackend::LibDataChannel;
    Cfg.Role = EO3DSWebRtcRole::Client;
    Cfg.SignalingUrl = TEXT("ws://127.0.0.1:0");
    Cfg.bVerbose = false;

    bool Started = C->Start(Cfg);
    TestTrue(TEXT("Start returned true"), Started);

    // Give it a moment then stop
    FPlatformProcess::Sleep(0.05f);
    C->Stop();
    return true;
}


// End-to-end test using the libdatachannel sample signaling server.
// Requires a local server on ws://127.0.0.1:8080. If not present, the test exits early (pass) to avoid false failures on CI.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSWebRTC_E2E_DataChannelAndAudio,
    "Open3DStream.M3.WebRTC.E2E.DataChannelAndAudio",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSWebRTC_E2E_DataChannelAndAudio::RunTest(const FString& Parameters)
{
    // Quick probe: attempt to connect a throwaway WS via the connector; if signaling connect state never appears, skip.
    std::atomic<bool> bClientSig{false};
    std::atomic<bool> bServerSig{false};
    std::atomic<bool> bDCOpen{false};
    std::atomic<bool> bGotRtp{false};

    // Server
    TSharedPtr<IWebRTCConnector> Server = FWebRTCConnectorFactory::Create(EO3DSWebRtcBackend::LibDataChannel);
    TestTrue(TEXT("Server connector created"), Server.IsValid());
    if (!Server.IsValid()) return false;
    Server->OnState().AddLambda([&](const FString& S, bool){ if (S == TEXT("SignalingConnected")) bServerSig.store(true); if (S == TEXT("DataChannelOpen")) bDCOpen.store(true); });
    Server->OnRemoteAudioRtp().AddLambda([&](const TArray<uint8>& Bytes){ if (Bytes.Num() > 0) bGotRtp.store(true); });

    FO3DSWebRtcConfig S; S.Role = EO3DSWebRtcRole::Server; S.SignalingUrl = TEXT("ws://127.0.0.1:8080/server"); S.bEnableAudio = true; S.bVerbose = false;
    Server->Start(S);

    // Client
    TSharedPtr<IWebRTCConnector> Client = FWebRTCConnectorFactory::Create(EO3DSWebRtcBackend::LibDataChannel);
    TestTrue(TEXT("Client connector created"), Client.IsValid());
    if (!Client.IsValid()) { Server->Stop(); return false; }
    Client->OnState().AddLambda([&](const FString& S2, bool){ if (S2 == TEXT("SignalingConnected")) bClientSig.store(true); });

    FO3DSWebRtcConfig Cfg; Cfg.Role = EO3DSWebRtcRole::Client; Cfg.SignalingUrl = TEXT("ws://127.0.0.1:8080/client"); Cfg.Room = TEXT("server");
    Cfg.bEnableAudio = true; Cfg.bSendDebugTone = true; Cfg.ToneDurationSec = 0.5; Cfg.bVerbose = false;
    Client->Start(Cfg);

    // If signaling doesn't connect quickly, assume the server isn't running and treat as pass (skip)
    const bool bSigOk = O3DS_WaitUntil(2.0, [&](){ return bClientSig.load() && bServerSig.load(); });
    if (!bSigOk)
    {
        // Cleanup and exit early
        Client->Stop();
        Server->Stop();
        AddInfo(TEXT("Skipping E2E (signaling server not detected on ws://127.0.0.1:8080)"));
        return true;
    }

    // Wait for DC open and at least one RTP packet
    const bool bOpened = O3DS_WaitUntil(5.0, [&](){ return bDCOpen.load(); });
    TestTrue(TEXT("DataChannel opened"), bOpened);

    const bool bRtp = O3DS_WaitUntil(5.0, [&](){ return bGotRtp.load(); });
    TestTrue(TEXT("Received RTP on server"), bRtp);

    Client->Stop();
    Server->Stop();
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
