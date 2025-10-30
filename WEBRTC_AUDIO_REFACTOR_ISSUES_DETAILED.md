# WebRTC Audio Refactor - Detailed Issue Specifications

**Created:** 2025-10-30  
**Source Documents:**
- `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md` (original refactor plan)
- `WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md` (executive summary)
- `WEBRTC_AUDIO_REFACTOR_PLANNING_COMPLETE.md` (planning overview)

**Reference Example:** https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp

This document contains complete, actionable specifications for the Epic and 7 sub-issues required to execute the WebRTC Audio Path Refactor. Each issue includes problem statement, required changes with file/line references, code examples, acceptance criteria, test plans, dependencies, and references.

---

## Issue 1: Unmask EnableAudioSend Failures in Adapter

### Title
`WebRTC Audio: Unmask EnableAudioSend failures in adapter`

### Labels
`area:unreal`, `area:webrtc`, `bug`, `audio`

### Priority
**High** (foundational fix)

### Effort Estimate
0.5 day

### Problem Statement

The `FLibDataChannelAdapter` in `WebRTCConnectorFactory.cpp` always returns `true` from `EnableAudioSend()` even when the inner connector rejects the call. This masks critical errors and prevents proper error handling upstream, making it impossible to diagnose why audio is not working.

**Root Cause:** The adapter caches success state and adds the stream to `EnabledStreams` regardless of the inner connector's actual return value. The calling code believes audio is enabled when it actually failed.

### Required Changes

**File:** `Source/Open3DStream/Private/WebRTCConnectorFactory.cpp`  
**Lines:** 46-63 (approximate location of `FLibDataChannelAdapter::EnableAudioSend`)

**Changes:**
1. Capture the actual boolean result from `Inner->EnableAudioSend(Config)`
2. Only add to `EnabledStreams` and emit announcements when the result is `true`
3. Add clear logging showing SUCCESS or FAILED with the stream label

**Current behavior (pseudocode):**
```cpp
bool FLibDataChannelAdapter::EnableAudioSend(const FAudioSendConfig& Config) {
    Inner->EnableAudioSend(Config);  // result ignored
    EnabledStreams.Add(Config.StreamLabel);
    // ... announce ...
    return true;  // always success!
}
```

**Required behavior:**
```cpp
bool FLibDataChannelAdapter::EnableAudioSend(const FAudioSendConfig& Config) {
    bool bSuccess = Inner->EnableAudioSend(Config);
    
    if (bSuccess) {
        EnabledStreams.Add(Config.StreamLabel);
        UE_LOG(LogWebRTC, Log, TEXT("[ADAPTER] EnableAudioSend(%s) -> SUCCESS"), 
               *Config.StreamLabel);
        // ... announce ...
    } else {
        UE_LOG(LogWebRTC, Warning, TEXT("[ADAPTER] EnableAudioSend(%s) -> FAILED: %s"), 
               *Config.StreamLabel, *Inner->GetLastError());
    }
    
    return bSuccess;
}
```

### Acceptance Criteria

1. ✅ When inner connector accepts `EnableAudioSend`, adapter returns `true` and logs SUCCESS
2. ✅ When inner connector rejects `EnableAudioSend` (e.g., called after Start), adapter returns `false` and logs FAILED with reason
3. ✅ Stream is only added to `EnabledStreams` when inner connector returns `true`
4. ✅ Calling code receives accurate success/failure information
5. ✅ Unit test added verifying rejection is propagated correctly

### Test Plan

**Unit Tests:**
1. Create mock connector that returns `false` from `EnableAudioSend`
2. Call adapter's `EnableAudioSend` and verify it returns `false`
3. Verify stream is NOT in `EnabledStreams`
4. Verify error log is emitted

**Integration Test:**
1. Start a transport (creates PeerConnection)
2. Call `EnableAudioSend` after Start
3. Verify it returns `false` with clear error message
4. Verify audio track is NOT added to PeerConnection

### Dependencies
None (foundational fix)

### References
- Original plan: `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md` section 6.1
- Adapter implementation: `Source/Open3DStream/Private/WebRTCConnectorFactory.cpp`
- Working example showing proper error handling: https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp

---

## Issue 2: Decouple Audio Capture Component

### Title
`WebRTC Audio: Make audio capture component a pure PCM source`

### Labels
`area:unreal`, `area:webrtc`, `refactor`, `audio`

### Priority
**High** (foundational refactor)

### Effort Estimate
1 day

### Problem Statement

`UO3DSBroadcastAudioCaptureComponent` currently owns a connector reference and participates in WebRTC negotiation (calling `EnsureConnector`, `SetConnector`, and `EnableAudioSend`). This creates fragile timing dependencies and race conditions because the component doesn't have full context about when the PeerConnection is created or what state the negotiation is in.

**Root Cause:** Mixed responsibilities. The component should be a simple PCM audio source that captures from mic/submix and forwards frames. It should NOT know about WebRTC, connectors, or negotiation.

### Required Changes

**Files:**
- `Source/Open3DStream/Public/O3DSBroadcastAudioCaptureComponent.h` (lines 92, 103-119)
- `Source/Open3DStream/Private/O3DSBroadcastAudioCaptureComponent.cpp` (lines 46-102, 195-257)

**Header Changes (O3DSBroadcastAudioCaptureComponent.h):**

Remove:
```cpp
// Lines ~103-119
UPROPERTY()
TScriptInterface<IWebRTCConnector> Connector;

void EnsureConnector();
void SetConnector(TScriptInterface<IWebRTCConnector> InConnector);
// ... any negotiation-related methods
```

Add:
```cpp
// New API - pure PCM source pattern
void SetStreamLabel(const FString& InLabel);
void SetAudioSink(TFunction<bool(const FString&, const float*, int32, int32, int32, double)> InSink);

private:
    FString StreamLabel;
    TFunction<bool(const FString&, const float*, int32, int32, int32, double)> AudioSink;
```

**Implementation Changes (O3DSBroadcastAudioCaptureComponent.cpp):**

Remove all connector-related code (lines ~46-102 for `EnsureConnector`/`SetConnector` and lines ~195-257 for negotiation calls).

Update `PushFrames` to use the sink:
```cpp
void UO3DSBroadcastAudioCaptureComponent::PushFrames(const float* AudioData, 
                                                      int32 NumFrames, 
                                                      int32 NumChannels, 
                                                      int32 SampleRate) {
    if (!AudioSink) {
        // No sink configured, silently drop frames
        return;
    }
    
    double Timestamp = FPlatformTime::Seconds();
    
    bool bSuccess = AudioSink(StreamLabel, AudioData, NumFrames, NumChannels, SampleRate, Timestamp);
    
    if (!bSuccess) {
        UE_LOG(LogO3DSBroadcast, Warning, 
               TEXT("[AudioCapture] Sink rejected frame (StreamLabel=%s, NumFrames=%d)"), 
               *StreamLabel, NumFrames);
    }
}
```

