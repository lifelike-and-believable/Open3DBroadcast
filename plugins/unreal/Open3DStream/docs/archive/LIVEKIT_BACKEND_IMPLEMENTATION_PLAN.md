# LiveKit Backend Implementation Plan

**Document Status:** Planning Phase  
**Target Milestone:** M3.4.1b  
**Related Issues:** #90, #94  
**Owner:** Open3DStream Team  
**Last Updated:** 2025-11-03

---

## Executive Summary

This document provides a comprehensive implementation plan for adding LiveKit SFU backend support to Open3DStream's WebRTC connectivity layer. The implementation will enable scalable 1-to-many and many-to-many broadcast of real-time animation data and audio over the internet, while maintaining backward compatibility with the existing libdatachannel peer-to-peer implementation.

### Goals
- Implement LiveKit SFU backend alongside existing LibDataChannel P2P backend
- Enable scalable multi-party animation and audio streaming
- Maintain behavior parity between backends for transparent operation
- Ensure zero impact on existing P2P functionality
- Provide clear testing and deployment documentation

### Non-Goals
- Replacing or deprecating LibDataChannel backend
- Modifying Broadcaster or Receiver component APIs
- Adding new animation data formats or protocols
- Implementing HLS/RTMP egress (Phase 2 feature)

---

## Architecture Overview

### Current State

The Open3DStream WebRTC implementation currently uses:
- **Interface:** `IWebRTCConnector` - backend-agnostic connector interface
- **Implementation:** `FLibDataChannelConnector` - libdatachannel-based P2P
- **Factory:** `FWebRTCConnectorFactory::Create()` - instantiates connectors
- **Modules:** All WebRTC code lives in `Open3DShared` module
- **Signaling:** Custom WebSocket-based signaling for P2P offer/answer
- **Data Transport:** SCTP DataChannels (ordered/unordered, reliable/unreliable)
- **Audio Transport:** Opus RTP tracks with subject labeling

### Target State

Add LiveKit backend while preserving existing architecture:
- **New Implementation:** `FLiveKitConnector` implementing `IWebRTCConnector`
- **SDK Integration:** LiveKit C++ client SDK as thirdparty dependency
- **Build System:** Conditional compilation via `O3DS_ENABLE_LIVEKIT` flag
- **Signaling:** LiveKit's native WebSocket signaling with JWT authentication
- **Data Transport:** LiveKit data messages (reliable/lossy) mapped to DataChannel semantics
- **Audio Transport:** LiveKit audio tracks with same subject labeling conventions
- **Factory Update:** Create FLiveKitConnector when backend = LiveKit

### Key Constraints

Per Open3DStream Agent Playbook:
1. **Module Isolation:** All code in `Open3DShared` - no dependencies on `Open3DStream` or `Open3DBroadcast`
2. **Interface Contract:** Must implement full `IWebRTCConnector` interface
3. **Backward Compatibility:** Existing LibDataChannel users see no changes
4. **Build Independence:** Plugin builds without LiveKit SDK when `O3DS_ENABLE_LIVEKIT=0`
5. **No UI Changes:** Backend selection already exists in Broadcast Component and LiveLink Source
6. **Testing First:** Validate via `UO3DSWebRTCConnectorComponent` before integration

---

## Phase 1: SDK Selection and Integration

### 1.1 SDK Evaluation

**Candidate SDKs:**

**Option A: Official LiveKit C++ SDK**
- **Repository:** https://github.com/livekit/client-sdk-cpp
- **Status:** Work in progress, experimental
- **Pros:** 
  - Official support from LiveKit team
  - Direct access to latest protocol features
  - Community contributions encouraged
- **Cons:**
  - Not production-ready (as stated in repo)
  - Limited documentation
  - API may be unstable
- **Dependencies:** libwebrtc, protobuf, libwebsockets

**Option B: Community SDK (zesun96)**
- **Repository:** https://github.com/zesun96/livekit-client-cpp
- **Status:** Active community project
- **Pros:**
  - More mature implementation
  - Cross-platform support (Windows, Linux, macOS, iOS, Android)
  - Examples for audio/video publishing
  - E2EE support included
- **Cons:**
  - Community-maintained (support depends on maintainer)
  - May lag behind latest LiveKit features
- **Dependencies:** libwebrtc, protobuf, nlohmann_json, plog, libwebsockets

**Recommendation:** Start with **Option B (zesun96)** for the following reasons:
1. More complete implementation with working examples
2. Better documentation and code maturity
3. Proven cross-platform support
4. Can migrate to official SDK when it reaches production readiness
5. Both use similar APIs (WebRTC-based), making future migration feasible

### 1.2 Dependency Integration Strategy

**Approach: Git Submodule**

Add LiveKit SDK as a submodule under `thirdparty/livekit-cpp-client`:

```bash
# Add submodule
git submodule add https://github.com/zesun96/livekit-client-cpp.git thirdparty/livekit-cpp-client

# Initialize recursive dependencies
git submodule update --init --recursive
```

