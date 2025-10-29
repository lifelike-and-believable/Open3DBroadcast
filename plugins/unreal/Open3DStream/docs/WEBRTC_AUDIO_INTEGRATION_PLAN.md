# WebRTC Audio Integration Plan

**Date:** 2025-10-29  
**Status:** Planning Phase  
**Related Issues:** #94 (completed), #88, #90  
**Authors:** Open3DStream Planning Agent

---

## Executive Summary

This document outlines a comprehensive plan for integrating audio capabilities into the Open3DStream WebRTC transport layer. The plan addresses both the C++ core library and the Unreal Engine plugin, ensuring consistent audio streaming across P2P (libdatachannel) and LiveKit SFU backends.

**Current State:**
- ✅ Unreal plugin has working Opus audio (send/receive) via libdatachannel (Issue #94)
- ✅ Backend-agnostic interface (`IWebRTCConnector`) defined with audio APIs
- ❌ Core C++ library (`src/o3ds/webrtc_connector.cpp`) has **no audio support**
- ❌ LiveKit backend is stubbed but not implemented
- ⚠️  Subject-aware audio routing is partially implemented (announce message exists, auto-binding missing)

**Goal:**  
Enable production-ready audio streaming over WebRTC for both P2P and SFU scenarios, supporting:
- Multiple audio tracks per connection (game mix + per-subject microphones)
- Opus encoding/decoding at 48 kHz
- Subject-aware routing and synchronization with animation data
- Scalable architecture for 1-to-many broadcasting

---

## 1. Current State Analysis

### 1.1 Unreal Plugin Audio Implementation

**Location:** `plugins/unreal/Open3DStream/Source/Open3DStream/`

**Status:** Partially Implemented (LibDataChannel only)

**Components:**
- `UO3DSBroadcastAudioCaptureComponent` - Captures game audio (submix) and/or microphone
- `UO3DSRemoteAudioComponent` - Plays remote audio for specific subjects
- `FWebRTCConnector` - Low-level libdatachannel wrapper with Opus send/receive
- `IWebRTCConnector` - Backend-agnostic interface with audio APIs
- `FLibDataChannelAdapter` - Adapts `FWebRTCConnector` to `IWebRTCConnector`

**Features:**
- ✅ Opus encoding/decoding (48 kHz, 1-2 channels)
- ✅ PCM16 ↔ float conversion
- ✅ RTP timestamp management for A/V sync
- ✅ `o3ds.audio.announce` JSON message for track metadata
- ✅ Per-track labels (`o3ds:mix`, `o3ds:subject/<Name>`)
- ⚠️  Manual subject routing (no auto-binding from announce message)
- ❌ Multi-track support (only one audio track per connection currently)

**Build Flags:**
- `O3DS_WITH_OPUS` - Enables Opus codec
- `O3DS_OPUS_NO_HEADER` - Controls Opus header inclusion
- `O3DS_ENABLE_LIVEKIT` - Feature gates LiveKit backend (default: OFF)

**Dependencies:**
- libdatachannel (built with `USE_MEDIA=ON`, `USE_OPUS=ON`, `USE_MBEDTLS=ON`)
- Opus codec library
- Unreal Engine audio APIs (`USoundWaveProcedural`, `UAudioComponent`, `ISubmixBufferListener`)

**Gaps:**
1. Only one audio track per connection (need multiple for mix + mics)
2. No automatic subject routing based on announce message
3. No LiveKit implementation
4. Audio track labels not set in SDP (currently named "audio")
5. No unified data messaging header (topic/seq/timestamp)

### 1.2 Core C++ Library Status

**Location:** `src/o3ds/webrtc_connector.cpp`

**Status:** ❌ No Audio Support

The core library's `WebRTCClient` and `WebRTCServer` classes:
- Have basic WebRTC DataChannel support for animation streaming
- Use libdatachannel for P2P connections
- **Do NOT include any audio track functionality**
- Do not link against Opus codec
- Cannot send or receive audio streams

**Impact:**
- C++ command-line tools (Repeater, SubscribeTest) cannot stream audio
- Non-Unreal integrations (Maya, MotionBuilder) lack audio capability
- Testing and debugging audio issues outside Unreal is difficult

**Required Changes:**
- Add Opus encoding/decoding to core library
- Extend `WebRTCClient`/`WebRTCServer` with audio track APIs
- Mirror Unreal's audio configuration structs
- Provide C++ examples for audio streaming

### 1.3 LiveKit Backend Status

**Status:** ❌ Not Implemented (Stub Only)

**Location:** `plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTCConnectorFactory.cpp`

Currently returns `nullptr` for `EO3DSWebRtcBackend::LiveKit`.

**Requirements for LiveKit:**
1. Integrate LiveKit C++ SDK
2. Implement room join with JWT authentication
3. Map DataChannel semantics to LiveKit data messages (reliable/lossy)
4. Publish Opus audio tracks with proper Track.Name labels
5. Subscribe to remote audio tracks with subject routing
6. Handle SFU-specific scenarios (late join, track renegotiation, bandwidth adaptation)

**Challenges:**
- LiveKit's data API is topic-based (not arbitrary SCTP channels)
- SDP/track names may be rewritten by SFU
- Need robust fallback to `o3ds.audio.announce` for routing

---

## 2. Architecture & Design Principles

### 2.1 Core Design Goals

1. **Backend Agnostic:** Audio APIs should work identically across P2P and LiveKit
2. **Subject-Aware:** Every audio track is associated with a LiveLink subject (or global mix)
3. **Low Latency:** Target <200ms glass-to-glass latency for interactive scenarios
4. **Scalable:** Support 1-to-many broadcasting (1–10 senders → 10–10,000+ receivers)
5. **Testable:** Core audio logic should be testable without Unreal Engine

### 2.2 Audio Track Labeling Convention

All audio tracks use standardized labels for routing:

- **Game Mix:** `o3ds:mix` (global audio, not tied to a subject)
- **Per-Subject Microphone:** `o3ds:subject/<SubjectName>` (e.g., `o3ds:subject/Actor_1`)

**Implementation:**
- **P2P (libdatachannel):** Set as MediaStream ID (`msid`) when adding track
- **LiveKit:** Set as `Track.Name` when publishing track

### 2.3 Audio Announce Message

On connection establishment, sender broadcasts metadata via reliable data channel:

```json
{
  "type": "o3ds.audio.announce",
  "tracks": [
    {
      "stream": "o3ds:mix",
      "track": "unique-track-id-0",
      "subject": "",
      "source": "mix",
      "sr": 48000,
      "ch": 2,
      "br": 128
    },
    {
      "stream": "o3ds:subject/Actor_1",
      "track": "unique-track-id-1",
      "subject": "Actor_1",
      "source": "mic",
      "sr": 48000,
      "ch": 1,
      "br": 64
    }
  ]
}
```

**Fields:**
- `stream`: Label used for routing (`o3ds:mix` or `o3ds:subject/<Name>`)
- `track`: Unique track identifier
- `subject`: LiveLink subject name (empty for mix)
- `source`: `"mic"` or `"mix"`
- `sr`, `ch`, `br`: Sample rate, channels, bitrate (kbps)

### 2.4 Unified Data Messaging

All animation and control messages should include a standard header:

```json
{
  "topic": "o3ds.anim",
  "v": 1,
  "seq": 12345,
  "ts": 1730000000.123,
  "subject": "Actor_1",
  "stream": "o3ds:subject/Actor_1"
}
```

**Topic Names:**
- `o3ds.anim` - Animation frames (lossy delivery)
- `o3ds.ctrl` - Control messages (reliable delivery)
- `o3ds.audio.announce` - Audio track metadata (reliable delivery)

**Delivery Modes:**
- **Lossy:** Unordered, unreliable; drop old frames based on seq/ts
- **Reliable:** Ordered, reliable; for critical state updates

---

## 3. Implementation Phases

### Phase 1: Core Library Audio Foundation (Priority: HIGH)

**Goal:** Add Opus audio support to `src/o3ds/webrtc_connector.cpp`

**Deliverables:**
1. Add Opus dependency to CMakeLists.txt
2. Extend `WebRTCClient` with audio track APIs:
   - `bool AddAudioTrack(const AudioConfig& config)`
   - `bool SendAudioFrame(const float* pcm, size_t frames, size_t channels, uint32_t sampleRate)`
   - `void OnRemoteAudioFrame(AudioFrameCallback callback)`
3. Implement Opus encoding/decoding
4. Add audio configuration structs (mirror Unreal's)
5. Update CMake to conditionally compile audio (e.g., `O3DS_ENABLE_AUDIO` flag)
6. Create C++ example: `apps/AudioStreamTest`

**Testing:**
- Build core library with and without `O3DS_ENABLE_AUDIO`
- Test P2P audio stream between two C++ apps
- Verify Opus encoding quality and latency

**Acceptance Criteria:**
- Core library can send/receive Opus audio over WebRTC DataChannel
- C++ command-line apps can stream audio without Unreal
- Build passes on Windows, Linux, macOS

**Estimated Effort:** 3-5 days

---

### Phase 2: Multi-Track Audio in Unreal Plugin (Priority: HIGH)

**Goal:** Support multiple simultaneous audio tracks (game mix + per-actor mics)

**Deliverables:**
1. Refactor `FWebRTCConnector` to support multiple audio tracks
2. Implement per-track Opus encoders (one encoder per `StreamLabel`)
3. Set audio track labels in SDP/media stream ID
4. Update `o3ds.audio.announce` to list all active tracks
5. Parse announce message on receiver side
6. Auto-bind `UO3DSRemoteAudioComponent` to correct subject based on labels

**Testing:**
- Create two `UO3DSBroadcastAudioCaptureComponent` instances:
  - One for game mix (`SubjectName = None`)
  - One for Actor_1 mic (`SubjectName = Actor_1`)
- Verify receiver gets two distinct audio tracks
- Confirm `UO3DSRemoteAudioComponent(Actor_1)` plays only Actor_1's mic

**Acceptance Criteria:**
- Can stream game mix + 2 actor microphones simultaneously
- Receiver correctly routes audio to subject-specific components
- Audio/animation sync maintained (<100ms drift)

**Estimated Effort:** 5-7 days

---

### Phase 3: Unified Data Messaging Layer (Priority: MEDIUM)

**Goal:** Add topic/seq/timestamp header to all data messages

**Deliverables:**
1. Define message header struct in both C++ and Unreal
2. Implement prepend/parse logic in `SendDataReliable`/`SendDataLossy`
3. Add lossy drop policy (queue max depth, drop oldest)
4. Configure DataChannel reliability modes (ordered/unordered)
5. Update serialization code to include headers

**Testing:**
- Send animation frames with header; verify seq/ts monotonically increase
- Inject out-of-order frames; confirm lossy path drops old frames
- Test reliable path with control messages

**Acceptance Criteria:**
- All messages have topic/seq/ts header
- Lossy path drops frames older than latest applied
- Message size remains ≤15 KB

**Estimated Effort:** 3-4 days

---

### Phase 4: LiveKit Backend Implementation (Priority: HIGH)

**Goal:** Implement full LiveKit SFU support with audio

**Deliverables:**
1. Integrate LiveKit C++ SDK (`livekit-client-sdk-cpp`)
2. Implement `FLiveKitConnector` class:
   - Room join with JWT token
   - Publish/subscribe audio tracks
   - Reliable/lossy data messaging
3. Map P2P audio APIs to LiveKit equivalents
4. Handle SFU-specific edge cases (late join, track renegotiation)
5. Update UI to expose LiveKit config (ServerUrl, Room, Token)

**Testing:**
- Deploy LiveKit SFU (Docker or cloud)
- Test 1 sender → 2+ receivers
- Verify audio/animation sync at scale
- Test late join, reconnection, bandwidth degradation

**Acceptance Criteria:**
- Backend selection works in both sender and receiver UI
- LiveKit audio quality matches libdatachannel
- Can scale to 10+ concurrent receivers
- End-to-end latency <500ms (95th percentile)

**Estimated Effort:** 7-10 days

---

### Phase 5: Scale-Out & Observability (Priority: MEDIUM)

**Goal:** Support 1-to-many broadcasting at scale (10–10,000+ viewers)

**Deliverables:**
1. Implement backpressure handling for lossy data
2. Add telemetry (frames sent/received, drops, latency, bitrate)
3. Expose stats via Blueprint and C++ APIs
4. Create monitoring dashboard (Grafana + Prometheus)
5. Write operational runbook for scale-out scenarios

**Testing:**
- Load test: 1 sender → 100+ receivers
- Measure CPU/bandwidth/latency at scale
- Inject network failures; verify recovery

**Acceptance Criteria:**
- 1 sender can broadcast to 100+ receivers via LiveKit
- Latency remains <1000ms at 95th percentile
- Graceful degradation under network stress
- Monitoring captures key metrics

**Estimated Effort:** 5-7 days

---

## 4. Technical Specifications

### 4.1 Audio Configuration

**Codec:** Opus (RFC 6716)

**Settings:**
- **Sample Rate:** 48 kHz (fixed)
- **Channels:** 1 (mono) or 2 (stereo)
- **Frame Size:** 20 ms (960 samples @ 48 kHz)
- **Bitrate:** 
  - Voice/Mic: 32–64 kbps
  - Game Mix: 64–128 kbps
- **DTX (Discontinuous Transmission):** Enabled for bandwidth savings
- **FEC (Forward Error Correction):** Configurable (default: OFF)
- **Complexity:** 5–10 (tradeoff: quality vs CPU)

### 4.2 Data Channel Configuration

**P2P (libdatachannel):**
- **Lossy:** `ordered=false`, `maxRetransmits=0`
- **Reliable:** `ordered=true`, `maxRetransmits=unlimited`

**LiveKit:**
- **Lossy:** `kind="lossy"`, topic-based delivery
- **Reliable:** `kind="reliable"`, topic-based delivery

### 4.3 Message Size Limits

- **Target:** ≤15 KB per message (avoid SCTP fragmentation)
- **Enforcement:** Chunk large messages with reassembly logic
- **Timeout:** 5 seconds for reassembly; discard incomplete messages

### 4.4 Synchronization Strategy

**Approach:** Use RTP timestamps for audio; align animation via monotonic clock

**Implementation:**
1. Sender includes `ts` (seconds since epoch) in data header
2. Receiver buffers audio with target latency (e.g., 200ms)
3. Animation frames applied when `ts` matches audio playout time
4. If drift >100ms, warn and resync

---

## 5. Testing & Validation Strategy

### 5.1 Unit Tests

**Location:** `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Tests/`

**Coverage:**
- Opus encode/decode round-trip (verify PCM fidelity)
- Message header serialization/deserialization
- Subject routing logic (announce message parsing)
- Lossy drop policy (seq/ts-based discard)

### 5.2 Integration Tests

**Scenarios:**
1. **Local loopback:** Sender and receiver in same process
2. **Two-machine LAN:** Low latency, no packet loss
3. **Simulated WAN:** Inject latency (100ms), jitter (20ms), loss (1%)
4. **Scale test:** 1 sender → 10+ receivers

**Metrics:**
- Audio latency (glass-to-glass)
- Animation sync error (audio vs mocap)
- CPU usage (sender/receiver)
- Bandwidth (kbps per stream)

### 5.3 End-to-End Smoke Tests

**smoke-webrtc (P2P):**
```bash
# Terminal 1: Run signaling server
cd plugins/unreal/Open3DStream/examples
node signaling-server.js

# Terminal 2: Launch UE sender (broadcast)
# Configure: WebRTC, P2P backend, enable audio

# Terminal 3: Launch UE receiver (Live Link Source)
# Configure: WebRTC Client, same room ID

# Verify: Audio plays, animation syncs, no drops
```

**smoke-livekit (SFU):**
```bash
# Prerequisite: LiveKit SFU running (Docker or cloud)

# Terminal 1: Generate JWT token
cd LiveKit
node gen-token.mjs --room=test --identity=sender

# Terminal 2: Launch UE sender
# Configure: WebRTC, LiveKit backend, paste token

# Terminal 3: Launch UE receiver(s)
# Configure: LiveKit backend, same room, subscriber token

# Verify: Multi-party audio/animation streaming works
```

### 5.4 Acceptance Criteria Summary

**Phase 1 (Core Library):**
- ✅ C++ apps can stream Opus audio
- ✅ Builds on Windows/Linux/macOS

**Phase 2 (Multi-Track):**
- ✅ Game mix + 2 mics stream simultaneously
- ✅ Subject routing works correctly

**Phase 3 (Unified Messaging):**
- ✅ All messages have header
- ✅ Lossy path drops old frames

**Phase 4 (LiveKit):**
- ✅ Backend selection UI functional
- ✅ Audio quality matches P2P
- ✅ Scales to 10+ receivers

**Phase 5 (Scale-Out):**
- ✅ 1 → 100+ receivers via SFU
- ✅ Latency <1000ms @ 95th percentile
- ✅ Monitoring/telemetry operational

---

## 6. Dependencies & Prerequisites

### 6.1 Third-Party Libraries

**Required:**
- libdatachannel (≥0.18, with `USE_MEDIA=ON`)
- Opus codec (≥1.3)
- nlohmann/json (for announce messages)
- MbedTLS (for DTLS)

**Optional (LiveKit):**
- livekit-client-sdk-cpp (≥1.0)

### 6.2 Build System Changes

**CMakeLists.txt (Core Library):**
```cmake
option(O3DS_ENABLE_AUDIO "Enable Opus audio in WebRTC" OFF)

if(O3DS_ENABLE_AUDIO)
  find_package(Opus REQUIRED)
  target_compile_definitions(o3ds PRIVATE O3DS_WITH_AUDIO)
  target_link_libraries(o3ds PRIVATE Opus::opus)
endif()
```

**Unreal Plugin Build.cs:**
```csharp
if (bEnableWebRTC && bEnableLiveKit)
{
    PublicDefinitions.Add("O3DS_ENABLE_LIVEKIT=1");
    PublicAdditionalLibraries.Add("livekit-client.lib");
}
```

### 6.3 Documentation Updates

**New Documents:**
- `WEBRTC_AUDIO_ARCHITECTURE.md` - High-level audio design
- `AUDIO_PERFORMANCE_TUNING.md` - Codec settings and optimization
- `LIVEKIT_AUDIO_GUIDE.md` - LiveKit-specific audio setup

**Updated Documents:**
- `WEBRTC_QUICKSTART.md` - Add audio examples
- `BROADCAST_TRANSPORT_GUIDE.md` - Document multi-track setup
- `LIBDATACHANNEL_INTEGRATION.md` - Audio build instructions

---

## 7. Risk Assessment & Mitigation

### 7.1 Technical Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Opus integration breaks UE build | High | Medium | Feature flag; test on all platforms before merge |
| LiveKit SDK conflicts with UE modules | High | Low | Static linking; namespace isolation |
| Audio/animation drift >100ms | Medium | Medium | Implement timestamp-based sync with buffer management |
| SFU rewrites track names | Medium | High | Always use `o3ds.audio.announce` as fallback |
| Multi-track causes CPU spike | Medium | Low | Profile; optimize encoding pipeline; use worker threads |

### 7.2 Schedule Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| Phase 4 (LiveKit) takes >10 days | Medium | Stub out LiveKit first; iterate in parallel |
| Opus build issues on macOS | Low | Use Homebrew; document build steps |
| Testing infra not ready | High | Set up test environment in Phase 1 |

---

## 8. Milestones & Timeline

**Assumptions:**
- 1 full-time engineer
- Phases run sequentially (some parallelization possible)

| Phase | Duration | Start | End | Deliverables |
|-------|----------|-------|-----|--------------|
| Phase 1: Core Audio | 5 days | Week 1 | Week 1 | C++ audio streaming |
| Phase 2: Multi-Track | 7 days | Week 2 | Week 2 | Game mix + mics |
| Phase 3: Unified Messaging | 4 days | Week 3 | Week 3 | Header implementation |
| Phase 4: LiveKit | 10 days | Week 4 | Week 5 | SFU support |
| Phase 5: Scale-Out | 7 days | Week 6 | Week 6 | Observability |
| **Total** | **33 days** | - | **~7 weeks** | Production-ready |

**Critical Path:** Phase 1 → Phase 2 → Phase 4 (LiveKit blocks scale-out testing)

---

## 9. Success Metrics

**Technical:**
- ✅ Audio latency <200ms (P2P), <500ms (LiveKit)
- ✅ A/V sync error <100ms (95th percentile)
- ✅ Zero audio dropouts under normal network conditions
- ✅ CPU usage <10% per audio track (encoding + decoding)

**Operational:**
- ✅ 1 sender → 100+ receivers via LiveKit SFU
- ✅ Monitoring dashboards operational
- ✅ On-call runbook complete

**Documentation:**
- ✅ All docs updated
- ✅ Code examples for C++ and Blueprint
- ✅ Troubleshooting guide for common issues

---

## 10. Open Questions & Future Work

### 10.1 Open Questions

1. **Q:** Should we support variable bitrate (VBR) encoding?  
   **A:** Yes, for bandwidth efficiency. Use `OPUS_SET_VBR(1)`.

2. **Q:** How do we handle multiple submixes (e.g., voice + music separately)?  
   **A:** Each submix gets its own `UO3DSBroadcastAudioCaptureComponent` instance.

3. **Q:** Should core library expose C API for non-C++ bindings?  
   **A:** Out of scope for this phase; revisit in future milestone.

4. **Q:** Do we need echo cancellation for bidirectional mic streams?  
   **A:** Not yet; focus on broadcast (1-to-many) first.

### 10.2 Future Enhancements

**Short-Term (Post-Phase 5):**
- Advanced A/V sync (PTS/DTS alignment)
- Audio spatialization (3D positional audio)
- Dynamic bitrate adaptation (based on network conditions)

**Long-Term:**
- Additional codecs (LC3, AAC) for compatibility
- Low-latency HLS/DASH fallback for mass audience
- Integration with Unreal MetaSounds for procedural audio

---

## 11. References

**Related Issues:**
- #94 - Backend-agnostic interface (completed)
- #88 - Phase 2: VR/Interactive Broadcast at Scale
- #90 - Phase 1: LiveKit Setup
- #77 - Native WebRTC Audio Track Support (closed)

**Documentation:**
- [WEBRTC_AUDIO_STATUS_2025-10-27.md](M3.4.1a_Docs/WEBRTC_AUDIO_STATUS_2025-10-27.md)
- [WEBRTC_BACKENDS.md](M3.4.1a_Docs/WEBRTC_BACKENDS.md)
- [WEBRTC_CONNECTOR_INTERFACE.md](M3.4.1a_Docs/WEBRTC_CONNECTOR_INTERFACE.md)
- [UE_AUDIO_COMPONENTS.md](M3.4.1a_Docs/UE_AUDIO_COMPONENTS.md)
- [WEBRTC_AUDIO_SUBJECT_ASSOCIATION.md](M3.4.1a_Docs/WEBRTC_AUDIO_SUBJECT_ASSOCIATION.md)

**External Resources:**
- [Opus Codec RFC 6716](https://datatracker.ietf.org/doc/html/rfc6716)
- [libdatachannel Documentation](https://github.com/paullouisageneau/libdatachannel)
- [LiveKit Documentation](https://docs.livekit.io/)
- [Unreal Engine Audio API](https://dev.epicgames.com/documentation/en-us/unreal-engine/audio-programming-in-unreal-engine)

---

## 12. Appendix: Code Examples

### A. Core Library Audio API (Proposed)

```cpp
// src/o3ds/webrtc_connector.h
namespace O3DS {

struct AudioConfig {
    std::string streamLabel;  // e.g., "o3ds:mix" or "o3ds:subject/Actor_1"
    std::string subjectName;  // LiveLink subject (empty for mix)
    int sampleRate = 48000;
    int numChannels = 1;
    int bitrateKbps = 64;
    bool useDTX = true;
};

class WebRTCClient : public AsyncConnector {
public:
    // Add audio track to peer connection
    bool AddAudioTrack(const AudioConfig& config);
    
    // Send audio frame (PCM float, interleaved)
    bool SendAudioFrame(const std::string& streamLabel, 
                       const float* pcm, 
                       size_t numFrames, 
                       size_t numChannels, 
                       uint32_t sampleRate);
    
    // Register callback for remote audio
    using AudioFrameCallback = std::function<void(
        const std::string& subject,
        const std::string& streamLabel,
        const float* pcm,
        size_t numFrames,
        size_t numChannels,
        uint32_t sampleRate
    )>;
    void SetRemoteAudioCallback(AudioFrameCallback callback);
};

} // namespace O3DS
```

### B. Unreal Multi-Track Setup (Blueprint)

```cpp
// Create broadcast component
auto* Broadcast = NewObject<UO3DSBroadcastComponent>(Actor);

// Add game mix audio
auto* MixAudio = NewObject<UO3DSBroadcastAudioCaptureComponent>(Actor);
MixAudio->CaptureMode = EO3DSCaptureMode::Mix;
MixAudio->SubjectName = NAME_None;
MixAudio->Config.BitrateKbps = 128;
MixAudio->Config.NumChannels = 2;

// Add per-actor mic
auto* MicAudio = NewObject<UO3DSBroadcastAudioCaptureComponent>(Actor);
MicAudio->CaptureMode = EO3DSCaptureMode::Input;
MicAudio->SubjectName = FName(TEXT("Actor_1"));
MicAudio->Config.BitrateKbps = 64;
MicAudio->Config.NumChannels = 1;

// Receiver: bind to specific subject
auto* RemoteAudio = NewObject<UO3DSRemoteAudioComponent>(ReceiverActor);
RemoteAudio->SubjectName = FName(TEXT("Actor_1"));
```

---

**End of Plan**