### Acceptance Criteria

1. ✅ Component compiles without `IWebRTCConnector` includes or member variables
2. ✅ No calls to connector methods (`EnableAudioSend`, `Start`, etc.) from this component
3. ✅ New `SetStreamLabel()` and `SetAudioSink()` methods are present and functional
4. ✅ `PushFrames()` successfully forwards audio data through the sink callback
5. ✅ Component logs show only capture/forwarding activity, never negotiation activity
6. ✅ Existing audio capture functionality (mic/submix tap) remains unchanged

### Test Plan

**Unit Tests:**
1. Create component, set a mock sink that captures calls
2. Call `PushFrames` with test data
3. Verify sink receives the data with correct parameters
4. Verify no WebRTC-related code is called

**Integration Test:**
1. Configure component with real sink connected to connector
2. Generate test audio from submix
3. Verify sink callback is invoked with proper PCM data
4. Verify connector receives the audio via `PushPcm`

**Compilation Test:**
1. Remove `#include "IWebRTCConnector.h"` from component header
2. Verify component still compiles

### Dependencies
None (but Issue 3 depends on this being complete)

### References
- Original plan: `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md` section 6.2
- Current implementation: `Source/Open3DStream/Private/O3DSBroadcastAudioCaptureComponent.cpp`
- Header: `Source/Open3DStream/Public/O3DSBroadcastAudioCaptureComponent.h`

---

## Issue 3: Centralize Audio Setup in BroadcastComponent

### Title
`WebRTC Audio: Wire audio before transport start in BroadcastComponent`

### Labels
`area:unreal`, `area:webrtc`, `refactor`, `audio`

### Priority
**High**

### Effort Estimate
1 day

### Dependencies
**Requires Issue #2 to be complete** (needs new AudioCaptureComponent API)

### Problem Statement

Audio setup is currently split across components with unclear ordering. We need a deterministic sequence where `BroadcastComponent` orchestrates the entire setup: compute the stream label, enable audio on the connector BEFORE starting the transport, wire the audio capture component, then start the transport so the PeerConnection is created with audio included.

**Root Cause:** No single owner of the setup sequence, leading to timing issues where audio configuration happens after the PeerConnection is already negotiating.

### Required Changes

**File:** `Source/Open3DBroadcast/Private/O3DSBroadcastComponent.cpp`  
**Lines:** 358-464 (approximate location of `StartCapture()` method)

**Required sequence in `StartCapture()`:**

```cpp
bool UO3DSBroadcastComponent::StartCapture() {
    // Step 1: Create transport (PrepareChannel) - no PeerConnection yet
    if (!PrepareInternalTransport()) {
        UE_LOG(LogO3DSBroadcast, Error, TEXT("Failed to prepare internal transport"));
        return false;
    }
    
    // Step 2: Get connector reference (PC doesn't exist yet)
    TScriptInterface<IWebRTCConnector> Connector = InternalTransport->GetConnector();
    if (!Connector) {
        UE_LOG(LogO3DSBroadcast, Error, TEXT("Failed to get connector from transport"));
        return false;
    }
    
    // Step 3: Compute StreamLabel once based on configuration
    FString StreamLabel;
    if (AudioCaptureMode == EAudioCaptureMode::Submix) {
        // For subject-associated audio
        StreamLabel = FString::Printf(TEXT("o3ds:subject/%s"), *SubjectName);
    } else if (AudioCaptureMode == EAudioCaptureMode::Microphone) {
        // For mic-based audio
        if (MicrophoneDeviceName.IsEmpty()) {
            StreamLabel = TEXT("o3ds:mic");
        } else {
            StreamLabel = FString::Printf(TEXT("o3ds:mic/%s"), *MicrophoneDeviceName);
        }
    }
    
    // Step 4: Call EnableAudioSend BEFORE Start (enforces ordering contract)
    FAudioSendConfig AudioConfig;
    AudioConfig.StreamLabel = StreamLabel;
    AudioConfig.SampleRate = 48000;
    AudioConfig.NumChannels = 2;
    AudioConfig.Bitrate = 128000;
    
    bool bAudioEnabled = Connector->EnableAudioSend(AudioConfig);
    if (!bAudioEnabled) {
        UE_LOG(LogO3DSBroadcast, Error, 
               TEXT("EnableAudioSend failed: %s"), *Connector->GetLastError());
        return false;
    }
    
    UE_LOG(LogO3DSBroadcast, Log, 
           TEXT("Audio enabled with StreamLabel=%s"), *StreamLabel);
    
    // Step 5: Wire AudioCaptureComponent with StreamLabel and sink callback
    if (!AudioCaptureComponent) {
        AudioCaptureComponent = NewObject<UO3DSBroadcastAudioCaptureComponent>(this);
    }
    
    AudioCaptureComponent->SetStreamLabel(StreamLabel);
    AudioCaptureComponent->SetAudioSink(
        [Connector](const FString& Label, const float* Data, int32 NumFrames, 
                    int32 NumChannels, int32 SampleRate, double Timestamp) -> bool {
            return Connector->PushPcm(Label, Data, NumFrames, NumChannels, SampleRate, Timestamp);
        }
    );
    
    // Configure capture source (mic or submix)
    if (AudioCaptureMode == EAudioCaptureMode::Microphone) {
        AudioCaptureComponent->SetMicrophoneCapture(MicrophoneDeviceName);
    } else {
        AudioCaptureComponent->SetSubmixCapture(AudioSubmixToCapture);
    }
    
    // Step 6: Start transport (PeerConnection created NOW with audio)
    bool bStarted = StartInternalTransport(SignalingUrl, TransportProtocol, ConnectionKey);
    if (!bStarted) {
        UE_LOG(LogO3DSBroadcast, Error, TEXT("Failed to start internal transport"));
        return false;
    }
    
    UE_LOG(LogO3DSBroadcast, Log, TEXT("Capture started successfully with audio"));
    return true;
}
```

### Acceptance Criteria

1. ✅ `PrepareInternalTransport` is called first (creates connector, no PC yet)
2. ✅ `StreamLabel` is computed correctly based on audio mode and subject/device name
3. ✅ `EnableAudioSend` is called BEFORE `StartInternalTransport`
4. ✅ `EnableAudioSend` failure aborts startup with clear error message
5. ✅ `AudioCaptureComponent` is configured with both `StreamLabel` and `AudioSink`
6. ✅ After `StartInternalTransport`, local SDP contains `m=audio` line
7. ✅ `o3ds.WebRTC.Audio.Status` shows: `AudioEnabled=1`, `TrackPresent=1`, `TrackOpen=1`
8. ✅ When audio is actively playing/captured, `SentPackets` counter increases

