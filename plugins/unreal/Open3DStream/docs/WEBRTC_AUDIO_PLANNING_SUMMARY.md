# WebRTC Audio Integration Planning Summary

**Date:** 2025-10-29  
**Planning Agent:** Open3DStream Planning Agent  
**Status:** Planning Complete — Ready for Implementation

---

## Overview

This document summarizes the comprehensive planning effort for integrating audio capabilities into the Open3DStream WebRTC transport. It serves as a guide for stakeholders and implementation teams to understand the current state, proposed architecture, and execution plan.

---

## Problem Statement

Open3DStream needs production-ready audio streaming over WebRTC to support:
- Real-time motion capture + voice communication
- Multiple audio sources (game mix + per-actor microphones)
- Scalable 1-to-many broadcasting (1–10 senders → 10–10,000+ receivers)
- Consistent behavior across P2P (libdatachannel) and SFU (LiveKit) backends

**Current Limitations:**
- Core C++ library has no audio support
- Unreal plugin supports only one audio track per connection
- LiveKit backend is not implemented
- Subject-aware audio routing is manual (no auto-binding)

---

## Planning Documents

### 1. [WEBRTC_AUDIO_INTEGRATION_PLAN.md](WEBRTC_AUDIO_INTEGRATION_PLAN.md)

**Purpose:** Comprehensive technical plan for full audio integration

**Contents:**
- Detailed current state analysis (Unreal vs core library)
- Architecture and design principles
- 5 implementation phases with task breakdowns
- Technical specifications (Opus codec, sync, messaging)
- Testing and validation strategy
- Risk assessment and mitigation
- Timeline: ~7 weeks (33 days) for full implementation

**Audience:** Engineering leads, architects, project managers

### 2. [WEBRTC_AUDIO_ROADMAP.md](WEBRTC_AUDIO_ROADMAP.md)

**Purpose:** Quick reference guide for implementation teams

**Contents:**
- Phase-by-phase checklists
- Code examples (C++ and Unreal)
- Common issues and solutions
- Performance targets and dependencies
- Step-by-step testing procedures

**Audience:** Individual contributors, QA engineers, DevOps

---

## Executive Summary

### Current State

**What Works:**
- ✅ Unreal plugin: Single Opus audio track via libdatachannel
- ✅ Audio capture from game submix or microphone
- ✅ Remote audio playback via `USoundWaveProcedural`
- ✅ Subject labeling convention defined (`o3ds:mix`, `o3ds:subject/<Name>`)
- ✅ Audio announce message format (`o3ds.audio.announce`)

