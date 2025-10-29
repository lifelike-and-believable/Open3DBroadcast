# WebRTC Audio Integration Roadmap

**Quick Reference Guide for Implementation Teams**

This document provides a condensed, actionable roadmap for implementing audio support across the Open3DStream WebRTC transport. For detailed planning, see [WEBRTC_AUDIO_INTEGRATION_PLAN.md](WEBRTC_AUDIO_INTEGRATION_PLAN.md).

---

## TL;DR Status

**What Works Today:**
- ✅ Unreal plugin: Opus audio send/receive via libdatachannel
- ✅ Single audio track per connection
- ✅ Subject labeling convention (`o3ds:mix`, `o3ds:subject/<Name>`)
- ✅ Audio announce message format defined

**What's Missing:**
- ❌ Core C++ library has no audio support
- ❌ Multi-track audio (mix + multiple mics)
- ❌ LiveKit backend implementation
- ❌ Automatic subject routing on receiver
- ❌ Unified data messaging headers

---

## Implementation Checklist

### Phase 1: Core Library Audio (5 days)

**Goal:** Enable C++ command-line tools to stream audio

**Tasks:**
- [ ] Add Opus dependency to `src/CMakeLists.txt`
- [ ] Implement `AudioConfig` struct in `src/o3ds/webrtc_connector.h`
- [ ] Add `AddAudioTrack()` method to `WebRTCClient`
- [ ] Implement Opus encoding in `SendAudioFrame()`
- [ ] Implement Opus decoding in receive callback
- [ ] Add `O3DS_ENABLE_AUDIO` CMake option
- [ ] Create `apps/AudioStreamTest` example app
- [ ] Test on Windows, Linux, macOS

**Deliverable:** `WebRTCClient` can stream Opus audio without Unreal

---

### Phase 2: Multi-Track in Unreal (7 days)

**Goal:** Stream game mix + per-actor microphones simultaneously

**Tasks:**
- [ ] Refactor `FWebRTCConnector` to track multiple audio tracks
- [ ] Create per-track Opus encoder map (`StreamLabel` → encoder)
- [ ] Set `msid` in libdatachannel audio track to `StreamLabel`
- [ ] Update `o3ds.audio.announce` to list all active tracks
- [ ] Parse announce message on receiver side
- [ ] Auto-bind `UO3DSRemoteAudioComponent` based on subject
- [ ] Add UI for multiple `UO3DSBroadcastAudioCaptureComponent` instances
- [ ] Test: Mix + 2 mics streaming in parallel

**Deliverable:** Multi-track audio routing works end-to-end

---

### Phase 3: Unified Data Messaging (4 days)

**Goal:** Add topic/seq/timestamp header to all messages

**Tasks:**
- [ ] Define `MessageHeader` struct (topic, v, seq, ts, subject, stream)
- [ ] Implement prepend/parse logic in `SendDataReliable`/`SendDataLossy`
- [ ] Add lossy drop policy (drop frames with older seq/ts)
- [ ] Configure DataChannel modes (reliable=ordered, lossy=unordered)
- [ ] Update O3DS serialization to include header
- [ ] Test: Inject out-of-order frames, verify drops

**Deliverable:** All data messages have standard header

---

### Phase 4: LiveKit Backend (10 days)

**Goal:** Full LiveKit SFU support with audio

**Tasks:**
- [ ] Integrate LiveKit C++ SDK (`livekit-client-sdk-cpp`)
- [ ] Implement `FLiveKitConnector` class
- [ ] Room join with JWT authentication
- [ ] Publish audio tracks with `Track.Name = StreamLabel`
- [ ] Subscribe to remote audio tracks
- [ ] Map reliable/lossy data to LiveKit data messages
- [ ] Handle SFU edge cases (late join, renegotiation)
- [ ] Update UI with LiveKit config fields
- [ ] Deploy test LiveKit SFU (Docker)
- [ ] Test: 1 sender → 5+ receivers via SFU

**Deliverable:** LiveKit backend feature-complete

---

### Phase 5: Scale-Out & Observability (7 days)

**Goal:** Production-ready at scale (100+ receivers)