### Test Plan

**Integration Tests:**

Test Matrix (all combinations should work):
- **Role:** Client, Server
- **Audio Mode:** Submix (Mix), Microphone
- **Transport:** Non-negotiated DataChannel (default)

For each combination:
1. Start capture with audio enabled
2. Verify SDP includes `m=audio` and `a=rtpmap:111 opus`
3. Verify `o3ds.WebRTC.Audio.Status` reports track open
4. Generate test audio (play sound or use test mic input)
5. Verify `SentPackets` increases over time
6. Verify no errors in logs

**Error Case Tests:**
1. Force `EnableAudioSend` to fail (mock rejection)
2. Verify `StartCapture` returns false
3. Verify error log mentions `EnableAudioSend` failure
4. Verify transport is NOT started

### Dependencies
**Requires Issue #2:** Needs `SetStreamLabel()` and `SetAudioSink()` APIs on audio capture component

### References
- Original plan: `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md` section 6.3
- Current implementation: `Source/Open3DBroadcast/Private/O3DSBroadcastComponent.cpp`
- Working example showing correct ordering: https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp (lines 144-168)

---

## Issue 4: Enforce Strict Ordering in Connector

### Title
`WebRTC Audio: Enforce EnableAudioSend must be called before Start`

### Labels
`area:unreal`, `area:webrtc`, `refactor`, `audio`

### Priority
**High**

### Effort Estimate
1 day

### Problem Statement

The connector needs to enforce that `EnableAudioSend` is called BEFORE `Start()` creates the PeerConnection. Once the PC exists, you can't retroactively add media to the initial offer without manual renegotiation. The contract must be enforced programmatically to prevent misuse.

Additionally, `SetupPeerConnection` must follow the critical ordering: add audio track FIRST, then create DataChannel (which triggers offer generation). This is the pattern from the working libdatachannel example.

**Root Cause:** No enforcement of the ordering contract, allowing invalid call sequences that silently produce incorrect SDPs.

### Required Changes

**File:** `Source/Open3DStream/Private/WebRTCConnector.cpp`

#### Change 1: Enforce ordering in EnableAudioSend (~line 1601)

```cpp
bool FWebRTCConnector::EnableAudioSend(const FAudioSendConfig& Config) {
    // Must be called BEFORE Start() creates the PeerConnection
    if (PeerConnection) {
        FString Error = TEXT("EnableAudioSend must be called before Start()");
        UE_LOG(LogWebRTC, Error, TEXT("[CONNECTOR] %s"), *Error);
        SetLastError(Error);
        return false;
    }
    
    // Store config for later use in SetupPeerConnection
    AudioSendConfig = Config;
    bAudioSendEnabled = true;
    
    UE_LOG(LogWebRTC, Log, 
           TEXT("[CONNECTOR] EnableAudioSend(%s) -> configured for setup"), 
           *Config.StreamLabel);
    
    return true;
}
```

#### Change 2: Enforce ordering in SetupPeerConnection (~line 1195)

```cpp
void FWebRTCConnector::SetupPeerConnection() {
    check(PeerConnection);  // Should exist from Start()
    
    // CRITICAL: Add audio track BEFORE DataChannel
    // This ensures audio is included in the initial SDP offer
    if (bAudioSendEnabled) {
        UE_LOG(LogWebRTC, Log, 
               TEXT("[CONNECTOR] Adding audio track BEFORE DataChannel (StreamLabel=%s)"), 
               *AudioSendConfig.StreamLabel);
        
        SetupAudioTrackAndHandlers(AudioSendConfig, PeerConnection);
        
        // Verify track was added
        if (!AudioTrack) {
            UE_LOG(LogWebRTC, Error, TEXT("[CONNECTOR] Failed to create audio track"));
            SetLastError(TEXT("Failed to create audio track"));
            return;
        }
    }
    
    // Create DataChannel AFTER audio track (triggers offer generation)
    UE_LOG(LogWebRTC, Log, TEXT("[CONNECTOR] Creating DataChannel (will trigger offer)"));
    DataChannel = PeerConnection->createDataChannel("o3ds-data");
    
    // Setup handlers, offer will be created with audio included
    SetupDataChannelHandlers(DataChannel);
    
    // Verify SDP includes audio
    if (bAudioSendEnabled) {
        ValidateSDPIncludesAudio();
    }
}
```

#### Change 3: Add SDP validation

```cpp
void FWebRTCConnector::ValidateSDPIncludesAudio() {
    if (!PeerConnection) return;
    
    std::optional<rtc::Description> LocalDesc = PeerConnection->localDescription();
    if (!LocalDesc.has_value()) {
        UE_LOG(LogWebRTC, Verbose, TEXT("[CONNECTOR] Local SDP not yet set"));
        return;
    }
    
    std::string SDP = LocalDesc->generateSdp();
    FString SDPString = UTF8_TO_TCHAR(SDP.c_str());
    
    if (SDPString.Contains(TEXT("m=audio"))) {
        UE_LOG(LogWebRTC, Log, TEXT("[CONNECTOR] ✓ Local SDP includes m=audio"));
        
        if (SDPString.Contains(TEXT("opus/48000"))) {
            UE_LOG(LogWebRTC, Verbose, TEXT("[CONNECTOR] ✓ Opus codec present in SDP"));
        }
    } else {
        UE_LOG(LogWebRTC, Warning, 
               TEXT("[CONNECTOR] ✗ Local SDP does NOT include m=audio (audio may not work!)"));
    }
}
```

#### Change 4: Ensure bounded buffer in PushAudioPCM16

```cpp
bool FWebRTCConnector::PushAudioPCM16(const FString& StreamLabel, 
                                      const int16* InterleavedData, 
                                      int32 NumFrames, 
                                      int32 NumChannels, 
                                      int32 SampleRate) {
    if (!AudioTrack) {
        // Buffer up to 250ms while waiting for track to open
        return BufferAudio(InterleavedData, NumFrames, NumChannels);
    }
    
    if (!AudioTrack->isOpen()) {
        // Track exists but not yet open (negotiation in progress)
        return BufferAudio(InterleavedData, NumFrames, NumChannels);
    }
    
    // Encode with Opus and send
    // Ensure RTP timestamp progression (48kHz clock)
    uint32_t RtpTimestamp = CurrentRtpTimestamp;
    CurrentRtpTimestamp += NumFrames;  // Advance by frame count at 48kHz
    
    try {
        rtc::binary EncodedFrame = OpusEncoder->Encode(InterleavedData, NumFrames, NumChannels);
        AudioTrack->send(EncodedFrame, RtpTimestamp);
        
        SentPackets++;
        SentBytes += EncodedFrame.size();
        
        return true;
    } catch (const std::exception& e) {
        UE_LOG(LogWebRTC, Warning, 
               TEXT("[CONNECTOR] Exception in PushAudioPCM16: %s"), UTF8_TO_TCHAR(e.what()));
        return false;
    }
}
```

