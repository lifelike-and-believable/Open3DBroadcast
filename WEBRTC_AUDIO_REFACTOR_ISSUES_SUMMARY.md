# WebRTC Audio Refactor - Issues Summary

**Created:** 2025-10-30  
**Source Document:** `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md`  
**Reference Example:** https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp

This document provides an executive summary of the GitHub Issues that should be created to execute the WebRTC Audio Path Refactor. The full, detailed issue descriptions are available in `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md`.

---

## Overview

The WebRTC audio refactor addresses silent failures and fragile timing in audio setup by:
1. **Unmasking failures** so errors are visible
2. **Decoupling components** to clarify responsibilities  
3. **Centralizing setup** in BroadcastComponent with deterministic ordering
4. **Enforcing contracts** to prevent misuse

The working libdatachannel example shows the critical ordering pattern:
- **Add audio track BEFORE DataChannel** to include audio in initial SDP offer

---

## Issues to Create

### Epic Issue

**Title:** `[EPIC] WebRTC Audio Path Refactor`  
**Labels:** `epic`, `area:unreal`, `area:webrtc`, `audio`

**Purpose:** Tracking issue for the entire refactor effort

**Sub-issues:** Links to Issues #1-7 below

**Estimated Timeline:** 3 days (buffered)

---

### Phase A: Unmask and Contain

#### Issue 1: Unmask EnableAudioSend Failures in Adapter

**Title:** `WebRTC Audio: Unmask EnableAudioSend failures in adapter`  
**Labels:** `area:unreal`, `area:webrtc`, `bug`, `audio`  
**Priority:** High (foundational fix)

**Problem:** Adapter always returns `true` even when inner connector fails, masking critical errors

**Change:** `WebRTCConnectorFactory.cpp` lines 46-63
- Capture and return actual result from `Inner->EnableAudioSend()`
- Only add to `EnabledStreams` on success
- Add SUCCESS/FAILED logging

**Estimate:** 0.5 day

---

#### Issue 2: Decouple Audio Capture Component

**Title:** `WebRTC Audio: Make audio capture component a pure PCM source`  
**Labels:** `area:unreal`, `area:webrtc`, `refactor`, `audio`  
**Priority:** High (foundational refactor)

**Problem:** AudioCaptureComponent owns connector and participates in negotiation, creating timing hazards

**Changes:**
- Remove `Connector` member and `EnsureConnector`/`SetConnector` methods
- Add new API: `SetStreamLabel()` and `SetAudioSink(callback)`
- Update `PushFrames` to use sink instead of connector

**Files:**
- `O3DSBroadcastAudioCaptureComponent.h` (lines 92, 103-119)
- `O3DSBroadcastAudioCaptureComponent.cpp` (lines 46-102, 195-257)

**Estimate:** 1 day

---

### Phase B: Centralize and Order

#### Issue 3: Centralize Audio Setup in BroadcastComponent

**Title:** `WebRTC Audio: Wire audio before transport start in BroadcastComponent`  
**Labels:** `area:unreal`, `area:webrtc`, `refactor`, `audio`  
**Priority:** High  
**Dependencies:** Issue #2

**Problem:** Setup order is unclear; need deterministic sequence

**Change:** `O3DSBroadcastComponent.cpp` lines 358-464 in `StartCapture()`:
1. Create transport (PrepareChannel) - no PeerConnection yet
2. Get connector reference
3. Compute StreamLabel (e.g., `o3ds:subject/<name>` or `o3ds:mic/<device>`)
4. Call `EnableAudioSend(config)` BEFORE Start
5. Wire AudioCaptureComponent with StreamLabel and sink callback
6. Start transport (PeerConnection created with audio)

**Estimate:** 1 day

---

#### Issue 4: Enforce Strict Ordering in Connector

**Title:** `WebRTC Audio: Enforce EnableAudioSend must be called before Start`  
**Labels:** `area:unreal`, `area:webrtc`, `refactor`, `audio`  
**Priority:** High

**Problem:** Need to enforce and validate the ordering contract

**Changes:** `WebRTCConnector.cpp`
- `EnableAudioSend` (line 1601): Enforce must-be-before-Start, return false otherwise
- `SetupPeerConnection` (line 1195): Verify audio track → DataChannel → offer order
- Add SDP validation logs (check for `m=audio` line)
- Ensure bounded buffer and timestamp progression in `PushAudioPCM16`

**Key Reference from libdatachannel example (client.cpp lines 144-168):**
```cpp
pc = createPeerConnection(config, ws);         // Step 1
audioTrack = pc->addTrack(media);              // Step 2: AUDIO FIRST
dc = pc->createDataChannel("test");            // Step 3: DC SECOND (triggers offer)
```

**Estimate:** 1 day

---