**Alternative Approach: Vendor SDK**

If submodule approach creates build complexity:
1. Build LiveKit SDK externally with correct UE-compatible flags
2. Vendor pre-built binaries and headers into `plugins/unreal/Open3DStream/ThirdParty/livekit/`
3. Similar to how libdatachannel is currently integrated

**Recommendation:** Use **Git Submodule** initially for easier SDK updates, but be prepared to vendor if build integration proves complex.

### 1.3 Build System Integration

**File:** `plugins/unreal/Open3DStream/Source/Open3DShared/Open3DShared.Build.cs`

**Changes Required:**

```csharp
// Add build flag
bool bEnableLiveKit = System.Environment.GetEnvironmentVariable("O3DS_ENABLE_LIVEKIT") == "1";
PublicDefinitions.Add($"O3DS_ENABLE_LIVEKIT={(bEnableLiveKit ? "1" : "0")}");

if (bEnableLiveKit)
{
    string LiveKitDir = System.IO.Path.Combine(ThirdPartyDir, "livekit");
    
    // Include paths
    PublicIncludePaths.Add(System.IO.Path.Combine(LiveKitDir, "include"));
    
    // Platform-specific linking
    if (Target.Platform == UnrealTargetPlatform.Win64)
    {
        PublicAdditionalLibraries.Add(System.IO.Path.Combine(LiveKitDir, "livekit-client.lib"));
        // Add additional dependencies (libwebrtc, protobuf, etc.)
    }
    else if (Target.Platform == UnrealTargetPlatform.Linux)
    {
        PublicAdditionalLibraries.Add(System.IO.Path.Combine(LiveKitDir, "liblivekit-client.a"));
    }
    // macOS support similar to Linux
}
```

**Dependencies to Link:**
- livekit-client (core SDK)
- libwebrtc (WebRTC implementation)
- protobuf (protocol buffers for LiveKit signaling)
- libwebsockets (WebSocket for signaling)
- SSL/crypto libraries (if not already linked)

### 1.4 Conditional Compilation

Use preprocessor guards throughout:

```cpp
#if O3DS_ENABLE_LIVEKIT
    #include <livekit/room.h>
    #include <livekit/participant.h>
    #include <livekit/track.h>
    // LiveKit-specific code
#endif
```

Factory implementation:

```cpp
TSharedPtr<IWebRTCConnector> FWebRTCConnectorFactory::Create(EO3DSWebRtcBackend Backend)
{
    switch (Backend)
    {
    case EO3DSWebRtcBackend::LibDataChannel:
        return MakeShared<FLibDataChannelConnector>();
    case EO3DSWebRtcBackend::LiveKit:
#if O3DS_ENABLE_LIVEKIT
        return MakeShared<FLiveKitConnector>();
#else
        UE_LOG(LogOpen3DShared, Warning, TEXT("LiveKit backend not available (O3DS_ENABLE_LIVEKIT=0)"));
        return nullptr;
#endif
    default:
        return nullptr;
    }
}
```

---

## Phase 2: Core Connector Implementation

### 2.1 Class Structure

**File:** `plugins/unreal/Open3DStream/Source/Open3DShared/Private/LiveKitConnector.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "IWebRTCConnector.h"

#if O3DS_ENABLE_LIVEKIT

#include <livekit/room.h>
#include <livekit/participant.h>
#include <livekit/track.h>
#include <memory>
#include <atomic>
#include <mutex>
#include <deque>

class FLiveKitConnector : public IWebRTCConnector
{
public:
    FLiveKitConnector() = default;
    virtual ~FLiveKitConnector() override;

    // IWebRTCConnector interface
    virtual bool Start(const FO3DSWebRtcConfig& Config) override;
    virtual void Stop() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool IsOpen() const override;
    virtual bool Send(const uint8* Data, int32 NumBytes) override;
    virtual bool EnableAudioSend(bool bEnable) override;
    virtual bool SendAudioPcm16(const int16* Samples, int32 NumSamples, int32 SampleRate, int32 NumChannels) override;

    virtual FO3DSOnWebRtcState& OnState() override { return StateDelegate; }
    virtual FO3DSOnWebRtcData& OnData() override { return DataDelegate; }
    virtual FO3DSOnWebRtcRtp& OnRemoteAudioRtp() override { return RtpDelegate; }

private:
    // LiveKit SDK objects
    std::shared_ptr<livekit::Room> LiveKitRoom;
    std::shared_ptr<livekit::LocalAudioTrack> LocalAudioTrack;
    std::shared_ptr<livekit::LocalDataTrack> LocalDataTrack;
    
    // Configuration
    FO3DSWebRtcConfig Config;
    
    // State
    std::atomic<bool> bStarted{false};
    std::atomic<bool> bConnected{false};
    std::atomic<bool> bDataChannelOpen{false};
    
    // Audio encoding (similar to LibDataChannel)
    std::mutex AudioMutex;
    std::deque<FAudioChunk> AudioQueue;
    // Opus encoder state
    
    // Delegates
    FO3DSOnWebRtcState StateDelegate;
    FO3DSOnWebRtcData DataDelegate;
    FO3DSOnWebRtcRtp RtpDelegate;
    
    // Helper methods
    void ConnectToRoom();
    void HandleConnectionStateChange(livekit::ConnectionState State);
    void HandleRemoteDataReceived(const uint8_t* Data, size_t Size);
    void HandleRemoteAudioReceived(std::shared_ptr<livekit::RemoteAudioTrack> Track);
    void PublishLocalAudio();
    void PublishLocalData();
};

#endif // O3DS_ENABLE_LIVEKIT
```