### Acceptance Criteria

1. ✅ Calling `EnableAudioSend` after `Start()` returns `false` and logs clear error
2. ✅ In `SetupPeerConnection`, audio track is added BEFORE DataChannel is created
3. ✅ Local SDP validation logs whether `m=audio` is present
4. ✅ When audio is enabled before start, track opens and `SentPackets` increases
5. ✅ `PushAudioPCM16` maintains bounded buffer (~250ms max) when track is not yet ready
6. ✅ RTP timestamp advances correctly based on frame count (at 48kHz)
7. ✅ Exception handling prevents crashes from Opus encoding errors

### Test Plan

**Unit Tests:**

1. **Ordering enforcement test:**
   - Create connector, call `Start()`, then call `EnableAudioSend()`
   - Verify returns `false` with error message

2. **Happy path test:**
   - Create connector, call `EnableAudioSend()`, then `Start()`
   - Verify audio track is created and present in SDP

3. **Buffer test:**
   - Call `PushAudioPCM16` before track opens
   - Verify data is buffered (returns true)
   - Once track opens, verify buffered data is sent

**Integration Tests:**

Test Matrix:
- Client and Server roles
- With audio enabled before Start
- Verify SDP logs show `m=audio` present
- Verify track opens and packets are sent

**Error Tests:**
1. Intentionally call `EnableAudioSend` after `Start`
2. Verify error is logged and audio is NOT added
3. Verify `o3ds.WebRTC.Audio.Status` shows `AudioEnabled=0`

### Dependencies
None (can be implemented independently, though works best with Issues 1-3)

### References
- Original plan: `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md` sections 6.4 and 9.4
- Current implementation: `Source/Open3DStream/Private/WebRTCConnector.cpp`
- Working example showing critical ordering: https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp lines 144-168

**Key reference from libdatachannel example:**
```cpp
// client.cpp lines 144-168
pc = createPeerConnection(config, ws);         // Step 1: Create PC
audioTrack = pc->addTrack(media);              // Step 2: AUDIO FIRST
dc = pc->createDataChannel("test");            // Step 3: DC SECOND (triggers offer)
```

---

## Issue 5: Polish Logging and Diagnostics

### Title
`WebRTC Audio: Improve logging and status diagnostics`

### Labels
`area:unreal`, `area:webrtc`, `enhancement`, `audio`

### Priority
**Medium**

### Effort Estimate
0.5 day

### Dependencies
**Requires Issues 1-4:** Diagnostics apply to the refactored system

### Problem Statement

After the refactor, we need comprehensive diagnostics to debug audio issues in the field. This includes standardized log categories, must-have log statements at key points, a comprehensive status command, and CVars to control verbosity.

**Goal:** Any developer should be able to run `o3ds.WebRTC.Audio.Status` and immediately understand the state of the audio system.

### Required Changes

#### Change 1: Standardize Log Categories

Ensure consistent use of these categories:
- `LogWebRTC` - Main WebRTC events
- `LogO3DSBroadcast` - Broadcast component orchestration
- `LogO3DSAudio` - Audio-specific processing

#### Change 2: Must-Have Log Statements

**In Adapter (WebRTCConnectorFactory.cpp):**
```cpp
UE_LOG(LogWebRTC, Log, TEXT("[ADAPTER] EnableAudioSend(%s) -> SUCCESS"), *Label);
UE_LOG(LogWebRTC, Warning, TEXT("[ADAPTER] EnableAudioSend(%s) -> FAILED: %s"), *Label, *Error);
```

**In Connector (WebRTCConnector.cpp):**
```cpp
UE_LOG(LogWebRTC, Log, TEXT("[CONNECTOR] Adding audio track BEFORE DataChannel (StreamLabel=%s)"), *Label);
UE_LOG(LogWebRTC, Log, TEXT("[CONNECTOR] ✓ Local SDP includes m=audio"));
UE_LOG(LogWebRTC, Warning, TEXT("[CONNECTOR] ✗ Local SDP does NOT include m=audio"));
UE_LOG(LogWebRTC, Log, TEXT("[CONNECTOR] Audio track opened (MID=%s)"), *MID);
UE_LOG(LogWebRTC, Log, TEXT("[CONNECTOR] Audio track closed"));
UE_LOG(LogWebRTC, Log, TEXT("[CONNECTOR] PeerConnection state: %s"), *StateString);
```

**In BroadcastComponent (O3DSBroadcastComponent.cpp):**
```cpp
UE_LOG(LogO3DSBroadcast, Log, TEXT("Audio enabled with StreamLabel=%s"), *StreamLabel);
UE_LOG(LogO3DSBroadcast, Error, TEXT("EnableAudioSend failed: %s"), *Error);
```

#### Change 3: Implement Comprehensive Status Command

Add console command `o3ds.WebRTC.Audio.Status` that prints:

```cpp
void FWebRTCConnector::PrintAudioStatus() {
    UE_LOG(LogWebRTC, Display, TEXT("=== WebRTC Audio Status ==="));
    
    // Basic state
    UE_LOG(LogWebRTC, Display, TEXT("AudioEnabled: %d"), bAudioSendEnabled ? 1 : 0);
    UE_LOG(LogWebRTC, Display, TEXT("StreamLabel: %s"), 
           bAudioSendEnabled ? *AudioSendConfig.StreamLabel : TEXT("N/A"));
    
    // Connection state
    if (PeerConnection) {
        UE_LOG(LogWebRTC, Display, TEXT("PeerConnection: %s"), 
               *PeerConnectionStateToString(PeerConnection->state()));
        UE_LOG(LogWebRTC, Display, TEXT("Signaling: %s"), 
               *SignalingStateToString(PeerConnection->signalingState()));
    } else {
        UE_LOG(LogWebRTC, Display, TEXT("PeerConnection: Not created"));
    }
    
    // Audio track state
    if (AudioTrack) {
        UE_LOG(LogWebRTC, Display, TEXT("TrackPresent: 1"));
        UE_LOG(LogWebRTC, Display, TEXT("TrackOpen: %d"), AudioTrack->isOpen() ? 1 : 0);
        if (AudioTrack->mid()) {
            UE_LOG(LogWebRTC, Display, TEXT("TrackMID: %s"), UTF8_TO_TCHAR(AudioTrack->mid()->c_str()));
        }
    } else {
        UE_LOG(LogWebRTC, Display, TEXT("TrackPresent: 0"));
        UE_LOG(LogWebRTC, Display, TEXT("TrackOpen: 0"));
    }
    
    // Opus state
    UE_LOG(LogWebRTC, Display, TEXT("OpusReady: %d"), OpusEncoder ? 1 : 0);
    
    // SDP analysis
    if (PeerConnection) {
        auto LocalDesc = PeerConnection->localDescription();
        auto RemoteDesc = PeerConnection->remoteDescription();
        
        if (LocalDesc.has_value()) {
            std::string SDP = LocalDesc->generateSdp();
            bool bHasAudio = SDP.find("m=audio") != std::string::npos;
            bool bHasOpus = SDP.find("opus/48000") != std::string::npos;
            UE_LOG(LogWebRTC, Display, TEXT("LocalSDP.m=audio: %d"), bHasAudio ? 1 : 0);
            UE_LOG(LogWebRTC, Display, TEXT("LocalSDP.opus: %d"), bHasOpus ? 1 : 0);
        }
        
        if (RemoteDesc.has_value()) {
            std::string SDP = RemoteDesc->generateSdp();
            bool bHasAudio = SDP.find("m=audio") != std::string::npos;
            UE_LOG(LogWebRTC, Display, TEXT("RemoteSDP.m=audio: %d"), bHasAudio ? 1 : 0);
        }
    }
    
    // Statistics
    UE_LOG(LogWebRTC, Display, TEXT("PendingSamples: %d"), PendingAudioBuffer.Num());
    UE_LOG(LogWebRTC, Display, TEXT("SentPackets: %llu"), SentPackets);
    UE_LOG(LogWebRTC, Display, TEXT("SentBytes: %llu"), SentBytes);
    
    // Errors
    if (!LastError.IsEmpty()) {
        UE_LOG(LogWebRTC, Display, TEXT("LastError: %s"), *LastError);
    }
    
    UE_LOG(LogWebRTC, Display, TEXT("========================="));
}
```

