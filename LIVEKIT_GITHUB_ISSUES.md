# LiveKit Backend Implementation - GitHub Issues

**Created:** 2025-11-03  
**Based on:** Planning documents in PR (copilot/add-livekit-backend-support)  
**Total Issues:** 8  
**Estimated Timeline:** 7-12 weeks

---

## Issue Structure

Each issue follows this structure:
- **Title:** Clear, action-oriented
- **Labels:** Phase number, area, priority
- **Assignee:** TBD (coding agents)
- **Milestone:** M3.4.1b - LiveKit Backend
- **Dependencies:** Links to prerequisite issues
- **Acceptance Criteria:** Clear, testable outcomes
- **Related Docs:** Links to planning documents

---

## Issue 1: Phase 1 - Evaluate and Select LiveKit C++ SDK

**Title:** `[LiveKit P1] Evaluate and select LiveKit C++ SDK for Unreal Engine integration`

**Description:**

### Objective
Evaluate candidate LiveKit C++ SDKs to determine which is best suited for integration with Unreal Engine 5.6 and Open3DStream architecture.

### Candidate SDKs

**Option A: zesun96/livekit-client-cpp** (Recommended)
- Repository: https://github.com/zesun96/livekit-client-cpp
- Status: Active community project
- Pros: Mature, cross-platform, working examples
- Cons: Community-maintained, may lag features

**Option B: livekit/client-sdk-cpp** (Official)
- Repository: https://github.com/livekit/client-sdk-cpp
- Status: Work in progress (WIP)
- Pros: Official support, latest features
- Cons: Not production-ready, unstable API

### Tasks

- [ ] Clone both candidate SDKs locally
- [ ] Build SDK on Windows 64-bit (primary target)
- [ ] Build SDK on Linux (optional, if targeting)
- [ ] Build SDK on macOS (optional)
- [ ] Test basic connectivity to LiveKit room with JWT token
- [ ] Test data message publish/subscribe
- [ ] Test audio track publish/subscribe
- [ ] Document dependencies (libwebrtc, protobuf, etc.)
- [ ] Document build flags and configuration
- [ ] Identify any API gaps vs `IWebRTCConnector` interface
- [ ] Deploy LiveKit test server using `LiveKit/docker-compose.yml`
- [ ] Generate test JWT tokens using `LiveKit/gen-token.mjs`

### Acceptance Criteria

- [ ] Both SDKs built successfully on Windows 64-bit
- [ ] Basic connectivity test passes for chosen SDK
- [ ] Data and audio functionality validated
- [ ] Complete dependency list documented
- [ ] SDK selection decision documented with rationale
- [ ] Test server deployed and accessible
- [ ] Evaluation report added to `docs/LIVEKIT_SDK_EVALUATION.md`

### Deliverables

- `docs/LIVEKIT_SDK_EVALUATION.md` - Evaluation report with recommendation
- LiveKit test server running (Docker)
- Token generation workflow documented

### Related Documentation