### 2.2 Connection Flow

**Start() Method:**

1. Parse and validate config (ServerUrl, Room, Token required)
2. Create LiveKit Room instance with token authentication
3. Connect to room asynchronously
4. Register event callbacks:
   - OnConnected → publish local tracks
   - OnDisconnected → update state
   - OnParticipantConnected → handle new subscribers
   - OnDataReceived → forward to DataDelegate
   - OnTrackSubscribed → handle remote audio
5. Set bStarted = true
6. Return success/failure

**Connection Sequence:**

```
Client                  LiveKit SFU
  |                          |
  |-- WebSocket Connect ---->|
  |                          |
  |<--- Challenge (JWT) -----|
  |                          |
  |-- Token Auth ----------->|
  |                          |
  |<--- Join Response -------|
  |                          |
  |-- Publish Tracks ------->| (local audio, data channel)
  |                          |
  |<--- Subscriptions -------|  (remote participants)
  |                          |
```

### 2.3 Data Channel Mapping

LiveKit uses **data messages** instead of SCTP DataChannels. Mapping:

| IWebRTCConnector Method | LiveKit Implementation | Topic | Kind |
|-------------------------|------------------------|-------|------|
| Send() (default) | PublishData() | `o3ds.anim` | lossy |
| Send() (control) | PublishData() | `o3ds.ctrl` | reliable |
| Audio announce | PublishData() | `o3ds.audio.announce` | reliable |

**Implementation:**

```cpp
bool FLiveKitConnector::Send(const uint8* Data, int32 NumBytes)
{
    if (!bDataChannelOpen || !LiveKitRoom)
        return false;
    
    // Determine topic based on message type (can inspect header or use default)
    std::string Topic = "o3ds.anim";  // default to animation (lossy)
    livekit::DataPublishKind Kind = livekit::DataPublishKind::kLossy;
    
    // TODO: Implement topic detection from message header
    // if (IsControlMessage(Data, NumBytes)) {
    //     Topic = "o3ds.ctrl";
    //     Kind = livekit::DataPublishKind::kReliable;
    // }
    
    // Publish data message
    LiveKitRoom->PublishData(Data, NumBytes, Kind, {Topic});
    
    return true;
}
```

**Backpressure Handling:**

LiveKit doesn't expose `bufferedAmount` API. Implement queue-based backpressure:

```cpp
// For lossy data (animation)
if (LossyQueue.size() > 2) {
    LossyQueue.pop_front();  // Drop oldest frame
}
LossyQueue.push_back({Data, NumBytes});

// For reliable data (control)
if (ReliableQueue.size() > 10) {
    UE_LOG(LogOpen3DShared, Warning, TEXT("Reliable queue full, dropping message"));
    return false;
}
ReliableQueue.push_back({Data, NumBytes});
```

### 2.4 Audio Track Implementation

**Publishing Audio:**

```cpp
bool FLiveKitConnector::SendAudioPcm16(const int16* Samples, int32 NumSamples, int32 SampleRate, int32 NumChannels)
{
    if (!LocalAudioTrack || !bConnected)
        return false;
    
    // Queue PCM for encoding on worker thread (similar to LibDataChannel)
    {
        std::lock_guard<std::mutex> Lock(AudioMutex);
        AudioQueue.push_back({Samples, NumSamples, SampleRate, NumChannels});
    }
    
    // Worker thread drains queue, encodes to Opus, publishes via LocalAudioTrack
    return true;
}
```

**Audio Track Labeling:**

Use LiveKit's track metadata to label subjects:

```cpp
// Create audio track with subject label
livekit::LocalAudioTrackOptions Options;
Options.name = "o3ds:subject/MyCharacter";  // or "o3ds:mix" for game audio
Options.stream = "o3ds-audio-stream";

LocalAudioTrack = livekit::LocalAudioTrack::CreateTrack(Options);
LiveKitRoom->PublishTrack(LocalAudioTrack);
```

**Receiving Audio:**

```cpp
void FLiveKitConnector::HandleRemoteAudioReceived(std::shared_ptr<livekit::RemoteAudioTrack> Track)
{
    // Extract subject from track name
    FString TrackName = FString(Track->name().c_str());
    FString Subject = ParseSubjectFromTrackName(TrackName);
    
    // Register callback for audio frames
    Track->OnAudioFrame([this](const livekit::AudioFrame& Frame) {
        // Convert to RTP format expected by RtpDelegate
        TArray<uint8> RtpBytes = EncodeAsRTP(Frame);
        RtpDelegate.Broadcast(RtpBytes);
    });
}
```