**What's Missing:**
- ❌ Core C++ library has no audio (blocks Maya, MotionBuilder, CLI tools)
- ❌ Multi-track support (can't stream mix + multiple mics simultaneously)
- ❌ LiveKit SFU backend (limits scale-out to 10–100+ receivers)
- ❌ Automatic subject routing (manual binding required)
- ❌ Unified data messaging headers (topic/seq/timestamp)

### Proposed Solution

Implement audio in **5 phases** over ~7 weeks:

| Phase | Duration | Priority | Deliverable |
|-------|----------|----------|-------------|
| **Phase 1:** Core Library Audio | 5 days | HIGH | C++ apps can stream Opus audio |
| **Phase 2:** Multi-Track Unreal | 7 days | HIGH | Game mix + per-actor mics |
| **Phase 3:** Unified Messaging | 4 days | MEDIUM | Standard message headers |
| **Phase 4:** LiveKit Backend | 10 days | HIGH | SFU support for scale-out |
| **Phase 5:** Scale & Observability | 7 days | MEDIUM | 100+ receivers, monitoring |

**Critical Path:** Phase 1 → Phase 2 → Phase 4 (LiveKit blocks scale testing)

---

## Key Design Decisions

### 1. Audio Track Labeling

**Convention:**
- `o3ds:mix` — Global game audio (not tied to a subject)
- `o3ds:subject/<SubjectName>` — Per-actor microphone

**Implementation:**
- **P2P:** Set as MediaStream ID (`msid`)
- **LiveKit:** Set as `Track.Name`

**Rationale:** Stable across backends; immune to SDP rewrites by SFU

### 2. Multi-Track Architecture

**Pattern:** One Opus encoder per `StreamLabel`

**Why:**
- Independent codec state per track
- Allows per-track bitrate/quality tuning
- Clean separation of game mix vs voice

**Tradeoff:** More CPU, but gains flexibility

### 3. Subject Routing

**Primary:** Parse `msid` (P2P) or `Track.Name` (LiveKit)  
**Fallback:** Use `o3ds.audio.announce` JSON message

**Why:** SFU may rewrite track metadata; announce provides reliable mapping

### 4. Unified Messaging

**Format:** JSON header + FlatBuffers payload

```json
{"topic":"o3ds.anim","v":1,"seq":12345,"ts":1730000000.123,"subject":"Actor_1"}
```

**Why:**
- Topic-based routing for LiveKit data messages
- Sequence/timestamp for lossy drop logic
- Subject field for routing

---

## Success Metrics

### Technical Targets

| Metric | P2P (libdatachannel) | LiveKit SFU |
|--------|----------------------|-------------|
| Audio Latency | <200ms | <500ms |
| A/V Sync Error | <100ms | <100ms |
| CPU (per track) | <10% | <15% |
| Max Receivers | ~10 | 100+ |

### Acceptance Criteria

**Phase 1 (Core Library):**
- ✅ C++ command-line apps can stream Opus audio
- ✅ Build passes on Windows, Linux, macOS

**Phase 2 (Multi-Track):**
- ✅ Can stream game mix + 2 actor mics simultaneously
- ✅ Receiver correctly routes audio by subject
- ✅ Audio/animation sync <100ms drift

**Phase 4 (LiveKit):**
- ✅ Backend selection UI functional
- ✅ Audio quality matches P2P
- ✅ Scales to 10+ receivers with <500ms latency

**Phase 5 (Scale-Out):**
- ✅ 1 sender → 100+ receivers via SFU
- ✅ Latency <1000ms @ 95th percentile
- ✅ Monitoring/telemetry operational

---

## Implementation Approach

### Phase 1: Core Library Audio Foundation (5 days)

**Goal:** Enable C++ command-line tools to stream audio

**Key Tasks:**
- Add Opus dependency to CMakeLists.txt
- Implement `AddAudioTrack()` and `SendAudioFrame()` in `WebRTCClient`
- Add Opus encoding/decoding logic
- Create `O3DS_ENABLE_AUDIO` CMake option
- Test on all platforms

**Risk:** Opus integration may conflict with existing dependencies  
**Mitigation:** Feature flag; test incrementally

### Phase 2: Multi-Track in Unreal (7 days)

**Goal:** Support game mix + per-actor microphones simultaneously

**Key Tasks:**
- Refactor `FWebRTCConnector` for multiple audio tracks
- Implement per-track Opus encoder map
- Set audio track labels in SDP
- Auto-bind `UO3DSRemoteAudioComponent` using announce message

**Risk:** Multi-track may cause CPU spike  
**Mitigation:** Profile; move encoding to worker threads

### Phase 3: Unified Data Messaging (4 days)

**Goal:** Add topic/seq/timestamp header to all messages

**Key Tasks:**
- Define `MessageHeader` struct
- Implement prepend/parse in send/receive paths
- Add lossy drop policy (discard old frames)
- Configure DataChannel reliability modes

**Risk:** Header increases message size  
**Mitigation:** Keep header <100 bytes; test against 15 KB limit

### Phase 4: LiveKit Backend (10 days)

**Goal:** Full LiveKit SFU support with audio

**Key Tasks:**
- Integrate LiveKit C++ SDK
- Implement `FLiveKitConnector` class
- Room join with JWT authentication
- Publish/subscribe audio tracks
- Map data messages to LiveKit topics

**Risk:** LiveKit SDK conflicts with Unreal modules  
**Mitigation:** Static linking; namespace isolation

### Phase 5: Scale-Out & Observability (7 days)

**Goal:** Production-ready at 100+ receivers

**Key Tasks:**
- Implement backpressure for lossy data
- Add telemetry (frames, drops, latency, CPU)
- Create Grafana dashboard
- Load test: 1 sender → 100+ receivers
- Write operational runbook

**Risk:** Scale testing infra not ready  
**Mitigation:** Deploy test LiveKit SFU early; use Docker

---

## Dependencies

### Third-Party Libraries

**Required:**
- libdatachannel (≥0.18, with `USE_MEDIA=ON`)
- Opus codec (≥1.3)
- nlohmann/json (for announce messages)
- MbedTLS (for DTLS)

**Optional (LiveKit):**
- livekit-client-sdk-cpp (≥1.0)

### Build System Changes

**Core Library:**
```cmake
option(O3DS_ENABLE_AUDIO "Enable Opus audio in WebRTC" OFF)
```

**Unreal Plugin:**
```csharp
PublicDefinitions.Add("O3DS_ENABLE_LIVEKIT=1");
```

---

## Timeline & Resources

### Assumptions
- 1 full-time senior engineer
- Phases run mostly sequentially
- Test environment ready by Phase 1

### Critical Path
Phase 1 → Phase 2 → Phase 4 (LiveKit blocks scale-out)

### Parallel Work Opportunities
- Phase 3 (Unified Messaging) can run parallel to Phase 2
- Documentation updates throughout
- Test environment setup in Week 1

### Total Effort
**33 days (~7 weeks)** for production-ready implementation

---

## Risk Assessment

### High-Impact Risks

1. **Opus integration breaks UE build**
   - **Mitigation:** Feature flag; test on all platforms before merge

2. **LiveKit SDK conflicts with UE modules**
   - **Mitigation:** Static linking; namespace isolation

3. **Audio/animation drift >100ms**
   - **Mitigation:** Implement timestamp-based sync with buffer management

### Medium-Impact Risks

1. **SFU rewrites track names**
   - **Mitigation:** Always use `o3ds.audio.announce` as fallback

2. **Multi-track causes CPU spike**
   - **Mitigation:** Profile; optimize encoding; use worker threads

3. **Phase 4 (LiveKit) takes longer than estimated**
   - **Mitigation:** Stub out LiveKit early; iterate in parallel

---

## Testing Strategy

### Unit Tests
- Opus encode/decode round-trip
- Message header serialization
- Subject routing logic
- Lossy drop policy

### Integration Tests
- Local loopback (same process)
- Two-machine LAN (low latency)
- Simulated WAN (latency/jitter/loss)
- Scale test (1 → 10+ receivers)

### End-to-End Smoke Tests

**smoke-webrtc (P2P):**
1. Start signaling server
2. Launch UE sender with audio
3. Launch UE receiver
4. Verify audio plays, animation syncs

**smoke-livekit (SFU):**
1. Deploy LiveKit SFU
2. Generate JWT tokens
3. Launch sender + multiple receivers
4. Verify multi-party streaming works

---

## Documentation Updates

### New Documents
- ✅ WEBRTC_AUDIO_INTEGRATION_PLAN.md
- ✅ WEBRTC_AUDIO_ROADMAP.md
- 🔜 WEBRTC_AUDIO_ARCHITECTURE.md
- 🔜 AUDIO_PERFORMANCE_TUNING.md
- 🔜 LIVEKIT_AUDIO_GUIDE.md

### Updated Documents
- ✅ README.md (added planning links)
- ✅ M3.4.1a_Docs/INDEX.md (added planning section)
- 🔜 WEBRTC_QUICKSTART.md (add audio examples)
- 🔜 BROADCAST_TRANSPORT_GUIDE.md (multi-track setup)
- 🔜 LIBDATACHANNEL_INTEGRATION.md (audio build instructions)

---

## Next Steps

### Immediate Actions

1. **Review & Approve Plan**
   - Engineering lead reviews planning documents
   - Stakeholders approve timeline and resource allocation
   - Identify any missing requirements or risks

2. **Set Up Environment**
   - Clone repository and build core library
   - Install dependencies (Opus, libdatachannel)
   - Deploy test LiveKit SFU (Docker)

3. **Assign Tasks**
   - Allocate engineer(s) to Phase 1
   - Create GitHub issues for each phase
   - Set up project board for tracking

### Week 1 Tasks

- [ ] Phase 1: Add Opus to core library CMakeLists.txt
- [ ] Phase 1: Implement `AudioConfig` struct
- [ ] Phase 1: Add `AddAudioTrack()` to `WebRTCClient`
- [ ] Deploy test signaling server
- [ ] Set up CI for core library audio builds

### Communication

- **Daily Standups:** Track progress, blockers
- **Weekly Reviews:** Demo completed phases
- **Documentation:** Update docs as features land
- **Stakeholder Updates:** Bi-weekly progress reports

---

## Related Issues & References

### GitHub Issues
- **#94** - Backend-agnostic interface (completed)
- **#88** - Phase 2: VR/Interactive Broadcast at Scale
- **#90** - Phase 1: LiveKit Setup
- **#77** - Native WebRTC Audio Track Support (closed, superseded)

### Documentation
- [WEBRTC_AUDIO_STATUS_2025-10-27.md](M3.4.1a_Docs/WEBRTC_AUDIO_STATUS_2025-10-27.md)
- [WEBRTC_BACKENDS.md](M3.4.1a_Docs/WEBRTC_BACKENDS.md)
- [WEBRTC_CONNECTOR_INTERFACE.md](M3.4.1a_Docs/WEBRTC_CONNECTOR_INTERFACE.md)
- [UE_AUDIO_COMPONENTS.md](M3.4.1a_Docs/UE_AUDIO_COMPONENTS.md)

### External Resources
- [Opus Codec RFC 6716](https://datatracker.ietf.org/doc/html/rfc6716)
- [libdatachannel Documentation](https://github.com/paullouisageneau/libdatachannel)
- [LiveKit Documentation](https://docs.livekit.io/)

---

## Conclusion

This planning effort has produced:
1. **Comprehensive technical plan** with 5 phases, detailed tasks, and timeline
2. **Quick reference roadmap** for implementation teams
3. **Clear success metrics** and acceptance criteria
4. **Risk assessment** with mitigation strategies
5. **Testing strategy** covering unit, integration, and smoke tests

**The Open3DStream project is now ready to proceed with WebRTC audio integration.**

Recommended approach: Start with Phase 1 (Core Library Audio) to establish foundation, then proceed to Phase 2 (Multi-Track) and Phase 4 (LiveKit) on the critical path. Phase 3 (Unified Messaging) can run in parallel with Phase 2 if resources allow.

**Estimated completion:** 7 weeks from start, assuming 1 FTE engineer and test environment readiness.

---

**Maintained by:** Open3DStream Planning Agent  
**Last Updated:** 2025-10-29  
**Status:** ✅ Planning Complete — Ready for Implementation
