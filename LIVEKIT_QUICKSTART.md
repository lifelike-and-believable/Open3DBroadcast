# LiveKit Implementation Quick Start

**For:** Engineers implementing the LiveKit backend  
**Status:** Planning phase - SDK evaluation pending  
**Prerequisites:** Familiarity with Unreal Engine, C++, WebRTC concepts

---

## What You're Building

A new `FLiveKitConnector` class that implements the `IWebRTCConnector` interface, enabling Open3DStream to use LiveKit's SFU for scalable multi-party streaming instead of (or alongside) the existing libdatachannel P2P backend.

**Key Constraint:** Zero changes to Broadcaster or Receiver components - they use the connector via the interface.

---

## Before You Start

### Read These First (30 minutes)

1. **Interface Contract:** `plugins/unreal/Open3DStream/Source/Open3DShared/Public/IWebRTCConnector.h`
   - This is what you must implement
   - Look at method signatures and comments

2. **Reference Implementation:** `plugins/unreal/Open3DStream/Source/Open3DShared/Private/LibDataChannelConnector.h`
   - Existing P2P implementation
   - Good patterns for state management, audio queuing, threading

3. **Testing Pattern:** `plugins/unreal/Open3DStream/docs/WEBRTC_TESTING_GUIDE.md`
   - How to test ground truth (WebRTCConnectorComponent)
   - How to test full integration (two editors)

4. **Agent Playbook:** `.github/copilot-instructions.md`
   - Rules you must follow
   - No exceptions to module isolation, backward compatibility, etc.

### Detailed Plan (reference, not required reading now)

- **Implementation Plan:** `plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md` (36KB)
- **Summary:** `LIVEKIT_IMPLEMENTATION_SUMMARY.md` (this directory)

---

## Phase 1: SDK Evaluation (Week 1)

### Goal
Choose and validate the LiveKit C++ SDK that will work with Unreal Engine.

### Candidate SDKs

**Option A: zesun96/livekit-client-cpp** (recommended)
```bash
git clone https://github.com/zesun96/livekit-client-cpp.git
cd livekit-client-cpp
# Follow build instructions
```

**Option B: livekit/client-sdk-cpp** (official, but WIP)
```bash
git clone https://github.com/livekit/client-sdk-cpp.git
cd client-sdk-cpp
# Follow build instructions
```

### Evaluation Checklist

Build the SDK on your target platforms:
- [ ] Windows 64-bit (primary)
- [ ] Linux (if targeting server)
- [ ] macOS (optional)

Test basic functionality:
- [ ] Can you connect to a LiveKit room with a JWT token?
- [ ] Can you publish a data message?
- [ ] Can you subscribe to data messages?
- [ ] Can you publish an audio track?
- [ ] Can you subscribe to an audio track?

Document findings:
- [ ] What dependencies are required? (libwebrtc, protobuf, etc.)
- [ ] Are there pre-built binaries or must you build everything?
- [ ] What build flags are needed for Unreal compatibility?
- [ ] Are there any API gaps vs IWebRTCConnector interface?

**Decision Point:** By end of Phase 1, choose one SDK and document rationale.

### Setting Up LiveKit Test Server

Use the provided Docker setup:

```bash
cd Open3DStream/LiveKit
docker-compose up -d
docker-compose logs -f livekit
```

Server runs at: http://localhost:7880 (or https://livekit.maamawi.dance if deployed)

Generate test token:
```bash
cd Open3DStream/LiveKit
npm install livekit-server-sdk
LK_API_KEY=devkey LK_API_SECRET=secret node gen-token.mjs
```

Test connection with SDK:
```cpp
// Minimal test program
livekit::RoomOptions options;
options.url = "ws://localhost:7880";
options.token = "<token-from-generator>";

auto room = livekit::Room::Connect(options);
// If this works, SDK is viable
```

---

## Phase 2: Core Implementation (Weeks 2-4)

### Step 1: Create Skeleton Class

**File:** `plugins/unreal/Open3DStream/Source/Open3DShared/Private/LiveKitConnector.h`