#### Change 4: Add Debug CVars

```cpp
// In WebRTCConnector module init
TAutoConsoleVariable<bool> CVarWebRTCAudioDebug(
    TEXT("o3ds.WebRTC.Audio.Debug"),
    false,
    TEXT("Enable verbose audio debugging logs"),
    ECVF_Default
);

TAutoConsoleVariable<bool> CVarWebRTCVerbose(
    TEXT("o3ds.WebRTC.Verbose"),
    false,
    TEXT("Enable verbose WebRTC logs (SDP, negotiation details)"),
    ECVF_Default
);

TAutoConsoleVariable<bool> CVarWebRTCDebugRx(
    TEXT("o3ds.WebRTC.DebugRx"),
    false,
    TEXT("Enable verbose receive path debugging"),
    ECVF_Default
);
```

Gate hot-path logs with these CVars:
```cpp
if (CVarWebRTCAudioDebug.GetValueOnGameThread()) {
    UE_LOG(LogWebRTC, Verbose, TEXT("[CONNECTOR] Encoded frame: %d bytes"), EncodedFrame.size());
}
```

### Acceptance Criteria

1. ✅ All key events (EnableAudioSend, track open/close, SDP validation) have clear log statements
2. ✅ Log categories are used consistently
3. ✅ `o3ds.WebRTC.Audio.Status` command exists and prints all required fields
4. ✅ Status output is readable and actionable
5. ✅ CVars allow controlling log verbosity without recompiling
6. ✅ Hot-path logs (per-frame encoding) are gated by debug CVars
7. ✅ Documentation added explaining how to use status command and CVars

### Test Plan

**Functional Tests:**

1. **Status command test:**
   - Start with audio disabled: verify status shows `AudioEnabled=0`
   - Enable audio and start: verify status shows enabled, track present/open, SDP has audio
   - Generate audio: verify `SentPackets` increases
   - Run status command multiple times, verify output is consistent

2. **Log verbosity test:**
   - Run with default settings: verify hot-path logs are NOT spammy
   - Enable `o3ds.WebRTC.Audio.Debug 1`: verify detailed logs appear
   - Disable: verify logs stop

3. **Error reporting test:**
   - Force an error (e.g., EnableAudioSend after Start)
   - Run status command: verify `LastError` field shows the error

**Documentation:**
- Add section to user guide explaining status command
- Add troubleshooting table: symptom → what to check in status output

### Dependencies
**Requires Issues 1-4:** Diagnostics apply to refactored code paths

### References
- Original plan: `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md` section 8
- Status implementation location: `Source/Open3DStream/Private/WebRTCConnector.cpp`

---

## Issue 6: Create Test Matrix

### Title
`WebRTC Audio: Implement comprehensive test matrix`

### Labels
`area:unreal`, `area:webrtc`, `testing`, `audio`

### Priority
**Medium**

### Effort Estimate
0.5 day

### Dependencies
**Requires Issues 1-5:** Tests validate the complete refactored system

### Problem Statement

We need a comprehensive test matrix to ensure the WebRTC audio refactor works in all supported configurations and handles error cases gracefully. Tests should cover unit-level validation (ordering, API contracts) and integration-level validation (end-to-end audio streaming).

### Required Tests

#### Unit Tests

**Test File:** `Tests/WebRTCAudioRefactorTests.cpp`

1. **Ordering Enforcement Test**
```cpp
TEST(WebRTCAudioTests, EnableAudioSendAfterStartReturnsFlase) {
    FWebRTCConnector Connector;
    Connector.Start("ws://localhost:8080", false);
    
    FAudioSendConfig Config;
    Config.StreamLabel = "test";
    bool bResult = Connector.EnableAudioSend(Config);
    
    EXPECT_FALSE(bResult);
    EXPECT_TRUE(Connector.GetLastError().Contains("before Start"));
}
```

2. **Adapter Failure Propagation Test**
```cpp
TEST(WebRTCAudioTests, AdapterPropagatesFailure) {
    MockWebRTCConnector* MockConnector = new MockWebRTCConnector();
    MockConnector->SetEnableAudioSendResult(false);
    
    FLibDataChannelAdapter Adapter(MockConnector);
    
    FAudioSendConfig Config;
    bool bResult = Adapter.EnableAudioSend(Config);
    
    EXPECT_FALSE(bResult);
    EXPECT_FALSE(Adapter.IsStreamEnabled(Config.StreamLabel));
}
```

3. **Audio Capture Component API Test**
```cpp
TEST(WebRTCAudioTests, AudioCaptureUsesProvidedSink) {
    UO3DSBroadcastAudioCaptureComponent* Component = NewObject<UO3DSBroadcastAudioCaptureComponent>();
    
    bool bSinkCalled = false;
    Component->SetStreamLabel("test-label");
    Component->SetAudioSink([&bSinkCalled](const FString& Label, const float* Data, 
                                            int32 NumFrames, int32 NumChannels, 
                                            int32 SampleRate, double Timestamp) {
        bSinkCalled = true;
        return true;
    });
    
    float TestData[480] = {0};
    Component->PushFrames(TestData, 480, 2, 48000);
    
    EXPECT_TRUE(bSinkCalled);
}
```