**Tasks:**
- [ ] Implement backpressure for lossy data (queue depth limits)
- [ ] Add telemetry (frames sent/received, drops, latency, CPU)
- [ ] Expose stats via Blueprint properties
- [ ] Create Grafana dashboard for monitoring
- [ ] Write operational runbook for scale-out
- [ ] Load test: 1 sender → 100+ receivers
- [ ] Measure latency distribution, identify bottlenecks
- [ ] Verify graceful degradation under network stress

**Deliverable:** System scales to 100+ receivers with <1s latency

---

## Quick Start: Testing Audio Today

**Prerequisites:**
- Unreal plugin built with `O3DS_WITH_OPUS` enabled
- libdatachannel built with `USE_MEDIA=ON`
- Signaling server running (`examples/signaling-server.js`)

**Test P2P Audio:**

1. **Terminal 1:** Start signaling server
   ```bash
   cd plugins/unreal/Open3DStream/examples
   npm install
   node signaling-server.js
   ```

2. **Terminal 2:** Launch UE Sender
   - Open Unreal project
   - Add `UO3DSBroadcastComponent` to an actor
   - Set Transport: WebRTC, Backend: P2P
   - Add `UO3DSBroadcastAudioCaptureComponent`
     - CaptureMode: Mix
     - SubjectName: None
   - Press Play

3. **Terminal 3:** Launch UE Receiver
   - Open second Unreal instance
   - Add Live Link Source: Open3DStream
   - Protocol: WebRTC Client
   - Backend: P2P
   - Same room ID as sender
   - Add `UO3DSRemoteAudioComponent` to an actor
   - Press Play

4. **Verify:**
   - Audio plays on receiver
   - Check logs for "Audio track received"

---

## API Examples

### Core C++ (Proposed)

```cpp
#include "o3ds/webrtc_connector.h"

// Create WebRTC client
O3DS::WebRTCClient client;
client.start("webrtc://localhost:8080/myroom");

// Add audio track
O3DS::AudioConfig cfg;
cfg.streamLabel = "o3ds:mix";
cfg.sampleRate = 48000;
cfg.numChannels = 2;
cfg.bitrateKbps = 128;
client.AddAudioTrack(cfg);

// Send audio frame (20ms @ 48kHz = 960 samples)
float pcmData[960 * 2]; // stereo
// ... fill pcmData with audio ...
client.SendAudioFrame("o3ds:mix", pcmData, 960, 2, 48000);

// Receive audio
client.SetRemoteAudioCallback([](const std::string& subject,
                                  const std::string& stream,
                                  const float* pcm,
                                  size_t frames,
                                  size_t channels,
                                  uint32_t sr) {
    printf("Received %zu audio frames for subject: %s\n", frames, subject.c_str());
    // ... play audio ...
});
```

### Unreal Blueprint

```cpp
// C++ setup for multi-track audio

// Add game mix audio capture
UO3DSBroadcastAudioCaptureComponent* MixCapture = 
    NewObject<UO3DSBroadcastAudioCaptureComponent>(Actor);
MixCapture->CaptureMode = EO3DSCaptureMode::Mix;
MixCapture->SubjectName = NAME_None;
MixCapture->Config.BitrateKbps = 128;
MixCapture->Config.NumChannels = 2;

// Add per-actor mic capture
UO3DSBroadcastAudioCaptureComponent* MicCapture = 
    NewObject<UO3DSBroadcastAudioCaptureComponent>(Actor);
MicCapture->CaptureMode = EO3DSCaptureMode::Input;
MicCapture->SubjectName = FName(TEXT("Actor_1"));
MicCapture->Config.BitrateKbps = 64;
MicCapture->Config.NumChannels = 1;

// Receiver: auto-binds to correct subject
UO3DSRemoteAudioComponent* RemoteAudio = 
    NewObject<UO3DSRemoteAudioComponent>(ReceiverActor);
RemoteAudio->SubjectName = FName(TEXT("Actor_1"));
```

---

## Key Design Decisions

### Audio Track Labeling

**Convention:** `o3ds:mix` (global) or `o3ds:subject/<Name>` (per-actor)