### 2.5 State Management

Track connection state through LiveKit callbacks:

```cpp
void FLiveKitConnector::HandleConnectionStateChange(livekit::ConnectionState State)
{
    switch (State)
    {
    case livekit::ConnectionState::kConnected:
        bConnected = true;
        bDataChannelOpen = true;
        StateDelegate.Broadcast(TEXT("connected"), false);
        break;
    
    case livekit::ConnectionState::kReconnecting:
        StateDelegate.Broadcast(TEXT("reconnecting"), false);
        break;
    
    case livekit::ConnectionState::kDisconnected:
        bConnected = false;
        bDataChannelOpen = false;
        StateDelegate.Broadcast(TEXT("disconnected"), false);
        break;
    
    case livekit::ConnectionState::kFailed:
        bConnected = false;
        bDataChannelOpen = false;
        StateDelegate.Broadcast(TEXT("failed"), true);
        break;
    }
}
```

### 2.6 Tick() Method

Non-blocking event pump (similar to LibDataChannel):

```cpp
void FLiveKitConnector::Tick(float DeltaSeconds)
{
    if (!bStarted)
        return;
    
    // Process queued events from LiveKit callbacks
    // LiveKit SDK may require periodic polling or may be fully event-driven
    // (Check SDK documentation for specific requirements)
    
    // Process audio encoding queue
    ProcessAudioQueue();
    
    // Process data send queue
    ProcessDataQueue();
}
```

---

## Phase 3: Configuration and Factory Integration

### 3.1 Configuration Flow

**Broadcaster Configuration:**

User configures in `UO3DSBroadcastComponent`:
- Transport = WebRTC
- Backend = LiveKit
- LiveKitServerUrl = "wss://livekit.example.com"
- LiveKitRoom = "room1"
- LiveKitToken = "eyJhbGc..."

Config flows through:
1. `UO3DSBroadcastComponent` → `FO3DSBroadcastTransportAdapter`
2. Transport adapter creates `FO3DSWebRtcConfig`:
   ```cpp
   FO3DSWebRtcConfig Cfg;
   Cfg.Backend = EO3DSWebRtcBackend::LiveKit;
   Cfg.SignalingUrl = LiveKitServerUrl;  // wss://
   Cfg.Room = LiveKitRoom;
   Cfg.Token = LiveKitToken;
   Cfg.bEnableAudio = bEnableAudio;
   // ... other settings
   ```
3. Factory creates connector: `FWebRTCConnectorFactory::Create(Cfg.Backend)`
4. Connector starts with: `Connector->Start(Cfg)`

**Receiver Configuration:**

User configures in LiveLink Source creation dialog:
- Protocol = WebRTC Server (or Client)
- Backend = LiveKit
- WebRtcRoom = "room1"
- LiveKitToken = "eyJhbGc..."
- Url = "wss://livekit.example.com"

Config flows through `FOpen3DStreamSettings` → `FOpen3DStreamSource` → Factory → Connector

### 3.2 Token Generation

LiveKit requires JWT tokens for authentication. Document workflow:

**Development:** Use `LiveKit/gen-token.mjs`:

```bash
cd LiveKit
npm install livekit-server-sdk
LK_API_KEY=your_key LK_API_SECRET=your_secret node gen-token.mjs
```

**Production:** Implement token service:
- Backend service with LiveKit API credentials
- Issues short-lived tokens per user/session
- UE plugin makes HTTP request to get token before connecting

**Security Note:** Never embed API secrets in UE builds. Always use token service in production.

### 3.3 Factory Update

**File:** `plugins/unreal/Open3DStream/Source/Open3DShared/Private/WebRTCConnectorFactory.cpp`

```cpp
TSharedPtr<IWebRTCConnector> FWebRTCConnectorFactory::Create(EO3DSWebRtcBackend Backend)
{
    switch (Backend)
    {
    case EO3DSWebRtcBackend::LibDataChannel:
        return MakeShared<FLibDataChannelConnector>();
    
    case EO3DSWebRtcBackend::LiveKit:
#if O3DS_ENABLE_LIVEKIT
        UE_LOG(LogOpen3DShared, Log, TEXT("Creating LiveKit connector"));
        return MakeShared<FLiveKitConnector>();
#else
        UE_LOG(LogOpen3DShared, Warning, 
            TEXT("LiveKit backend requested but not available. ")
            TEXT("Build with O3DS_ENABLE_LIVEKIT=1 to enable."));
        return nullptr;
#endif
    
    default:
        UE_LOG(LogOpen3DShared, Error, TEXT("Unknown WebRTC backend: %d"), (int32)Backend);
        return nullptr;
    }
}
```

---

## Phase 4: Testing Strategy

### 4.1 Ground Truth Test: WebRTCConnectorComponent

**Why This Component First:**