```cpp
#pragma once
#include "CoreMinimal.h"
#include "IWebRTCConnector.h"

#if O3DS_ENABLE_LIVEKIT
#include <livekit/room.h>
// ... other SDK includes

class FLiveKitConnector : public IWebRTCConnector
{
public:
    FLiveKitConnector() = default;
    virtual ~FLiveKitConnector() override;

    // IWebRTCConnector interface - stub everything initially
    virtual bool Start(const FO3DSWebRtcConfig& Config) override { return false; }
    virtual void Stop() override {}
    virtual void Tick(float DeltaSeconds) override {}
    virtual bool IsOpen() const override { return false; }
    virtual bool Send(const uint8* Data, int32 NumBytes) override { return false; }
    virtual bool EnableAudioSend(bool bEnable) override { return false; }
    virtual bool SendAudioPcm16(const int16* Samples, int32 NumSamples, int32 SampleRate, int32 NumChannels) override { return false; }
    
    virtual FO3DSOnWebRtcState& OnState() override { return StateDelegate; }
    virtual FO3DSOnWebRtcData& OnData() override { return DataDelegate; }
    virtual FO3DSOnWebRtcRtp& OnRemoteAudioRtp() override { return RtpDelegate; }

private:
    FO3DSOnWebRtcState StateDelegate;
    FO3DSOnWebRtcData DataDelegate;
    FO3DSOnWebRtcRtp RtpDelegate;
};

#endif // O3DS_ENABLE_LIVEKIT
```

**File:** `plugins/unreal/Open3DStream/Source/Open3DShared/Private/LiveKitConnector.cpp`

```cpp
#include "LiveKitConnector.h"

#if O3DS_ENABLE_LIVEKIT

FLiveKitConnector::~FLiveKitConnector()
{
    Stop();
}

// Implement methods one by one...

#endif // O3DS_ENABLE_LIVEKIT
```

### Step 2: Update Build System

**File:** `plugins/unreal/Open3DStream/Source/Open3DShared/Open3DShared.Build.cs`

Add after existing WebRTC includes:

```csharp
// LiveKit SDK integration (optional)
bool bEnableLiveKit = System.Environment.GetEnvironmentVariable("O3DS_ENABLE_LIVEKIT") == "1";
PublicDefinitions.Add($"O3DS_ENABLE_LIVEKIT={(bEnableLiveKit ? "1" : "0")}");

if (bEnableLiveKit)
{
    string LiveKitDir = System.IO.Path.Combine(ThirdPartyDir, "livekit");
    PublicIncludePaths.Add(System.IO.Path.Combine(LiveKitDir, "include"));
    
    if (Target.Platform == UnrealTargetPlatform.Win64)
    {
        PublicAdditionalLibraries.Add(System.IO.Path.Combine(LiveKitDir, "livekit-client.lib"));
        // Add other dependencies as discovered during SDK build
    }
    // Linux/Mac similar
}
```

### Step 3: Update Factory

**File:** `plugins/unreal/Open3DStream/Source/Open3DShared/Private/WebRTCConnectorFactory.cpp`

```cpp
#include "WebRTCConnectorFactory.h"
#include "LibDataChannelConnector.h"

#if O3DS_ENABLE_LIVEKIT
#include "LiveKitConnector.h"
#endif

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
        UE_LOG(LogOpen3DShared, Warning, TEXT("LiveKit backend not available"));
        return nullptr;
#endif
    
    default:
        return nullptr;
    }
}
```

**Test Build:**
```powershell
# Without LiveKit (should still build)
cd Open3DStream/plugins/unreal/Open3DStream
.\Build\Scripts\Run-Build.ps1

# With LiveKit (after SDK integrated)
$env:O3DS_ENABLE_LIVEKIT="1"
.\Build\Scripts\Run-Build.ps1
```

### Step 4: Implement Methods Incrementally

**Recommended Order:**

1. **Start()** - room connection and auth
   - Parse config (URL, room, token)
   - Create LiveKit Room instance
   - Connect with JWT token
   - Register callbacks (OnConnected, OnDisconnected, etc.)
   - Return success/failure

2. **Stop()** - clean shutdown
   - Disconnect from room
   - Clean up SDK objects
   - Reset state flags

3. **IsOpen()** - state query
   - Return bConnected flag

4. **OnState() callbacks** - state notifications
   - Broadcast connection state changes
   - Handle errors appropriately

5. **Send()** - data messages
   - Publish to `o3ds.anim` topic (lossy) by default
   - Add header with topic/seq/timestamp
   - Handle backpressure with queue

6. **OnData() callbacks** - receive data
   - Subscribe to all topics
   - Forward received data to delegate
   - Parse and route by topic

7. **SendAudioPcm16()** - audio publishing
   - Queue PCM samples
   - Encode to Opus on worker thread
   - Publish via audio track

8. **OnRemoteAudioRtp() callbacks** - receive audio
   - Subscribe to remote audio tracks
   - Convert frames to RTP format
   - Forward to delegate

9. **Tick()** - event pump
   - Process queued events
   - Drain audio encoding queue
   - Non-blocking operation

### Step 5: Test Each Method

After implementing each method, test with WebRTCConnectorComponent:

```cpp
// In your test level
UO3DSWebRTCConnectorComponent* Comp = NewObject<UO3DSWebRTCConnectorComponent>();
Comp->SignalingUrl = TEXT("wss://localhost:7880");
Comp->Room = TEXT("test-room");
Comp->bServer = false; // Client mode for LiveKit
// Set token from generator
Comp->BeginPlay(); // Starts connector

// Check logs for connection, state changes, etc.
```

**Incremental Testing:**
- Start/Stop → connection logs appear
- Send/Receive → data messages flow
- Audio → RTP packets received

---

## Phase 3: Integration (Week 5)

### Test with Broadcast Component

1. Open ProjectSandbox in UE Editor
2. Add `UO3DSBroadcastComponent` to character
3. Set Transport = WebRTC, Backend = LiveKit
4. Fill in URL, Room, Token
5. Set Subject to character's skeleton
6. Play in editor
7. Verify animation data published

### Test with LiveLink Source

1. Open second UE Editor instance
2. Window → Live Link → + Source
3. Select Open3D Stream
4. Set Protocol = WebRTC, Backend = LiveKit
5. Fill in URL, Room, Token
6. Verify Live Link subject appears
7. Verify animation updates in real-time

### Test End-to-End

With both editors running:
- Animation data flows from broadcaster to receiver
- Audio flows (if enabled)
- No errors in logs
- Performance is acceptable
- Latency < 200ms

---

## Phase 4: Testing & Validation (Week 6)

### Unit Tests

**File:** `plugins/unreal/Open3DStream/Source/Open3DBroadcast/Private/Tests/O3DSLiveKitTests.cpp`

```cpp
#include "Misc/AutomationTest.h"
#include "WebRTCConnectorFactory.h"
#include "IWebRTCConnector.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveKitFactoryTest,
    "Open3DStream.LiveKit.Factory",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLiveKitFactoryTest::RunTest(const FString& Parameters)
{
#if O3DS_ENABLE_LIVEKIT
    TSharedPtr<IWebRTCConnector> Connector = 
        FWebRTCConnectorFactory::Create(EO3DSWebRtcBackend::LiveKit);
    
    TestTrue(TEXT("Connector created"), Connector.IsValid());
    return true;
#else
    AddWarning(TEXT("LiveKit not enabled"));
    return true;
#endif
}

// Add more tests for:
// - Config validation
// - Connection lifecycle
// - Data send/receive
// - Audio publishing
// - Error handling
```

Run tests:
```powershell
.\Build\Scripts\Run-AutomationTests.ps1
```

### Performance Benchmarking

Compare to LibDataChannel baseline:

```cpp
// Instrument your code with timing
double StartTime = FPlatformTime::Seconds();
Connector->Send(Data, Size);
double EndTime = FPlatformTime::Seconds();
UE_LOG(LogOpen3DShared, Log, TEXT("Send took %.2f ms"), (EndTime - StartTime) * 1000.0);
```

Collect metrics:
- Connection time
- Send latency
- Receive latency
- CPU usage (UE Profiler)
- Memory usage

**Targets:**
- Connection: < 5 seconds
- E2E Latency: < 200ms
- CPU (sender): < 5%
- CPU (receiver): < 3%

---

## Common Pitfalls

### 1. Module Dependencies

❌ **Don't:**
```cpp
// In Open3DShared
#include "Open3DStreamSource.h"  // Wrong! Cross-module dependency
```

✅ **Do:**
```cpp
// Only use interfaces and shared types
#include "IWebRTCConnector.h"
```

### 2. Blocking Operations

❌ **Don't:**
```cpp
virtual void Tick(float DeltaSeconds) override
{
    auto result = Room->SendDataBlocking(Data);  // Blocks game thread!
}
```

✅ **Do:**
```cpp
virtual void Tick(float DeltaSeconds) override
{
    // Queue on game thread
    DataQueue.push(Data);
    
    // Worker thread drains queue asynchronously
}
```

### 3. Missing Conditional Compilation

❌ **Don't:**
```cpp
// In header, always includes LiveKit
#include <livekit/room.h>  // Fails if SDK not installed
```

✅ **Do:**
```cpp
#if O3DS_ENABLE_LIVEKIT
#include <livekit/room.h>
#endif
```

### 4. Hard-Coded Configuration

❌ **Don't:**
```cpp
const char* ApiSecret = "my-secret-key";  // Never!
```

✅ **Do:**
```cpp
// Tokens come from config, generated externally
FString Token = Config.Token;
```

### 5. Missing Error Handling

❌ **Don't:**
```cpp
bool FLiveKitConnector::Start(const FO3DSWebRtcConfig& Config)
{
    Room = ConnectToRoom(Config.SignalingUrl);  // Assumes success
    return true;
}
```