4. **StreamLabel Computation Test**
```cpp
TEST(WebRTCAudioTests, StreamLabelComputedCorrectly) {
    // Test submix mode
    FString Label1 = ComputeStreamLabel(EAudioCaptureMode::Submix, "TestSubject", "");
    EXPECT_EQ(Label1, "o3ds:subject/TestSubject");
    
    // Test mic mode with device
    FString Label2 = ComputeStreamLabel(EAudioCaptureMode::Microphone, "", "DefaultMic");
    EXPECT_EQ(Label2, "o3ds:mic/DefaultMic");
    
    // Test mic mode without device
    FString Label3 = ComputeStreamLabel(EAudioCaptureMode::Microphone, "", "");
    EXPECT_EQ(Label3, "o3ds:mic");
}
```

#### Integration Tests

**Test Matrix:**

| Role   | Audio Mode | Transport Mode      | Expected Result |
|--------|-----------|---------------------|-----------------|
| Client | Submix    | Non-negotiated DC   | ✓ Audio works   |
| Client | Mic       | Non-negotiated DC   | ✓ Audio works   |
| Server | Submix    | Non-negotiated DC   | ✓ Audio works   |
| Server | Mic       | Non-negotiated DC   | ✓ Audio works   |

**Test Template for Each Configuration:**
```cpp
TEST(WebRTCAudioIntegration, Client_Submix_NonNegotiated) {
    // Setup
    UO3DSBroadcastComponent* Broadcast = CreateBroadcastComponent();
    Broadcast->SetRole(EWebRTCRole::Client);
    Broadcast->SetAudioCaptureMode(EAudioCaptureMode::Submix);
    Broadcast->SetSubjectName("TestSubject");
    
    // Start capture
    bool bStarted = Broadcast->StartCapture();
    ASSERT_TRUE(bStarted);
    
    // Wait for connection
    WaitForCondition([&]() { 
        return Broadcast->GetConnectionState() == EWebRTCConnectionState::Connected; 
    }, 5.0);
    
    // Verify SDP
    FString LocalSDP = Broadcast->GetLocalSDP();
    EXPECT_TRUE(LocalSDP.Contains("m=audio"));
    EXPECT_TRUE(LocalSDP.Contains("opus/48000"));
    
    // Verify status
    FWebRTCAudioStatus Status = Broadcast->GetAudioStatus();
    EXPECT_TRUE(Status.bAudioEnabled);
    EXPECT_TRUE(Status.bTrackPresent);
    EXPECT_TRUE(Status.bTrackOpen);
    
    // Generate test audio
    GenerateTestAudio(Broadcast, 2.0); // 2 seconds
    
    // Verify packets sent
    EXPECT_GT(Status.SentPackets, 0);
}
```

#### Error Case Tests

1. **Late EnableAudioSend Test**
```cpp
TEST(WebRTCAudioErrors, EnableAudioSendAfterStartFails) {
    UO3DSBroadcastComponent* Broadcast = CreateBroadcastComponent();
    
    // Start without audio
    Broadcast->SetAudioEnabled(false);
    Broadcast->StartCapture();
    
    // Try to enable audio after start
    Broadcast->SetAudioEnabled(true);
    bool bResult = Broadcast->EnableAudio();
    
    EXPECT_FALSE(bResult);
    EXPECT_TRUE(Broadcast->GetLastError().Contains("before Start"));
}
```

2. **Missing Opus Test**
```cpp
TEST(WebRTCAudioErrors, MissingOpusCodecHandledGracefully) {
    // Force Opus encoder to be unavailable
    FWebRTCConnector::SetOpusAvailable(false);
    
    UO3DSBroadcastComponent* Broadcast = CreateBroadcastComponent();
    Broadcast->SetAudioEnabled(true);
    
    bool bStarted = Broadcast->StartCapture();
    
    // Should fail or log clear warning
    if (bStarted) {
        // Verify error is reported in status
        FWebRTCAudioStatus Status = Broadcast->GetAudioStatus();
        EXPECT_FALSE(Status.bOpusReady);
    }
}
```

3. **Invalid Config Test**
```cpp
TEST(WebRTCAudioErrors, InvalidConfigRejected) {
    FWebRTCConnector Connector;
    
    FAudioSendConfig Config;
    Config.StreamLabel = "";  // Invalid: empty label
    Config.SampleRate = 8000; // Invalid: unsupported rate
    
    bool bResult = Connector.EnableAudioSend(Config);
    
    EXPECT_FALSE(bResult);
    EXPECT_FALSE(Connector.GetLastError().IsEmpty());
}
```

### Test Documentation

Create `docs/WEBRTC_AUDIO_TESTING.md`:
```markdown
# WebRTC Audio Testing Guide

## Running Tests

### Unit Tests
```
RunTests.bat WebRTCAudioTests
```

### Integration Tests  
```
RunTests.bat WebRTCAudioIntegration
```

### Full Matrix
```
RunTests.bat WebRTCAudio*
```

## Manual Testing

### Test Setup
1. Launch two UE instances
2. Configure one as Client, one as Server
3. Enable audio on both
4. Start capture

### Verification Checklist
- [ ] Both local SDPs contain `m=audio`
- [ ] Status command shows `TrackOpen=1` on both
- [ ] `SentPackets` increases on both
- [ ] Audio playback works (if receive path implemented)

### Troubleshooting
See status command output:
```
o3ds.WebRTC.Audio.Status
```

If `AudioEnabled=0`: Check EnableAudioSend was called before Start  
If `TrackPresent=0`: Check SDP negotiation  
If `TrackOpen=0`: Check ICE connection state  
If `SentPackets=0`: Check audio capture is active
```

### Acceptance Criteria

1. ✅ All unit tests pass
2. ✅ All integration test matrix configurations pass
3. ✅ All error case tests verify proper failure handling
4. ✅ Test documentation exists explaining how to run tests
5. ✅ Manual testing checklist provided for QA
6. ✅ Tests are integrated into CI/CD pipeline

### Test Plan

**This IS the test plan** - implement the tests described above and ensure they all pass.

### Dependencies
**Requires Issues 1-5:** Tests validate the complete refactored system with all fixes

### References
- Original plan: `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md` section 9.6
- Test infrastructure: `Tests/` directory
- Working example to reference: https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/

---

## Issue 7: Cleanup and Documentation

### Title
`WebRTC Audio: Final cleanup and documentation`

### Labels
`area:unreal`, `area:webrtc`, `documentation`, `cleanup`

### Priority
**Low** (final polish)

### Effort Estimate
0.5 day

### Dependencies
**Requires Issues 1-6:** This is the final cleanup after all work is complete

### Problem Statement

After implementing the refactor, we need to remove dead code, update documentation to reflect the new architecture, and provide migration guidance for any existing users. This ensures the codebase is clean and maintainable going forward.

