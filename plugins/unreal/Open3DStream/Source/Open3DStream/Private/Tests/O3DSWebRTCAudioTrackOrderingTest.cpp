// Copyright (c) Open3DStream Contributors
// Test to verify audio track + data channel ordering fix

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"

#include <atomic>
#include <rtc/rtc.hpp>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * This test validates the critical fix for WebRTC audio track negotiation.
 * 
 * PROBLEM: Audio tracks were failing to open because they were added AFTER
 * data channel creation, causing them to be omitted from the initial SDP offer.
 * 
 * SOLUTION: Audio tracks must be added BEFORE data channel creation.
 * 
 * This test creates a minimal in-process WebRTC connection with both an audio
 * track and a data channel, verifying that both negotiate successfully when
 * the audio track is added first.
 * 
 * Reference: libdatachannel examples/audio-comm-test/
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSWebRTC_InProc_AudioTrackAndDataChannel,
    "Open3DStream.WebRTC.InProc.AudioTrackAndDataChannel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSWebRTC_InProc_AudioTrackAndDataChannel::RunTest(const FString& Params)
{
    std::atomic<bool> aConnected{false}, bConnected{false};
    std::atomic<bool> audioTrackOpened{false};
    std::atomic<bool> dataChannelOpened{false};
    std::atomic<bool> offerCreated{false}, answerCreated{false};
    std::atomic<bool> gotAudioTrack{false};
    std::atomic<bool> gotDataMessage{false};

    auto cfg = std::make_shared<rtc::Configuration>();
    auto A = std::make_shared<rtc::PeerConnection>(*cfg);  // Client/Sender
    auto B = std::make_shared<rtc::PeerConnection>(*cfg);  // Server/Receiver

    // Wire ICE and SDP exchange directly between peers (no network/signaling)
    A->onLocalDescription([&offerCreated, B](rtc::Description desc){ 
        offerCreated = true;
        B->setRemoteDescription(std::move(desc)); 
    });
    
    B->onLocalDescription([&answerCreated, A](rtc::Description desc){ 
        answerCreated = true;
        A->setRemoteDescription(std::move(desc)); 
    });
    
    A->onLocalCandidate([B](rtc::Candidate c){ B->addRemoteCandidate(std::move(c)); });
    B->onLocalCandidate([A](rtc::Candidate c){ A->addRemoteCandidate(std::move(c)); });

    A->onStateChange([&aConnected](rtc::PeerConnection::State s){ 
        if (s == rtc::PeerConnection::State::Connected) aConnected = true; 
    });
    
    B->onStateChange([&bConnected](rtc::PeerConnection::State s){ 
        if (s == rtc::PeerConnection::State::Connected) bConnected = true; 
    });

    // Receiver: Listen for incoming audio track
    B->onTrack([&gotAudioTrack](std::shared_ptr<rtc::Track> track){
        if (track) {
            gotAudioTrack = true;
        }
    });

    // Receiver: Listen for incoming data channel
    B->onDataChannel([&dataChannelOpened, &gotDataMessage](std::shared_ptr<rtc::DataChannel> dc){
        dc->onOpen([&dataChannelOpened](){ 
            dataChannelOpened = true; 
        });
        dc->onMessage([&gotDataMessage](rtc::message_variant msg){
            if (auto *p = std::get_if<rtc::binary>(&msg)) {
                std::string s(reinterpret_cast<const char*>(p->data()), p->size());
                if (s == "TEST") gotDataMessage = true;
            }
        });
    });

    // CRITICAL ORDER: Add audio track FIRST, then create data channel
    // This is the fix validated by this test!
    
    // Step 1: Add audio track
    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
    audio.addOpusCodec(111);
    audio.addSSRC(0xA17C0001u, "o3ds", "o3ds:mix", "o3ds:mix");
    auto audioTrack = A->addTrack(audio);
    if (!audioTrack) {
        AddError(TEXT("Failed to add audio track"));
        return false;
    }
    
    audioTrack->onOpen([&audioTrackOpened]() {
        audioTrackOpened = true;
    });
    
    // Step 2: Create data channel AFTER audio track
    auto dc = A->createDataChannel("o3ds");
    if (!dc) {
        AddError(TEXT("Failed to create data channel"));
        return false;
    }

    // Create offer/answer
    try {
        A->setLocalDescription();
    } catch (const std::exception& e) {
        AddError(FString::Printf(TEXT("setLocalDescription() threw: %s"), UTF8_TO_TCHAR(e.what())));
        return false;
    }

    // Wait for offer creation
    const double startOffer = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - startOffer) < 3.0 && !offerCreated.load()) {
        FPlatformProcess::Sleep(0.01f);
    }
    TestTrue(TEXT("Offer created"), offerCreated.load());
    if (!offerCreated.load()) return false;

    // Wait for answer creation
    const double startAnswer = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - startAnswer) < 3.0 && !answerCreated.load()) {
        FPlatformProcess::Sleep(0.01f);
    }
    TestTrue(TEXT("Answer created"), answerCreated.load());
    if (!answerCreated.load()) return false;

    // Wait for connection establishment
    const double startConn = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - startConn) < 5.0 && 
           (!aConnected.load() || !bConnected.load())) {
        FPlatformProcess::Sleep(0.01f);
    }
    TestTrue(TEXT("Both peers connected"), aConnected.load() && bConnected.load());

    // Wait for audio track to be detected on receiver
    const double startAudio = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - startAudio) < 5.0 && !gotAudioTrack.load()) {
        FPlatformProcess::Sleep(0.01f);
    }
    TestTrue(TEXT("Receiver detected audio track"), gotAudioTrack.load());

    // Wait for data channel to open
    const double startDC = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - startDC) < 5.0 && !dataChannelOpened.load()) {
        FPlatformProcess::Sleep(0.01f);
    }
    TestTrue(TEXT("Data channel opened"), dataChannelOpened.load());

    // Send a test message over data channel
    if (dataChannelOpened.load()) {
        dc->send(std::string("TEST"));
        const double startMsg = FPlatformTime::Seconds();
        while ((FPlatformTime::Seconds() - startMsg) < 2.0 && !gotDataMessage.load()) {
            FPlatformProcess::Sleep(0.01f);
        }
        TestTrue(TEXT("Received test message over data channel"), gotDataMessage.load());
    }

    // Verify both audio track and data channel work simultaneously
    const bool bothWork = gotAudioTrack.load() && dataChannelOpened.load();
    TestTrue(TEXT("Both audio track and data channel negotiated successfully"), bothWork);

    return bothWork;
}

#endif // WITH_DEV_AUTOMATION_TESTS