✅ **Do:**
```cpp
bool FLiveKitConnector::Start(const FO3DSWebRtcConfig& Config)
{
    if (Config.Token.IsEmpty())
    {
        UE_LOG(LogOpen3DShared, Error, TEXT("LiveKit token required"));
        StateDelegate.Broadcast(TEXT("Token missing"), true);
        return false;
    }
    
    try
    {
        Room = ConnectToRoom(Config.SignalingUrl, Config.Token);
    }
    catch (const std::exception& e)
    {
        UE_LOG(LogOpen3DShared, Error, TEXT("Connection failed: %s"), *FString(e.what()));
        StateDelegate.Broadcast(TEXT("Connection failed"), true);
        return false;
    }
    
    return true;
}
```

---

## Debug Tips

### Enable Verbose Logging

```cpp
// In your connector
if (Config.bVerbose)
{
    UE_LOG(LogOpen3DShared, Log, TEXT("LiveKit: Publishing data size=%d topic=%s"), Size, *Topic);
}
```

Console commands:
```
o3ds.LiveKit.Log 1           // Enable LiveKit logging
o3ds.LiveKit.DebugLatency 1  // Log per-frame timing
```

### Check LiveKit Server Logs

```bash
docker-compose logs -f livekit
# Look for:
# - Client join events
# - Track publish/subscribe
# - Error messages
```

### Use LiveKit Web Dashboard

If using LiveKit Cloud or with dashboard enabled:
- View active rooms
- See participant list
- Monitor tracks (published/subscribed)
- Check connection quality

### Network Debugging

```bash
# Test WebSocket connection
wscat -c wss://livekit.example.com

# Check port accessibility
nc -zv livekit.example.com 443
nc -zvu livekit.example.com 50000-50050  # Media ports
```

---

## Getting Help

### Documentation
- **Detailed Plan:** `docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md`
- **Interface:** `Source/Open3DShared/Public/IWebRTCConnector.h`
- **Reference Impl:** `Source/Open3DShared/Private/LibDataChannelConnector.cpp`
- **Testing Guide:** `docs/WEBRTC_TESTING_GUIDE.md`

### Code References
- **Factory Pattern:** See how LibDataChannel is instantiated
- **State Management:** Copy patterns from FLibDataChannelConnector
- **Audio Queueing:** Similar patterns for Opus encoding
- **Threading:** Use Unreal's thread-safe primitives

### External Resources
- **LiveKit Docs:** https://docs.livekit.io/
- **SDK GitHub:** https://github.com/zesun96/livekit-client-cpp (or official)
- **WebRTC Basics:** https://webrtc.org/getting-started/overview

### Ask Questions
- Consult the detailed implementation plan for specifics
- Check existing issues (#90, #94) for related discussion
- Review code comments in interface definitions

---

## Success Checklist

Before considering Phase 2 complete:

- [ ] FLiveKitConnector compiles with O3DS_ENABLE_LIVEKIT=1
- [ ] Plugin still builds with O3DS_ENABLE_LIVEKIT=0
- [ ] WebRTCConnectorComponent test passes (connection + data + audio)
- [ ] Factory correctly creates LiveKit connector when backend selected
- [ ] All IWebRTCConnector methods implemented
- [ ] State callbacks work (OnState delegate)
- [ ] Data messages flow bidirectionally (OnData delegate)
- [ ] Audio tracks publish and subscribe (OnRemoteAudioRtp delegate)
- [ ] Error handling covers common failures (token invalid, network down, etc.)
- [ ] Code follows Playbook rules (module isolation, no blocking, etc.)

Before considering implementation complete:

- [ ] Full E2E test passes (two editors, broadcaster + receiver)
- [ ] Animation data streams correctly
- [ ] Audio streams and plays correctly
- [ ] Multi-participant test works (3+ participants)
- [ ] Performance meets targets (< 200ms latency, < 5% CPU)
- [ ] Unit tests pass
- [ ] Documentation updated (testing guide, troubleshooting, etc.)
- [ ] Code review complete
- [ ] Regression tests pass (LibDataChannel still works)

---

## Next Steps

1. **Start SDK Evaluation** (today)
   - Clone candidate SDK
   - Build on your platform
   - Test basic connectivity

2. **Report Findings** (end of week 1)
   - Which SDK works best?
   - What dependencies are needed?
   - Any blockers discovered?

3. **Begin Implementation** (week 2)
   - Create connector skeleton
   - Update build system
   - Test stub compiles

4. **Iterate Weekly** (weeks 3-6)
   - Implement methods incrementally
   - Test after each addition
   - Update docs as you go

5. **Final Validation** (week 7)
   - Full regression testing
   - Performance benchmarks
   - Documentation review

---

**Good luck! This is a well-scoped, achievable project with clear success criteria. Follow the plan, test incrementally, and you'll have a working LiveKit backend in 7-12 weeks.**