### Required Changes

#### Change 1: Remove Dead Code

**Files to clean:**
- `O3DSBroadcastAudioCaptureComponent.cpp`: Remove any commented-out connector/negotiation code
- `O3DSBroadcastComponent.cpp`: Remove old audio setup patterns if any remain
- `WebRTCConnector.cpp`: Remove unused flags or workarounds related to audio ordering

**Search for and remove:**
- `// TODO: audio refactor` comments
- Commented-out negotiation code in audio capture component
- Unused member variables related to old patterns

#### Change 2: Update Documentation

**Mark refactor complete:**

Update `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md`:
```markdown
**Status:** ✅ COMPLETE (Implemented 2025-10-30)

This refactor has been completed. The audio path now follows the deterministic pattern:
1. EnableAudioSend() before Start()
2. Audio track added before DataChannel
3. Audio capture component is a pure PCM source

See the following for current implementation details:
- [WebRTC Audio Testing Guide](WEBRTC_AUDIO_TESTING.md)
- [WebRTC Audio Migration Guide](WEBRTC_AUDIO_MIGRATION.md)
```

**Create user-facing quickstart:**

Update `plugins/unreal/Open3DStream/docs/WEBRTC_QUICKSTART.md` with audio setup:
```markdown
## Enabling Audio

To enable audio streaming:

1. Configure audio capture mode:
```cpp
BroadcastComponent->SetAudioCaptureMode(EAudioCaptureMode::Submix); // or Microphone
BroadcastComponent->SetAudioSubmixToCapture(YourSubmix);
```

2. Start capture (audio is automatically configured):
```cpp
bool bSuccess = BroadcastComponent->StartCapture();
```

3. Verify audio is working:
```
o3ds.WebRTC.Audio.Status
```

Look for:
- `AudioEnabled=1`
- `TrackOpen=1`
- `SentPackets` increasing

## Troubleshooting Audio

If audio is not working:

| Symptom | Check | Solution |
|---------|-------|----------|
| `AudioEnabled=0` | Audio config | Call SetAudioCaptureMode before StartCapture |
| `TrackPresent=0` | Ordering | Ensure EnableAudioSend was called before Start |
| `TrackOpen=0` | Connection | Check ICE connection state |
| `SentPackets=0` | Audio source | Verify submix/mic is producing audio |
```

**Update broadcast user guide:**

`plugins/unreal/Open3DStream/docs/BROADCAST_WEBRTC_USER_GUIDE.md`:
- Add section on audio setup
- Reference status command
- Link to troubleshooting guide

**Create migration guide:**

`plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_MIGRATION.md`:
```markdown
# WebRTC Audio Refactor Migration Guide

## Summary

The WebRTC audio path was refactored in 2025-10-30 to improve reliability and debuggability.

## Breaking Changes

### Audio Capture Component API

**Old API (removed):**
```cpp
AudioCaptureComponent->SetConnector(Connector);
AudioCaptureComponent->EnsureConnector();
```

**New API:**
```cpp
AudioCaptureComponent->SetStreamLabel("o3ds:subject/MySubject");
AudioCaptureComponent->SetAudioSink([Connector](const FString& Label, ...) {
    return Connector->PushPcm(...);
});
```

### Connector Ordering Contract

**New requirement:** `EnableAudioSend()` MUST be called before `Start()`.

```cpp
// ❌ WRONG (will fail)
Connector->Start();
Connector->EnableAudioSend(Config);

// ✅ CORRECT
Connector->EnableAudioSend(Config);
Connector->Start();
```

### Adapter Behavior

**Old:** `EnableAudioSend()` always returned true (masked failures)  
**New:** Returns false when inner connector rejects the call

Check return value:
```cpp
if (!Adapter->EnableAudioSend(Config)) {
    UE_LOG(LogYourModule, Error, TEXT("Failed: %s"), *Adapter->GetLastError());
}
```

## Migration Steps

If you have custom code calling audio APIs:

1. **Update audio capture component usage:**
   - Remove `SetConnector` calls
   - Add `SetStreamLabel` and `SetAudioSink` calls
   - Let BroadcastComponent orchestrate setup

2. **Check EnableAudioSend timing:**
   - Ensure called before `Start()`
   - Handle failure return value

3. **Update error handling:**
   - Adapter now propagates failures
   - Check and log `GetLastError()` on failure

## Testing

After migration:
1. Run `o3ds.WebRTC.Audio.Status` console command
2. Verify `AudioEnabled=1` and `TrackOpen=1`
3. Verify `SentPackets` increases when audio is active

## Questions?

See [WEBRTC_AUDIO_TESTING.md](WEBRTC_AUDIO_TESTING.md) for detailed troubleshooting.
```

#### Change 3: Update Code Comments

Add clear API documentation in headers:

**IWebRTCConnector.h:**
```cpp
/**
 * Enable audio sending on this connector.
 * 
 * MUST be called BEFORE Start() creates the PeerConnection.
 * Once Start() is called, the PeerConnection is created and
 * audio cannot be added to the initial offer without renegotiation.
 * 
 * @param Config Audio configuration including StreamLabel, sample rate, channels
 * @return true if audio was successfully enabled, false if called too late or invalid config
 * @see GetLastError() for failure reason
 */
virtual bool EnableAudioSend(const FAudioSendConfig& Config) = 0;
```

**O3DSBroadcastAudioCaptureComponent.h:**
```cpp
/**
 * Audio capture component - pure PCM source.
 * 
 * This component captures audio from microphone or submix and forwards
 * PCM frames to a provided sink. It does NOT participate in WebRTC
 * negotiation or own transport connections.
 * 
 * Usage:
 *   1. SetStreamLabel("o3ds:mic")
 *   2. SetAudioSink([Connector] { return Connector->PushPcm(...); })
 *   3. SetMicrophoneCapture() or SetSubmixCapture()
 *   4. Frames automatically forwarded to sink
 */
UCLASS()
class UO3DSBroadcastAudioCaptureComponent : public UActorComponent {
    // ...
};
```

#### Change 4: Update CHANGELOG

Add entry to `CHANGELOG.md`:
```markdown
## [X.Y.Z] - 2025-10-30

### Changed - WebRTC Audio Path Refactor

#### Schema/Protocol
- Audio track is now consistently included in SDP when audio is enabled
- No protocol format changes

#### API Changes (Breaking)
- `UO3DSBroadcastAudioCaptureComponent`: Removed `SetConnector()` and `EnsureConnector()` methods
  - Added: `SetStreamLabel()` and `SetAudioSink()` - component is now a pure PCM source
- `IWebRTCConnector::EnableAudioSend()`: Now enforces MUST-be-before-Start contract
  - Returns false (instead of silently succeeding) when called after Start()
- `FLibDataChannelAdapter::EnableAudioSend()`: Now propagates failures from inner connector
  - No longer masks errors - returns actual result

#### Compatibility & Migration
- **Existing code using BroadcastComponent is NOT affected** - setup is automatic
- **Custom code calling audio APIs directly:** See `docs/WEBRTC_AUDIO_MIGRATION.md`
- **Testing:** Use `o3ds.WebRTC.Audio.Status` console command for diagnostics

#### Internal Changes
- Audio setup centralized in `UO3DSBroadcastComponent::StartCapture()`
- Audio track added before DataChannel in all cases (ensures inclusion in SDP offer)
- Comprehensive logging and diagnostics added
- Test matrix covering all supported configurations
```