**Why:** Stable across P2P and SFU; immune to SDP rewrites

**Implementation:**
- P2P: Set as MediaStream ID (`msid`)
- LiveKit: Set as `Track.Name`

### Multi-Track Architecture

**Pattern:** One Opus encoder per `StreamLabel`

**Why:** Independent codec state allows per-track bitrate/quality tuning

**Tradeoff:** More CPU for encoding, but gains flexibility

### Subject Routing

**Primary:** Parse `msid` (P2P) or `Track.Name` (LiveKit)

**Fallback:** Use `o3ds.audio.announce` JSON message

**Why:** SFU may rewrite track metadata; announce is reliable

### Unified Messaging

**Format:** JSON header + FlatBuffers payload

```json
{"topic":"o3ds.anim","v":1,"seq":12345,"ts":1730000000.123}
```

**Why:** Topic-based routing for LiveKit; seq/ts for drop logic

---

## Common Issues & Solutions

### Issue: Audio not playing on receiver

**Symptoms:** Connection established, but no audio output

**Checklist:**
- [ ] Verify `O3DS_WITH_OPUS` enabled in build
- [ ] Check libdatachannel built with `USE_MEDIA=ON`
- [ ] Confirm `o3ds.audio.announce` received (check logs)
- [ ] Verify `UO3DSRemoteAudioComponent.SubjectName` matches sender
- [ ] Check Unreal audio device output (not muted)

### Issue: Audio/animation out of sync

**Symptoms:** Audio plays 100ms+ before/after animation

**Solution:**
- Implement timestamp-based buffer management
- Target 200ms playout delay on receiver
- Align animation frame application with audio timestamp

### Issue: CPU spike with multi-track

**Symptoms:** >20% CPU for 3+ audio tracks

**Solution:**
- Move Opus encoding to worker thread
- Use DTX (Discontinuous Transmission) for silence
- Lower complexity setting (5 instead of 10)

### Issue: LiveKit track not found

**Symptoms:** Receiver can't find audio track by name

**Solution:**
- Always send `o3ds.audio.announce` on connect
- Use announce message to map track IDs to subjects
- Log received track names for debugging

---

## Dependencies Quick Reference

| Component | Dependency | Version | Purpose |
|-----------|-----------|---------|---------|
| Core Library | libdatachannel | ≥0.18 | P2P WebRTC |
| Core Library | Opus | ≥1.3 | Audio codec |
| Core Library | nlohmann/json | ≥3.10 | Announce messages |
| UE Plugin | Unreal Engine | 5.6 | Game engine |
| LiveKit | livekit-client-sdk-cpp | ≥1.0 | SFU client |

**Build Flags:**
- `O3DS_ENABLE_WEBRTC` - Enable WebRTC transport (default: ON)
- `O3DS_ENABLE_AUDIO` - Enable audio in core library (default: OFF)
- `O3DS_WITH_OPUS` - Enable Opus in Unreal plugin (default: OFF)
- `O3DS_ENABLE_LIVEKIT` - Enable LiveKit backend (default: OFF)

---

## Performance Targets

| Metric | P2P | LiveKit SFU |
|--------|-----|-------------|
| Audio Latency | <200ms | <500ms |
| A/V Sync Error | <100ms | <100ms |
| CPU (per track) | <10% | <15% |
| Bandwidth (mono voice) | 32-64 kbps | 32-64 kbps |
| Bandwidth (stereo mix) | 64-128 kbps | 64-128 kbps |
| Max Receivers (1 sender) | ~10 | 100+ |

---

## Next Steps

1. **Prioritize:** Decide which phase to tackle first based on project needs
2. **Assign:** Allocate engineers to each phase
3. **Track:** Use GitHub project board to monitor progress
4. **Test:** Run smoke tests after each phase
5. **Document:** Update docs with learnings
6. **Iterate:** Gather feedback and refine

For detailed implementation plan, see [WEBRTC_AUDIO_INTEGRATION_PLAN.md](WEBRTC_AUDIO_INTEGRATION_PLAN.md).

---

**Maintained by:** Open3DStream Team  
**Last Updated:** 2025-10-29