Per WEBRTC_TESTING_GUIDE.md: "We use the WebRTCConnectorComponent as our ground truth connectivity test, as it enables us to test the networking and behavior parity independent of the complexity of the production Broadcaster and Receiver classes."

**Test Setup:**

1. Deploy LiveKit server (use `LiveKit/docker-compose.yml`)
2. Generate test token with room="test-room"
3. Create test level in ProjectSandbox
4. Add two actors with `UO3DSWebRTCConnectorComponent`:
   - **Publisher:** Backend=LiveKit, ServerUrl, Room, Token
   - **Subscriber:** Backend=LiveKit, ServerUrl, Room, Token (same room)
5. Enable audio and debug tone on publisher
6. Play in editor

**Expected Results:**

- Both components connect to LiveKit room
- Publisher sends debug tone → subscriber receives RTP packets
- Bidirectional data flow works (send test messages both directions)
- Logs show connection, track publish/subscribe events
- No crashes or errors

**Success Criteria:**

- Connection established within 5 seconds
- Audio RTP packets received (verify packet size ~80-100 bytes for tone)
- Data messages received bidirectionally
- Clean shutdown with no errors
- Behavior matches LibDataChannel connector

### 4.2 Unit Tests

**File:** `plugins/unreal/Open3DStream/Source/Open3DBroadcast/Private/Tests/O3DSLiveKitTests.cpp`

Create FAutomationTestBase-derived tests:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveKitConnectorCreateTest, 
    "Open3DStream.LiveKit.CreateConnector", 
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLiveKitConnectorCreateTest::RunTest(const FString& Parameters)
{
#if O3DS_ENABLE_LIVEKIT
    TSharedPtr<IWebRTCConnector> Connector = 
        FWebRTCConnectorFactory::Create(EO3DSWebRtcBackend::LiveKit);
    
    TestTrue(TEXT("Connector created"), Connector.IsValid());
    TestFalse(TEXT("Not connected initially"), Connector->IsOpen());
    
    return true;
#else
    AddWarning(TEXT("Test skipped: LiveKit not enabled"));
    return true;
#endif
}
```

Additional tests:
- Config validation (missing token, invalid URL)
- State transitions (connecting → connected → disconnected)
- Data send/receive round-trip
- Audio track publishing
- Error handling (invalid token, network failure)

### 4.3 Integration Tests

**Broadcaster + Receiver End-to-End:**

Following WEBRTC_TESTING_GUIDE.md pattern, adapted for LiveKit:

**Setup:**
1. Start LiveKit server (Docker or cloud)
2. Generate tokens for sender and receiver
3. Launch two UE editor instances

**Receiver Editor (Subscriber):**
1. Open ProjectSandbox
2. Window → Live Link → + Source → Open3D Stream
3. Configure:
   - Protocol: WebRTC Server
   - Backend: LiveKit
   - Url: wss://livekit.example.com
   - Room: test-room
   - Token: <receiver-token>
   - Enable WebRTC Audio: ✓
4. Add `O3DSRemoteAudioComponent` to actor for audio playback

**Broadcaster Editor (Publisher):**
1. Open ProjectSandbox (separate instance)
2. Add `O3DSBroadcastComponent` to character
3. Configure:
   - Transport: WebRTC
   - Backend: LiveKit
   - Signaling URL: wss://livekit.example.com
   - Room: test-room
   - Token: <sender-token>
   - Subject: Character's skeleton
   - Capture Mode: Mix or Input
   - Enable Audio: ✓
4. Play in editor

**Validation:**
- Receiver sees Live Link subject appear (green indicator)
- Animation data flows smoothly
- Audio plays in receiver
- Latency similar to LibDataChannel (~50-150ms)
- No frame drops or audio glitches
- Clean disconnect on stop

### 4.4 Performance Benchmarks

Compare LibDataChannel vs LiveKit:

| Metric | Target | LibDataChannel | LiveKit |
|--------|--------|----------------|---------|
| Connection Time | <5s | ~2s | TBD |
| End-to-End Latency | <150ms | 50-100ms | TBD |
| Data Throughput | 10-50 KB/s | 30 KB/s | TBD |
| Audio Packet Rate | 50 pkt/s | 50 pkt/s | TBD |
| CPU (Sender) | <5% | 3% | TBD |
| CPU (Receiver) | <3% | 2% | TBD |

Collect metrics using:
- UE Profiler for CPU usage
- Custom CVar logging for latency (o3ds.LiveKit.DebugLatency 1)
- Network stats from LiveKit SDK

---

## Phase 5: Documentation Updates

### 5.1 Testing Guide Updates

**File:** `plugins/unreal/Open3DStream/docs/WEBRTC_TESTING_GUIDE.md`

Add new section: "Test Setup 3: LiveKit SFU (Cloud/Multi-Party)"

Content:
- Prerequisites (LiveKit server, token generator)
- Server deployment options (Docker, cloud)
- Token generation workflow
- Configuration differences from P2P
- Expected behavior (same as P2P from user perspective)
- Troubleshooting (token errors, network issues, SFU-specific problems)
- Performance characteristics (slightly higher latency vs P2P, but scales better)

### 5.2 Backend Comparison Guide

Create new doc: `docs/WEBRTC_BACKEND_COMPARISON.md`

| Feature | LibDataChannel (P2P) | LiveKit (SFU) |
|---------|----------------------|---------------|
| **Architecture** | Peer-to-peer direct | Media server relay |
| **Signaling** | Custom WebSocket | LiveKit WebSocket + JWT |
| **Best For** | 1-to-1, low latency | 1-to-many, scalability |
| **NAT Traversal** | STUN/TURN required | Handled by SFU |
| **Bandwidth (Sender)** | Scales with peers | Fixed (send once) |
| **Latency** | 50-100ms | 100-200ms |
| **Max Participants** | 2-4 practical | 100+ |
| **Auth** | None (signaling auth) | JWT tokens |
| **Deployment** | Simple signaling server | LiveKit server required |

Guidance:
- Use LibDataChannel for development, low-latency P2P
- Use LiveKit for production multi-party, scalability, cloud deployment

### 5.3 Troubleshooting Guide

Add LiveKit-specific troubleshooting to WEBRTC_TESTING_GUIDE.md:

**Token Errors:**
- Symptom: "Invalid token" or connection rejected
- Solution: Verify token is valid, not expired, has correct grants
- Tool: Decode JWT at jwt.io to inspect claims

**Network Issues:**
- Symptom: Connection timeout
- Solution: Check LiveKit server is reachable, ports open (443 for signaling, UDP 50000-50050 for media)
- Tool: curl wss://livekit.example.com (should return upgrade response)

**SFU Media Relay:**
- Symptom: Audio/data not received by some participants
- Solution: Check LiveKit server logs, verify all participants joined same room
- Tool: LiveKit web dashboard or CLI

**Bandwidth:**
- Symptom: High sender bandwidth with many receivers
- Solution: This is expected (SFU advantage) - sender sends once, server relays
- Compare to P2P where sender bandwidth scales with peer count

---

## Phase 6: Deployment and Operations

### 6.1 LiveKit Server Deployment

**Development (Docker):**

Use provided `LiveKit/docker-compose.yml`:

```bash
cd LiveKit
docker-compose up -d
docker-compose logs -f livekit
```

Server accessible at: https://livekit.maamawi.dance (or localhost)

**Production (AWS):**

Follow `LiveKit/README.md`:
1. Launch EC2 instance (Ubuntu 22.04, t3.medium+)
2. Configure security groups (TCP 443, UDP 50000-50050)
3. Set up DNS A record
4. Deploy Docker stack with Caddy reverse proxy
5. Generate API keys and update livekit.yaml
6. Start services and verify HTTPS certificate

**Cloud Services:**

LiveKit offers managed cloud service:
- Sign up at https://cloud.livekit.io
- Get API key/secret
- Use wss://your-project.livekit.cloud
- No server management required

### 6.2 Token Service

**Development:**

Use `LiveKit/gen-token.mjs` locally:

```bash
LK_API_KEY=your_key LK_API_SECRET=your_secret node gen-token.mjs
```

**Production:**

Deploy token service (Node.js example):

```javascript
const express = require('express');
const { AccessToken } = require('livekit-server-sdk');