#### Change 5: Update README

`README.md` - update features section:
```markdown
### Features

- **WebRTC Audio Streaming** (✅ Stable)
  - Opus codec support for efficient audio transmission
  - Microphone and submix capture modes
  - Deterministic setup with comprehensive diagnostics
  - See [WebRTC Audio Guide](plugins/unreal/Open3DStream/docs/WEBRTC_QUICKSTART.md#enabling-audio)
```

### Acceptance Criteria

1. ✅ All commented-out dead code removed
2. ✅ `WEBRTC_AUDIO_REFACTOR.md` marked as complete with links to new docs
3. ✅ `WEBRTC_QUICKSTART.md` includes audio setup section
4. ✅ `BROADCAST_WEBRTC_USER_GUIDE.md` updated with audio configuration
5. ✅ `WEBRTC_AUDIO_MIGRATION.md` created with clear breaking changes and migration steps
6. ✅ API documentation comments added to key interfaces
7. ✅ CHANGELOG.md updated with complete refactor summary
8. ✅ README.md features section updated
9. ✅ All documentation is internally consistent and cross-linked

### Test Plan

**Documentation Review:**
1. Read through all updated docs as if you were a new user
2. Verify all links work
3. Verify code examples compile
4. Verify troubleshooting steps are actionable

**Code Cleanliness:**
1. Search for `// TODO: audio refactor` - should be zero results
2. Search for `#if 0` blocks - should be removed if audio-related
3. Run static analysis - no new warnings

**Integration Verification:**
1. Follow quickstart guide from scratch
2. Verify audio setup works as documented
3. Run status command and verify output matches documentation
4. Break something intentionally and verify troubleshooting guide helps

### Dependencies
**Requires Issues 1-6:** All previous work must be complete before final cleanup

### References
- Original plan: `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md` section 9.7
- Documentation locations: `plugins/unreal/Open3DStream/docs/`
- CHANGELOG: `CHANGELOG.md`

---

## Issue 8: Epic - WebRTC Audio Path Refactor

### Title
`[EPIC] WebRTC Audio Path Refactor`

### Labels
`epic`, `area:unreal`, `area:webrtc`, `audio`

### Description

This Epic tracks the complete refactor of the WebRTC audio path to address silent failures, fragile timing, and unclear ordering in audio setup. The refactor follows the proven pattern from the libdatachannel audio-comm-test example.

### Goals

1. **Unmask Failures:** Make errors visible so they can be diagnosed and fixed
2. **Decouple Components:** Clear separation of concerns (capture vs. negotiation)
3. **Centralize Setup:** Deterministic ordering in BroadcastComponent
4. **Enforce Contracts:** Programmatic enforcement of API ordering requirements

### Background

**Problem:** Audio track sometimes missing from SDP, setup fails silently, hard to debug.

**Root Causes:**
- Adapter masks connector failures → system thinks audio enabled when it's not
- AudioCaptureComponent participates in negotiation → timing hazards
- Setup order not enforced → audio track can be added after offer is generated
- Poor diagnostics → hard to debug audio issues in the field

**Solution Pattern** (from working libdatachannel example):
```cpp
pc = createPeerConnection(config, ws);         // 1. Create PC
audioTrack = pc->addTrack(media);              // 2. Audio FIRST
dc = pc->createDataChannel("test");            // 3. DC SECOND (triggers offer)
```

This ensures audio track is included in the initial SDP offer.

### Sub-Issues

#### Phase A: Unmask and Contain (1.5 days)
- [ ] #XX - Unmask EnableAudioSend failures in adapter
- [ ] #YY - Decouple audio capture component from WebRTC negotiation

#### Phase B: Centralize and Order (2 days)
- [ ] #ZZ - Wire audio before transport start in BroadcastComponent (depends on #YY)
- [ ] #AA - Enforce strict ordering in WebRTCConnector

#### Phase C: Diagnostics and Tests (1 day)
- [ ] #BB - Polish logging and status diagnostics (depends on #XX-#AA)
- [ ] #CC - Create comprehensive test matrix (depends on #XX-#BB)

#### Phase D: Cleanup (0.5 day)
- [ ] #DD - Final cleanup and documentation (depends on all)

### Implementation Strategy

**Rollout:**
1. **PR 1:** Issue #XX (adapter fix) - Quick win, immediate value
2. **PR 2:** Issues #YY + #ZZ (decouple + centralize) - Core refactor
3. **PR 3:** Issue #AA (enforce ordering) - Contract hardening
4. **PR 4:** Issues #BB + #CC + #DD (diagnostics + tests + cleanup) - Polish

Each PR compiles independently and provides incremental value.

### Success Criteria

After all issues complete:
- ✅ Audio track consistently present in SDP when audio is enabled
- ✅ `EnableAudioSend` after Start fails loudly (returns false, logs error)
- ✅ AudioCaptureComponent has zero WebRTC negotiation code
- ✅ Setup sequence is deterministic and well-logged
- ✅ `o3ds.WebRTC.Audio.Status` command provides comprehensive diagnostics
- ✅ Test matrix passes for all configurations (Client/Server × Mix/Mic)
- ✅ Documentation complete and accurate

### Timeline

**Estimated:** 3 days (5 days unbuffered with 40% buffer)
- Phase A: 1.5 days (parallel or sequential)
- Phase B: 2 days (#ZZ depends on #YY, but #AA can be parallel)
- Phase C: 1 day (sequential)
- Phase D: 0.5 day (final)

### References

- **Refactor Plan:** `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md`
- **Working Example:** https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp
- **Current Implementation:**
  - Adapter: `Source/Open3DStream/Private/WebRTCConnectorFactory.cpp`
  - Audio Capture: `Source/Open3DStream/Private/O3DSBroadcastAudioCaptureComponent.cpp`
  - Connector: `Source/Open3DStream/Private/WebRTCConnector.cpp`
  - Broadcast: `Source/Open3DBroadcast/Private/O3DSBroadcastComponent.cpp`

### Notes

This Epic provides the framework for tracking the refactor. Once sub-issues are created, link them above and check them off as they're completed. Each sub-issue contains detailed specifications with file paths, line numbers, code examples, and acceptance criteria.

---