### Phase C: Diagnostics and Tests

#### Issue 5: Polish Logging and Diagnostics

**Title:** `WebRTC Audio: Improve logging and status diagnostics`  
**Labels:** `area:unreal`, `area:webrtc`, `enhancement`, `audio`  
**Priority:** Medium  
**Dependencies:** Issues #1-4

**Changes:**
- Standardize log categories
- Add must-have logs for audio path
- Implement comprehensive `o3ds.WebRTC.Audio.Status` console command
- Document CVars for debugging
- Gate hot-path logs with CVars

**Estimate:** 0.5 day

---

#### Issue 6: Create Test Matrix

**Title:** `WebRTC Audio: Implement comprehensive test matrix`  
**Labels:** `area:unreal`, `area:webrtc`, `testing`, `audio`  
**Priority:** Medium  
**Dependencies:** Issues #1-5

**Tests:**
- Unit tests for ordering, masking, component API
- Integration test matrix: Client/Server × Mix/Mic × Negotiated/Non-negotiated
- Error case tests (late EnableAudioSend, missing Opus, invalid config)
- Documentation: `WEBRTC_AUDIO_TESTING.md`

**Estimate:** 0.5 day

---

### Phase D: Cleanup

#### Issue 7: Cleanup and Documentation

**Title:** `WebRTC Audio: Final cleanup and documentation`  
**Labels:** `area:unreal`, `area:webrtc`, `documentation`, `cleanup`  
**Priority:** Low  
**Dependencies:** All previous issues

**Changes:**
- Remove dead code and commented sections
- Update documentation:
  - Mark WEBRTC_AUDIO_REFACTOR.md as complete
  - Update WEBRTC_QUICKSTART.md with audio setup
  - Update BROADCAST_WEBRTC_USER_GUIDE.md
  - Create WEBRTC_AUDIO_MIGRATION.md
  - Add API doc comments
- Update CHANGELOG.md
- Update README.md features section

**Estimate:** 0.5 day

---

## Implementation Strategy

### Rollout Plan

1. **PR 1:** Issue #1 (Adapter fix) - Quick win, immediate improvement
2. **PR 2:** Issues #2 + #3 (Component refactor + Centralize setup) - Core changes
3. **PR 3:** Issue #4 (Enforce ordering) - Contract hardening
4. **PR 4:** Issues #5 + #6 + #7 (Diagnostics + Tests + Cleanup) - Polish

Each PR should compile and be testable independently.

### Success Criteria

After all issues are complete:
- ✅ Audio track consistently present in SDP when enabled
- ✅ `EnableAudioSend` after Start fails visibly
- ✅ AudioCaptureComponent has no WebRTC negotiation code
- ✅ Setup sequence is deterministic and logged
- ✅ Comprehensive status command available
- ✅ Test matrix passes
- ✅ Documentation complete and accurate

---

## Key Design Decisions

### 1. Why Audio Track Before DataChannel?

From the working libdatachannel example, adding the audio track before creating the DataChannel ensures it's included in the initial offer SDP. If added after, the audio track won't be negotiated unless renegotiation occurs.

### 2. Why Remove Connector from AudioCaptureComponent?

The component was participating in timing-sensitive negotiation, creating race conditions. Making it a pure PCM source with callback pattern eliminates timing dependencies and clarifies ownership.

### 3. Why Compute StreamLabel in BroadcastComponent?

StreamLabel needs to be determined once and consistently used across setup, capture, and transmission. BroadcastComponent has the context (SubjectName, audio mode, device name) to compute it correctly.

### 4. Why Enforce EnableAudioSend Before Start?

Once the PeerConnection exists, you can't retroactively add media to the initial offer without renegotiation. Enforcing this ordering prevents a common mistake and makes the API contract clear.

---

## References

- **Refactor Plan:** `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md`
- **Working Example:** https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp
- **Current Implementation:**
  - Adapter: `Source/Open3DStream/Private/WebRTCConnectorFactory.cpp`
  - Audio Capture: `Source/Open3DStream/Private/O3DSBroadcastAudioCaptureComponent.cpp`
  - Connector: `Source/Open3DStream/Private/WebRTCConnector.cpp`
  - Broadcast: `Source/Open3DBroadcast/Private/O3DSBroadcastComponent.cpp`

---

## Next Steps

1. **Review this summary** with the team
2. **Create the Epic issue** in GitHub
3. **Create sub-issues #1-7** using the detailed descriptions in `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md`
4. **Link all sub-issues** to the Epic
5. **Assign Phase A issues** (#1-2) to start implementation
6. **Set up project board** to track progress

---

**Note:** The full, detailed issue descriptions with complete code examples, acceptance criteria, and testing instructions are available in `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md`. Copy issue content from there when creating GitHub issues.