- [Quick Start Guide - Phase 1](LIVEKIT_QUICKSTART.md#phase-1-sdk-evaluation-week-1)
- [Implementation Plan - Section 1.1](plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md#11-sdk-evaluation)

**Labels:** `phase-1`, `research`, `webrtc`, `livekit`, `high-priority`  
**Milestone:** M3.4.1b - LiveKit Backend  
**Estimated Time:** 3-5 days

---

## Issue 2: Phase 1 - Integrate LiveKit SDK into Build System

**Title:** `[LiveKit P1] Integrate LiveKit SDK into Unreal plugin build system`

**Description:**

### Objective
Add LiveKit SDK as a thirdparty dependency and update the build system to support conditional compilation via `O3DS_ENABLE_LIVEKIT` flag.

### Prerequisites
- Issue #1 completed (SDK selected)
- SDK built and validated

### Tasks

- [ ] Add LiveKit SDK to `thirdparty/` directory (submodule or vendor)
- [ ] Update `.gitmodules` if using submodule approach
- [ ] Update `Open3DShared.Build.cs` with conditional linking
- [ ] Add `O3DS_ENABLE_LIVEKIT` environment variable support
- [ ] Configure include paths for SDK headers
- [ ] Configure library linking for Windows 64-bit
- [ ] Configure library linking for Linux (if applicable)
- [ ] Configure library linking for macOS (if applicable)
- [ ] Add preprocessor definitions for conditional compilation
- [ ] Test build with `O3DS_ENABLE_LIVEKIT=0` (default - SDK not required)
- [ ] Test build with `O3DS_ENABLE_LIVEKIT=1` (SDK enabled)
- [ ] Update `.gitignore` to exclude SDK build artifacts
- [ ] Document build flags in `BUILD_WEBRTC_LIBS.md`

### Acceptance Criteria

- [ ] SDK integrated into `thirdparty/` directory
- [ ] Plugin builds successfully with `O3DS_ENABLE_LIVEKIT=0`
- [ ] Plugin builds successfully with `O3DS_ENABLE_LIVEKIT=1`
- [ ] No build errors or warnings related to SDK integration
- [ ] All existing tests pass (regression check)
- [ ] Build documentation updated

### Deliverables

- Updated `Open3DShared.Build.cs`
- LiveKit SDK in `thirdparty/` or `ThirdParty/livekit/`
- Updated build documentation

### Related Documentation

- [Implementation Plan - Section 1.2-1.4](plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md#12-dependency-integration-strategy)
- [Quick Start Guide - Step 2](LIVEKIT_QUICKSTART.md#step-2-update-build-system)

**Labels:** `phase-1`, `build-system`, `webrtc`, `livekit`, `high-priority`  
**Milestone:** M3.4.1b - LiveKit Backend  
**Depends On:** #1  
**Estimated Time:** 2-3 days

---

## Issue 3: Phase 2 - Implement FLiveKitConnector Class Skeleton

**Title:** `[LiveKit P2] Create FLiveKitConnector class skeleton implementing IWebRTCConnector`

**Description:**

### Objective
Create the `FLiveKitConnector` class structure with stub implementations of all `IWebRTCConnector` interface methods.

### Prerequisites
- Issue #2 completed (build system integrated)
- SDK available in build

### Tasks

- [ ] Create `LiveKitConnector.h` in `Source/Open3DShared/Private/`
- [ ] Create `LiveKitConnector.cpp` in `Source/Open3DShared/Private/`
- [ ] Add conditional compilation guards (`#if O3DS_ENABLE_LIVEKIT`)
- [ ] Include necessary SDK headers
- [ ] Define `FLiveKitConnector` class inheriting from `IWebRTCConnector`
- [ ] Stub all interface methods:
  - [ ] `Start(const FO3DSWebRtcConfig& Config)`
  - [ ] `Stop()`
  - [ ] `Tick(float DeltaSeconds)`
  - [ ] `IsOpen() const`
  - [ ] `Send(const uint8* Data, int32 NumBytes)`
  - [ ] `EnableAudioSend(bool bEnable)`
  - [ ] `SendAudioPcm16(...)`
  - [ ] `OnState()`, `OnData()`, `OnRemoteAudioRtp()` delegate accessors
- [ ] Add private member variables (SDK objects, state flags, delegates)
- [ ] Add helper method declarations
- [ ] Test compilation with `O3DS_ENABLE_LIVEKIT=1`

### Acceptance Criteria

- [ ] `FLiveKitConnector` class compiles successfully
- [ ] All `IWebRTCConnector` methods stubbed (return false/empty for now)
- [ ] Code follows Open3DStream coding standards
- [ ] No compilation errors or warnings
- [ ] Conditional compilation works correctly

### Deliverables

- `Source/Open3DShared/Private/LiveKitConnector.h`
- `Source/Open3DShared/Private/LiveKitConnector.cpp`

### Related Documentation

- [Implementation Plan - Section 2.1](plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md#21-class-structure)
- [Quick Start Guide - Step 1](LIVEKIT_QUICKSTART.md#step-1-create-skeleton-class)

**Labels:** `phase-2`, `implementation`, `webrtc`, `livekit`, `high-priority`  
**Milestone:** M3.4.1b - LiveKit Backend  
**Depends On:** #2  
**Estimated Time:** 1-2 days

---

## Issue 4: Phase 2 - Implement Connection and Authentication

**Title:** `[LiveKit P2] Implement LiveKit room connection, JWT authentication, and state management`

**Description:**

### Objective
Implement the core connection logic including room connection, JWT token authentication, and state change handling.

### Prerequisites
- Issue #3 completed (skeleton class created)

### Tasks

- [ ] Implement `Start()` method:
  - [ ] Parse and validate config (URL, room, token required)
  - [ ] Create LiveKit Room instance
  - [ ] Configure JWT token authentication
  - [ ] Connect to room asynchronously
  - [ ] Register event callbacks
- [ ] Implement `Stop()` method:
  - [ ] Disconnect from room
  - [ ] Clean up SDK objects
  - [ ] Reset state flags
- [ ] Implement `IsOpen()` method
- [ ] Implement connection state tracking
- [ ] Add event handlers:
  - [ ] `OnConnected` - room join success
  - [ ] `OnDisconnected` - connection lost
  - [ ] `OnParticipantConnected` - new participant
  - [ ] `OnParticipantDisconnected` - participant left
- [ ] Implement `HandleConnectionStateChange()` helper
- [ ] Broadcast state changes via `OnState()` delegate
- [ ] Add error handling and logging
- [ ] Implement `Tick()` for non-blocking event processing

### Acceptance Criteria

- [ ] Connector successfully connects to LiveKit room with valid token
- [ ] Connection state changes broadcast correctly
- [ ] Error handling works (invalid token, network failure, etc.)
- [ ] Clean disconnect on `Stop()`
- [ ] No blocking operations on game thread
- [ ] State delegate callbacks fire appropriately
- [ ] Logging follows Open3DStream conventions

### Deliverables

- Functional connection and authentication logic
- State management implementation

### Related Documentation

- [Implementation Plan - Section 2.2](plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md#22-connection-flow)
- [Implementation Plan - Section 2.5](plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md#25-state-management)

**Labels:** `phase-2`, `implementation`, `webrtc`, `livekit`, `high-priority`  
**Milestone:** M3.4.1b - LiveKit Backend  
**Depends On:** #3  
**Estimated Time:** 2-3 days

---

## Issue 5: Phase 2 - Implement Data Channel Functionality

**Title:** `[LiveKit P2] Implement data message publishing/subscription with topic routing`

**Description:**

### Objective
Implement data channel functionality using LiveKit's topic-based data messages, mapping them to DataChannel semantics.

### Prerequisites
- Issue #4 completed (connection working)

### Tasks

- [ ] Implement `Send()` method:
  - [ ] Map to LiveKit `PublishData()` API
  - [ ] Implement topic routing (`o3ds.anim`, `o3ds.ctrl`)
  - [ ] Add message header (topic, seq, timestamp)
  - [ ] Implement backpressure handling
- [ ] Implement lossy data queue (max 2 frames, drop oldest)
- [ ] Implement reliable data queue (max 10 messages, warn on full)
- [ ] Implement data receive handling:
  - [ ] Subscribe to all topics
  - [ ] Parse incoming messages
  - [ ] Forward to `OnData()` delegate
- [ ] Add topic detection logic (animation vs control)
- [ ] Implement message size validation (<= 15 KB)
- [ ] Add data flow logging (when verbose enabled)
- [ ] Test bidirectional data flow

### Acceptance Criteria

- [ ] Data messages successfully published to LiveKit
- [ ] Topic routing works correctly (lossy vs reliable)
- [ ] Backpressure handling prevents queue overflow
- [ ] Received messages forwarded to delegate
- [ ] Message size validation enforces limits
- [ ] Logging shows data flow metrics
- [ ] Behavior matches LibDataChannel semantics

### Deliverables

- Functional data channel implementation
- Queue-based backpressure handling

### Related Documentation

- [Implementation Plan - Section 2.3](plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md#23-data-channel-mapping)
- [Data Messaging Guide](plugins/unreal/Open3DStream/docs/M3.4.1a_Docs/LIVEKIT_DATA_MESSAGING.md)

**Labels:** `phase-2`, `implementation`, `webrtc`, `livekit`, `high-priority`  
**Milestone:** M3.4.1b - LiveKit Backend  
**Depends On:** #4  
**Estimated Time:** 2-3 days

---

## Issue 6: Phase 2 - Implement Audio Track Functionality

**Title:** `[LiveKit P2] Implement audio track publishing/subscription with Opus encoding`

**Description:**

### Objective
Implement audio track publishing and subscription with Opus encoding, subject labeling, and RTP forwarding.

### Prerequisites
- Issue #4 completed (connection working)

### Tasks

- [ ] Implement `EnableAudioSend()` method
- [ ] Implement `SendAudioPcm16()` method:
  - [ ] Queue PCM samples
  - [ ] Create worker thread for encoding
  - [ ] Implement Opus encoding (48kHz, mono/stereo)
  - [ ] Publish via LiveKit audio track
- [ ] Implement audio track creation with subject labels:
  - [ ] Game mix: `o3ds:mix`
  - [ ] Per-subject mic: `o3ds:subject/<Name>`
- [ ] Implement audio receive handling:
  - [ ] Subscribe to remote audio tracks
  - [ ] Parse track names for subject routing
  - [ ] Convert frames to RTP format
  - [ ] Forward to `OnRemoteAudioRtp()` delegate
- [ ] Implement audio announce message (reliable topic)
- [ ] Add audio queue management (similar to LibDataChannel)
- [ ] Test audio publishing
- [ ] Test audio subscription

### Acceptance Criteria

- [ ] PCM audio successfully queued and encoded to Opus
- [ ] Audio tracks published with correct subject labels
- [ ] Remote audio tracks received and forwarded
- [ ] Subject routing works correctly
- [ ] Audio quality matches LibDataChannel
- [ ] No audio thread blocks game thread
- [ ] RTP format matches receiver expectations

### Deliverables

- Functional audio track implementation
- Opus encoding on worker thread
- Subject-aware audio routing

### Related Documentation

- [Implementation Plan - Section 2.4](plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md#24-audio-track-implementation)
- [Audio Subject Association](plugins/unreal/Open3DStream/docs/M3.4.1a_Docs/WEBRTC_AUDIO_SUBJECT_ASSOCIATION.md)

**Labels:** `phase-2`, `implementation`, `audio`, `webrtc`, `livekit`, `high-priority`  
**Milestone:** M3.4.1b - LiveKit Backend  
**Depends On:** #4  
**Estimated Time:** 3-4 days

---

## Issue 7: Phase 3 - Update Factory and Configuration Wiring

**Title:** `[LiveKit P3] Update WebRTCConnectorFactory and wire configuration from UI`

**Description:**

### Objective
Integrate `FLiveKitConnector` into the factory pattern and ensure configuration flows correctly from Broadcast Component and LiveLink Source to the connector.

### Prerequisites
- Issues #3-6 completed (FLiveKitConnector fully implemented)

### Tasks

- [ ] Update `WebRTCConnectorFactory.cpp`:
  - [ ] Add case for `EO3DSWebRtcBackend::LiveKit`
  - [ ] Instantiate `FLiveKitConnector` when selected
  - [ ] Add logging for backend selection
- [ ] Verify config flow from `UO3DSBroadcastComponent`:
  - [ ] Backend selection property wired
  - [ ] LiveKit URL, Room, Token properties wired
  - [ ] Config passed to connector's `Start()` method
- [ ] Verify config flow from `FOpen3DStreamSource`:
  - [ ] Backend selection wired
  - [ ] LiveKit properties wired
  - [ ] Config passed to connector
- [ ] Test backend selection UI in Broadcast Component
- [ ] Test backend selection UI in LiveLink Source
- [ ] Add config validation:
  - [ ] Warn if token missing
  - [ ] Warn if URL invalid
  - [ ] Warn if room empty
- [ ] Test switching between LibDataChannel and LiveKit backends
- [ ] Verify no impact on existing LibDataChannel usage

### Acceptance Criteria

- [ ] Factory creates `FLiveKitConnector` when backend = LiveKit
- [ ] Config flows correctly from both UI contexts
- [ ] Backend selection dropdown shows LiveKit option
- [ ] Config validation prevents invalid setups
- [ ] Switching backends works without errors
- [ ] LibDataChannel users unaffected (regression test)

### Deliverables

- Updated `WebRTCConnectorFactory.cpp`
- Verified config wiring end-to-end

### Related Documentation

- [Implementation Plan - Section 3](plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md#phase-3-configuration-and-factory-integration)
- [Quick Start Guide - Step 3](LIVEKIT_QUICKSTART.md#step-3-update-factory)

**Labels:** `phase-3`, `integration`, `webrtc`, `livekit`, `high-priority`  
**Milestone:** M3.4.1b - LiveKit Backend  
**Depends On:** #3, #4, #5, #6  
**Estimated Time:** 1-2 days

---

## Issue 8: Phase 4 - Testing and Validation

**Title:** `[LiveKit P4] Implement comprehensive testing suite and validation`

**Description:**

### Objective
Create comprehensive tests for the LiveKit connector, validate behavior parity with LibDataChannel, and benchmark performance.

### Prerequisites
- Issue #7 completed (full integration)

### Tasks

**Ground Truth Testing (WebRTCConnectorComponent):**
- [ ] Create test level in ProjectSandbox
- [ ] Add two `UO3DSWebRTCConnectorComponent` instances
- [ ] Configure one as publisher (with debug tone)
- [ ] Configure one as subscriber
- [ ] Test bidirectional data flow
- [ ] Test audio RTP reception
- [ ] Verify clean connection/disconnection

**Unit Tests:**
- [ ] Create `O3DSLiveKitTests.cpp`
- [ ] Test factory creates LiveKit connector
- [ ] Test config validation
- [ ] Test connection lifecycle
- [ ] Test data send/receive
- [ ] Test audio publishing
- [ ] Test error handling

**Integration Tests (Two Editors):**
- [ ] Test Broadcaster → LiveKit → Receiver (animation)
- [ ] Test Broadcaster → LiveKit → Receiver (audio)
- [ ] Test multi-participant (1-to-many)
- [ ] Test late-join scenario
- [ ] Test reconnection after network interruption

**Performance Benchmarking:**
- [ ] Measure connection time
- [ ] Measure end-to-end latency
- [ ] Measure CPU usage (sender/receiver)
- [ ] Measure memory usage
- [ ] Compare to LibDataChannel baseline
- [ ] Document results in performance report

**Documentation:**
- [ ] Update `WEBRTC_TESTING_GUIDE.md` with LiveKit section
- [ ] Add troubleshooting section for LiveKit
- [ ] Document token generation workflow
- [ ] Document LiveKit server deployment

### Acceptance Criteria

- [ ] All unit tests pass
- [ ] Ground truth test validates connectivity
- [ ] Two-editor E2E test passes
- [ ] Multi-participant test works (3+ participants)
- [ ] Performance meets targets (see below)
- [ ] Behavior parity with LibDataChannel verified
- [ ] Documentation updated

**Performance Targets:**
- Connection time: < 5 seconds
- E2E latency: < 200ms
- Data throughput: 10-50 KB/s
- CPU (sender): < 5%
- CPU (receiver): < 3%
- Scales to 10+ receivers

### Deliverables

- `O3DSLiveKitTests.cpp` (unit tests)
- Test level with WebRTCConnectorComponents
- Performance benchmark report
- Updated `WEBRTC_TESTING_GUIDE.md`

### Related Documentation

- [Implementation Plan - Phase 4](plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md#phase-4-testing-strategy)
- [Testing Guide](plugins/unreal/Open3DStream/docs/WEBRTC_TESTING_GUIDE.md)

**Labels:** `phase-4`, `testing`, `validation`, `webrtc`, `livekit`, `high-priority`  
**Milestone:** M3.4.1b - LiveKit Backend  
**Depends On:** #7  
**Estimated Time:** 5-7 days

---

## Issue Dependencies Graph

```
Issue #1 (SDK Eval)
    ↓
Issue #2 (Build System)
    ↓
Issue #3 (Skeleton)
    ↓
    ├─→ Issue #4 (Connection)
    |       ↓
    |       ├─→ Issue #5 (Data)
    |       └─→ Issue #6 (Audio)
    |               ↓
    └───────────────┴─→ Issue #7 (Integration)
                            ↓
                        Issue #8 (Testing)
```

---

## Suggested Labels

Create these labels in the repository:
- `phase-1`, `phase-2`, `phase-3`, `phase-4` - Implementation phases
- `livekit` - LiveKit-specific work
- `webrtc` - WebRTC-related
- `audio` - Audio functionality
- `build-system` - Build configuration
- `integration` - Integration work
- `testing` - Testing and validation
- `research` - Research and evaluation
- `documentation` - Documentation updates

---

## Milestone

Create milestone: **M3.4.1b - LiveKit Backend**
- Due date: 12 weeks from start
- Description: "LiveKit SFU backend implementation for scalable multi-party streaming"

---

## Assignment Strategy

1. **Sequential Assignment:** Issues should be assigned in dependency order
2. **Parallel Work:** Issues #5 and #6 can be done in parallel after #4
3. **Code Review:** Each issue requires review before next issue starts
4. **Testing Checkpoints:** Validate after each phase before proceeding

---

## Communication Plan

- **Daily Standups:** Progress updates on current issue
- **Phase Reviews:** Review at end of each phase (1-4)
- **Weekly Reports:** Progress against timeline
- **Blocker Escalation:** Immediate communication if blocked

---

## Success Metrics

Track these metrics across all issues:
- **Velocity:** Issues completed per week
- **Quality:** Test pass rate, code review feedback
- **Performance:** Benchmark results vs targets
- **Documentation:** Coverage completeness

---

**Next Steps:**
1. Create these 8 issues in GitHub
2. Assign to milestone M3.4.1b
3. Apply appropriate labels
4. Assign Issue #1 to kick off Phase 1
5. Begin SDK evaluation