const app = express();

app.post('/token', (req, res) => {
    const { identity, room } = req.body;
    
    const token = new AccessToken(
        process.env.LK_API_KEY,
        process.env.LK_API_SECRET,
        { identity, ttl: 3600 }
    );
    
    token.addGrant({ 
        room,
        roomJoin: true,
        canPublish: true,
        canSubscribe: true
    });
    
    res.json({ token: token.toJwt() });
});

app.listen(3000);
```

UE plugin integration:
1. Add HTTP request node to Blueprint or C++
2. Call token service with user identity
3. Receive JWT token
4. Pass to connector config
5. Connect to LiveKit room

### 6.3 Monitoring and Observability

**LiveKit Metrics:**

- Room count, participant count
- Published/subscribed tracks
- Bandwidth usage
- Connection quality (packet loss, jitter)

**UE Plugin Metrics:**

Add CVars for debugging:

```cpp
o3ds.LiveKit.Log 0/1                  // General logging
o3ds.LiveKit.DebugLatency 0/1        // Log per-frame latency
o3ds.LiveKit.Stats 0/1               // Print connection stats
```

Log important events:
- Connection state changes
- Track publish/subscribe
- Data message send/receive counts
- Audio encoding/decoding stats
- Error conditions

**Dashboards:**

LiveKit provides web dashboard:
- View active rooms and participants
- Monitor bandwidth and quality
- Debug connection issues
- Replay sessions

---

## Implementation Checklist

### Prerequisites
- [ ] Review Phase 1-6 in detail
- [ ] Evaluate SDK options (official vs community)
- [ ] Validate SDK builds on target platforms (Win64, Linux, Mac)
- [ ] Document SDK selection rationale

### Phase 1: SDK Integration
- [ ] Add SDK as thirdparty dependency (submodule or vendor)
- [ ] Update Open3DShared.Build.cs with LiveKit linking
- [ ] Add O3DS_ENABLE_LIVEKIT build flag
- [ ] Test build with flag=0 (no LiveKit, existing code works)
- [ ] Test build with flag=1 (LiveKit enabled, plugin compiles)

### Phase 2: Connector Implementation
- [ ] Create LiveKitConnector.h with class skeleton
- [ ] Implement Start() - room connection and JWT auth
- [ ] Implement Stop() - clean shutdown
- [ ] Implement IsOpen() - connection state query
- [ ] Implement Send() - data message publishing with topic routing
- [ ] Implement EnableAudioSend() - audio track setup
- [ ] Implement SendAudioPcm16() - audio frame queueing and encoding
- [ ] Implement Tick() - non-blocking event processing
- [ ] Add state callbacks (OnState delegate)
- [ ] Add data callbacks (OnData delegate)
- [ ] Add audio callbacks (OnRemoteAudioRtp delegate)
- [ ] Implement connection state tracking
- [ ] Implement remote participant handling
- [ ] Add error handling and recovery logic

### Phase 3: Integration
- [ ] Update WebRTCConnectorFactory::Create()
- [ ] Verify config propagation from Broadcast Component
- [ ] Verify config propagation from LiveLink Source
- [ ] Test backend selection UI (ensure LiveKit appears in dropdown)
- [ ] Test config validation (missing token, invalid URL)

### Phase 4: Testing
- [ ] Deploy LiveKit test server (Docker)
- [ ] Set up token generation workflow
- [ ] Create WebRTCConnectorComponent test level
- [ ] Test ground truth connectivity (publisher + subscriber in same level)
- [ ] Verify bidirectional data flow
- [ ] Verify audio publishing and reception
- [ ] Write unit tests for connector lifecycle
- [ ] Test two-editor setup (full broadcaster + receiver)
- [ ] Verify animation data transmission
- [ ] Verify audio transmission and playback
- [ ] Test multi-participant scenarios (1-to-many)
- [ ] Test reconnection and error recovery
- [ ] Benchmark performance vs LibDataChannel

### Phase 5: Documentation
- [ ] Update WEBRTC_TESTING_GUIDE.md with LiveKit section
- [ ] Create WEBRTC_BACKEND_COMPARISON.md
- [ ] Document token generation workflow
- [ ] Add LiveKit troubleshooting section
- [ ] Document deployment options (Docker, cloud, AWS)
- [ ] Create example configurations
- [ ] Add performance benchmarks and comparison

### Phase 6: Validation
- [ ] Code review (follow Open3DStream Agent Playbook)
- [ ] Security review (token handling, no API secrets in code)
- [ ] Performance validation (meet targets in benchmark table)
- [ ] Cross-platform build test (Windows, Linux, Mac)
- [ ] Regression test (LibDataChannel still works)
- [ ] Documentation review (completeness, accuracy)
- [ ] User acceptance testing with team

---

## Risk Assessment

### Technical Risks

**1. SDK Maturity**
- **Risk:** Community SDK may have bugs or missing features
- **Mitigation:** 
  - Evaluate SDK thoroughly in Phase 1
  - Create minimal test harness before full integration
  - Keep abstraction layer thin for easier SDK replacement
  - Monitor official SDK progress for future migration

**2. Build Complexity**
- **Risk:** LiveKit SDK has many dependencies (libwebrtc, protobuf, etc.)
- **Mitigation:**
  - Consider vendoring pre-built binaries
  - Document exact build flags and versions
  - Test on all target platforms early
  - Provide build scripts/documentation

**3. API Surface Differences**
- **Risk:** LiveKit data messages vs DataChannels have different semantics
- **Mitigation:**
  - Implement queue-based backpressure simulation
  - Add message header with topic/seq/timestamp
  - Test thoroughly under load and packet loss
  - Document any behavioral differences

**4. Performance**
- **Risk:** SFU may add latency vs direct P2P
- **Mitigation:**
  - Benchmark early and often
  - Document expected latency increase (50-100ms)
  - Optimize media relay paths
  - Consider TURN for P2P if latency critical

### Operational Risks

**5. Token Management**
- **Risk:** JWT tokens require secure generation and distribution
- **Mitigation:**
  - Never embed API secrets in builds
  - Provide token service reference implementation
  - Document token lifecycle and security best practices
  - Add token expiration handling in connector

**6. Server Deployment**
- **Risk:** Teams may struggle with LiveKit server setup
- **Mitigation:**
  - Provide Docker compose for development
  - Document AWS deployment step-by-step
  - Recommend LiveKit Cloud for production
  - Include troubleshooting guide for common issues

### Project Risks

**7. Scope Creep**
- **Risk:** Feature requests beyond core connectivity (HLS, recording, etc.)
- **Mitigation:**
  - Clearly define non-goals (Phase 2 features)
  - Stick to IWebRTCConnector interface contract
  - Defer advanced features to follow-on issues
  - Keep PRs small and focused

**8. Backward Compatibility**
- **Risk:** Changes break existing LibDataChannel users
- **Mitigation:**
  - Zero changes to LibDataChannel code paths
  - Extensive regression testing
  - Build flag makes LiveKit optional
  - Factory pattern isolates backends completely

---

## Success Metrics

### Functional
- [ ] LiveKit connector implements full IWebRTCConnector interface
- [ ] Broadcaster publishes animation + audio via LiveKit
- [ ] Receiver subscribes to animation + audio via LiveKit
- [ ] Multi-participant support works (1-to-many, many-to-many)
- [ ] Behavior parity with LibDataChannel (from user perspective)
- [ ] Clean connection/disconnection with no errors
- [ ] Reconnection works automatically on network interruption

### Performance
- [ ] Connection time < 5 seconds
- [ ] End-to-end latency < 200ms (acceptable for SFU architecture)
- [ ] Data throughput 10-50 KB/s (same as LibDataChannel)
- [ ] Audio packet rate 50 pkt/s (same as LibDataChannel)
- [ ] CPU usage < 5% sender, < 3% receiver
- [ ] Memory usage comparable to LibDataChannel
- [ ] Scales to 10+ simultaneous receivers without degradation

### Quality
- [ ] Code follows Open3DStream Agent Playbook rules
- [ ] All public APIs documented with comments
- [ ] Unit tests cover core functionality
- [ ] Integration tests cover end-to-end scenarios
- [ ] Documentation enables team to deploy and use LiveKit backend
- [ ] No security vulnerabilities (tokens, credentials, etc.)
- [ ] Builds successfully on Windows, Linux, Mac
- [ ] Regression test suite passes (existing functionality unaffected)

### User Experience
- [ ] Backend selection is clear and intuitive
- [ ] Configuration is straightforward (URL, room, token)
- [ ] Error messages are actionable
- [ ] Troubleshooting guide covers common issues
- [ ] Performance is acceptable for production use
- [ ] Deployment options are well-documented

---

## Timeline Estimate

**Assumptions:**
- 1 engineer, full-time
- SDK evaluation already complete
- LiveKit server already deployed
- Unreal Engine 5.6 environment ready

**Phase 1: SDK Integration** - 3-5 days
- SDK evaluation and selection: 1 day
- Build system integration: 1-2 days
- Conditional compilation setup: 1 day
- Build validation: 1 day

**Phase 2: Core Implementation** - 10-15 days
- Connector class skeleton: 2 days
- Connection and auth: 2-3 days
- Data channel implementation: 2-3 days
- Audio track implementation: 3-4 days
- State management and callbacks: 2-3 days

**Phase 3: Integration** - 2-3 days
- Factory updates: 0.5 day
- Config wiring: 1 day
- UI validation: 0.5-1 day
- End-to-end smoke test: 1 day

**Phase 4: Testing** - 5-7 days
- Unit tests: 2 days
- WebRTCConnectorComponent tests: 1-2 days
- Integration tests (two editors): 1-2 days
- Multi-participant tests: 1 day
- Performance benchmarking: 1-2 days

**Phase 5: Documentation** - 3-4 days
- Testing guide updates: 1 day
- Backend comparison guide: 1 day
- Troubleshooting section: 1 day
- Deployment guide: 1 day

**Phase 6: Polish and Review** - 3-5 days
- Code review and fixes: 1-2 days
- Security review: 1 day
- Cross-platform testing: 1-2 days
- Final validation: 1 day

**Total: 26-39 days (5-8 weeks)**

**Parallel Workstreams:**
- Server deployment can happen during Phase 1-2
- Documentation can be written throughout implementation
- Testing can start as soon as each phase completes

**Risk Buffer:** Add 25-50% for unknowns and SDK integration challenges

**Realistic Estimate: 7-12 weeks**

---

## Next Steps

1. **Review and Approval**
   - Review this plan with team
   - Get approval on SDK selection
   - Confirm timeline and resource allocation

2. **SDK Evaluation (Phase 1 Start)**
   - Clone and build candidate SDKs locally
   - Test basic room connection with LiveKit server
   - Validate cross-platform builds
   - Document findings and recommendation

3. **Kickoff**
   - Create tracking issue linking to this plan
   - Set up development branch
   - Deploy LiveKit test server
   - Begin Phase 1 implementation

---

## References

- Issue #90: LiveKit Setup and Unreal Open3DStream Integration
- Issue #94: WebRTC transport refactor (backend-agnostic interface)
- WEBRTC_TESTING_GUIDE.md: Current testing procedures
- WEBRTC_BACKENDS.md: Backend comparison and selection
- LIVEKIT_DATA_MESSAGING.md: Data channel mapping details
- LiveKit/README.md: Server deployment guide
- Open3DStream Agent Playbook: Implementation rules and constraints

---

**Document Revision History:**
- 2025-11-03: Initial draft (Planning Agent)

