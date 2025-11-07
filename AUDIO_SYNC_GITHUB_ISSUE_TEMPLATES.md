# GitHub Issue Templates for Audio-Synchronized Mocap Playback

This document contains ready-to-use GitHub issue templates for all phases of the Audio-Synchronized Mocap Playback implementation.

**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md`

---

## EPIC ISSUE

### Title
Epic: Audio-Synchronized Mocap Playback over WebRTC

### Labels
epic, area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Epic: Audio-Synchronized Mocap Playback over WebRTC

**Reference Document**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md`  
**Development Plan**: `AUDIO_SYNC_DEVELOPMENT_PLAN.md`

## Executive Summary

### Problem Statement
The WebRTC LiveKit receiver experiences jerky, inconsistent animation playback when subscribers connect:
- First few seconds: slow and jerky animation
- Next few seconds: speeds up/catches up
- Eventually settles but never fully stable
- Issue specific to WebRTC path; TCP/UDP/NNG transports work correctly

### Root Causes Identified

1. **Aggressive Frame Coalescing**: WebRTC receiver drops intermediate frames
2. **Strict Out-of-Order Rejection**: Unordered delivery causes frame drops
3. **No Jitter Buffer**: Immediate frame application without smoothing
4. **No Audio Synchronization**: Mocap frames play independently of audio

### Solution Overview
Implement an **audio-driven jitter buffer** that:
- Uses sequence numbers for frame ordering
- Uses timestamps for audio/mocap synchronization
- Provides user-tunable buffer delay (0-2 seconds)
- Ensures smooth, consistent animation with lip-sync

## Implementation Overview

**Total Estimated Effort**: ~8.5 hours across 7 phases with 13 tasks

## Success Criteria

### Primary Goals
✅ Animation smooth from first frame  
✅ Audio/mocap synchronized (0-500ms configurable)  
✅ No frame drops or memory leaks  
✅ User can tune buffer delay

### Secondary Goals
✅ Console commands for debugging  
✅ Comprehensive documentation  
✅ Unit tests for core logic  
✅ Backward compatible with existing transports

## Task Breakdown

### Phase 1: Audio Clock Infrastructure
- [ ] #TBD Add Global Audio Playback Time Tracking

### Phase 2: Jitter Buffer Data Structures
- [ ] #TBD Add Timestamped Frame Buffer to LiveLink Source
- [ ] #TBD Implement Frame Buffering Logic

### Phase 3: Frame Pipeline Integration
- [ ] #TBD Refactor OnPackage to Use Buffering
- [ ] #TBD Update Tick to Process Buffer

### Phase 4: User Configuration & Settings
- [ ] #TBD Add Audio Sync Settings
- [ ] #TBD Wire Settings into Constructor

### Phase 5: Console Commands & Debugging
- [ ] #TBD Add Console Variables
- [ ] #TBD Add Buffer Dump Console Command

### Phase 6: Remove Aggressive Coalescing
- [ ] #TBD Remove Frame Coalescing from WebRTC Receiver

### Phase 7: Documentation & Testing
- [ ] #TBD Create Implementation Documentation
- [ ] #TBD Update Testing Guide
- [ ] #TBD Add Unit Tests

## Related Issues
- #149: Protocol hardening with tx_seq and tx_wallclock_us
- #118-#125: WebRTC Audio refactor
- #88, #90: LiveKit backend implementation
```

---

## PHASE 1

### Title
Phase 1: Add Global Audio Playback Time Tracking

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 1: Audio Clock Infrastructure

**Part of Epic**: #TBD (Audio-Synchronized Mocap Playback)  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 1)  
**Effort**: 30 minutes

## Objective
Add global audio playback time tracking to `FO3DSAudioBus` for audio/mocap synchronization.

## Problem
Mocap frames play immediately without audio synchronization, causing:
- Animation starting before audio
- Lip sync drift
- Inconsistent playback speed

## Solution
Implement thread-safe audio clock that:
- Tracks current audio playout position
- Updates from audio metadata
- Provides lock-protected game thread access

## Implementation

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Public/O3DSAudioBus.h`

Add methods:
- `static void UpdateAudioPlayoutTime(double TimeSeconds)`
- `static double GetAudioPlayoutTime()`
- Integrate into `PublishPcm16()`

## Acceptance Criteria
- [x] Methods exist and use FCriticalSection
- [x] PublishPcm16 calls UpdateAudioPlayoutTime
- [x] Console command to query audio time works
- [x] Audio time increases monotonically
- [x] No threading issues

## Testing
- Create console command to print audio time
- Verify monotonic increase during playback
- Verify thread safety under load

See reference document for complete code snippets.
```

---

## PHASE 2.1

### Title
Phase 2.1: Add Timestamped Frame Buffer to LiveLink Source

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 2.1: Add Timestamped Frame Buffer Data Structure

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 2, Task 2.1)  
**Effort**: 30 minutes  
**Dependencies**: Phase 1

## Objective
Add data structures to buffer mocap frames with sequence numbers and timestamps.

## Problem
Frames processed immediately on arrival causes:
- Out-of-order rejection
- No jitter smoothing
- No audio synchronization

## Solution
Add timestamped frame buffer with:
- Sequence number for ordering
- Timestamp for audio sync
- Configurable size limits

## Implementation

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Public/Open3DStreamSource.h`

Add:
- `struct FTimestampedFrame`
- `TArray<FTimestampedFrame> FrameBuffer`
- `void BufferMocapFrame()` (declaration)
- `void ProcessBufferedFrames()` (declaration)

## Acceptance Criteria
- [x] FTimestampedFrame struct defined
- [x] FrameBuffer member added
- [x] Methods declared
- [x] Code compiles
- [x] No impact on existing functionality

See reference document for complete struct definition.
```

---

## PHASE 2.2

### Title
Phase 2.2: Implement Frame Buffering Logic

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 2.2: Implement Frame Buffering and Processing Logic

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 2, Task 2.2)  
**Effort**: 1 hour  
**Dependencies**: Phase 2.1

## Objective
Implement core buffering and audio-synchronized playback logic.

## Solution
- Buffer frames in sequence order
- Drop duplicates/out-of-order frames
- Schedule playback based on audio clock
- Prevent unbounded memory growth

## Implementation

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

Implement:
- `BufferMocapFrame()` - add, sort, cap buffer
- `ProcessBufferedFrames()` - audio-synchronized playback

## Acceptance Criteria
- [x] Frames sorted by sequence
- [x] Duplicates dropped
- [x] Buffer capped at MaxBufferFrames
- [x] Frames processed when timestamp <= target
- [x] Debug logging works
- [x] No memory leaks

## Testing
- Test ordering: inject [100,102,101] → sorted
- Test duplicates: inject [100,100] → second dropped
- Test buffer cap: 15 frames, max=10 → 5 dropped

See reference for complete implementation.
```

---

## PHASE 3.1

### Title
Phase 3.1: Refactor OnPackage to Use Buffering

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 3.1: Refactor OnPackage to Use Buffering

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 3, Task 3.1)  
**Effort**: 45 minutes  
**Dependencies**: Phase 2.2

## Objective
Route incoming frames to buffer instead of immediate application.

## Implementation

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

Changes:
- Modify `OnPackage()` to call `BufferMocapFrame()`
- Create `OnPackageInternal()` with existing frame logic
- Extract sequence and timestamp metadata

## Acceptance Criteria
- [x] OnPackage buffers instead of immediate apply
- [x] OnPackageInternal created with existing logic
- [x] Metadata extracted correctly
- [x] Game thread marshaling still works

See reference for complete refactoring details.
```

---

## PHASE 3.2

### Title  
Phase 3.2: Update Tick to Process Buffer

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 3.2: Update Tick to Process Buffered Frames

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 3, Task 3.2)  
**Effort**: 15 minutes  
**Dependencies**: Phase 3.1

## Objective
Process buffered frames based on audio clock in Tick().

## Implementation

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

Add to `Tick()`:
- Call `ProcessBufferedFrames()` after WebRTC receiver tick

## Acceptance Criteria
- [x] ProcessBufferedFrames called in Tick
- [x] Frames play out smoothly
- [x] Timing aligns with audio
- [x] No performance regression

## Testing
- Verify smooth playback
- Check timing with console debug
- Test with/without audio

See reference for implementation details.
```

---

## PHASE 4.1

### Title
Phase 4.1: Add Audio Sync Settings to Source Settings

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 4.1: Add Audio Sync Settings

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 4, Task 4.1)  
**Effort**: 20 minutes  
**Dependencies**: Phase 3

## Objective
Add user-configurable buffer settings to FOpen3DStreamSettings.

## Implementation

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Public/Open3DStreamSourceSettings.h`

Add properties:
- `AudioSyncBufferSeconds` (0-2s, default 0.1)
- `bEnableAudioSync` (default true)
- `MaxBufferedFrames` (3-30, default 10)

## Acceptance Criteria
- [x] Settings visible in UI
- [x] Tooltips descriptive
- [x] Settings saved/loaded correctly
- [x] Validation ranges enforced

See reference for UPROPERTY definitions.
```

---

## PHASE 4.2

### Title
Phase 4.2: Wire Settings into Source Constructor

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 4.2: Wire Settings into Constructor

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 4, Task 4.2)  
**Effort**: 15 minutes  
**Dependencies**: Phase 4.1

## Objective
Apply user settings to buffer configuration.

## Implementation

**Files**:
- `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`
- `plugins/unreal/Open3DStream/Source/Open3DStream/Public/Open3DStreamSource.h`

Changes:
- Read settings in constructor
- Apply to buffer variables
- Store bAudioSyncEnabled flag

## Acceptance Criteria
- [x] Settings read in constructor
- [x] Buffer configured correctly
- [x] Behavior changes with settings
- [x] Settings persist across sessions

See reference for constructor modifications.
```

---

## PHASE 5.1

### Title
Phase 5.1: Add Console Variables for Runtime Control

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 5.1: Add Console Variables

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 5, Task 5.1)  
**Effort**: 30 minutes  
**Dependencies**: Phase 4

## Objective
Add runtime control of audio sync via console variables.

## Implementation

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

Add CVars:
- `o3ds.Receiver.AudioSync` (0/1)
- `o3ds.Receiver.BufferDelay` (0.0-2.0)

Modify ProcessBufferedFrames to:
- Read CVars at runtime
- Support immediate mode when disabled

## Acceptance Criteria
- [x] CVars exist and work
- [x] Runtime control functional
- [x] Immediate mode works
- [x] CVar changes applied immediately

See reference for CVar definitions.
```

---

## PHASE 5.2

### Title
Phase 5.2: Add Buffer Dump Console Command

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 5.2: Add Buffer Dump Console Command

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 5, Task 5.2)  
**Effort**: 45 minutes  
**Dependencies**: Phase 5.1

## Objective
Add diagnostic console command for troubleshooting.

## Implementation

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

Add:
- Global registry of active sources
- Register/unregister in constructor/destructor
- `o3ds.Receiver.DumpBuffer` console command

## Acceptance Criteria
- [x] Command exists
- [x] Shows buffer state for all sources
- [x] Displays sequences, timestamps, ages
- [x] Output is useful for debugging

## Testing
- Run command with 0 sources → shows "No active sources"
- Run with 1 source, 3 frames → shows all 3
- Verify buffer age calculation

See reference for command implementation.
```

---

## PHASE 6

### Title
Phase 6: Remove Aggressive Frame Coalescing from WebRTC Receiver

### Labels
area:unreal, area:webrtc, enhancement, audio

### Body
```markdown
# Phase 6: Remove Aggressive Frame Coalescing

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 6, Task 6.1)  
**Effort**: 30 minutes  
**Dependencies**: Phase 5

## Objective
Remove aggressive frame coalescing; let jitter buffer handle ordering.

## Implementation

**Files**:
- `plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTC/Open3DSWebRtcReceiver.cpp`
- `plugins/unreal/Open3DStream/Source/Open3DStream/Public/WebRTC/Open3DSWebRtcReceiver.h`

Changes:
- Remove coalescing from OnConnectorData
- Pass all frames to jitter buffer
- Remove CoalesceMutex, PendingData, bDataDispatchScheduled

## Acceptance Criteria
- [x] All frames passed through
- [x] No coalescing logic remains
- [x] Unused members removed
- [x] Buffer handles all frames correctly

## Testing
- Verify no frames dropped unnecessarily
- Monitor buffer with dump command
- Check buffer size stays healthy

See reference for implementation details.
```

---

## PHASE 7.1

### Title
Phase 7.1: Create Audio Sync Implementation Documentation

### Labels
documentation, area:unreal, area:webrtc, audio

### Body
```markdown
# Phase 7.1: Create Implementation Documentation

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 7, Task 7.1)  
**Effort**: 1 hour  
**Dependencies**: Phase 6

## Objective
Create comprehensive documentation for audio sync implementation.

## Implementation

**File**: Create `plugins/unreal/Open3DStream/docs/AUDIO_SYNC_IMPLEMENTATION.md`

Sections:
- Overview and architecture
- Component descriptions
- Frame flow diagram
- Configuration reference
- Console commands
- Troubleshooting guide

## Acceptance Criteria
- [x] Documentation exists and is complete
- [x] Architecture clearly explained
- [x] All settings documented
- [x] Troubleshooting guide helpful
- [x] Examples provided

See reference for content template.
```

---

## PHASE 7.2

### Title
Phase 7.2: Update WebRTC Testing Guide

### Labels
documentation, area:unreal, area:webrtc, audio

### Body
```markdown
# Phase 7.2: Update Testing Guide

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 7, Task 7.2)  
**Effort**: 45 minutes  
**Dependencies**: Phase 7.1

## Objective
Update testing guide with audio sync test procedures.

## Implementation

**File**: `plugins/unreal/Open3DStream/docs/WEBRTC_TESTING_GUIDE.md`

Add section:
- Audio-synchronized playback tests
- Test cases: smooth startup, lip sync, reconnect, tuning
- Metrics section with buffer dump examples

## Acceptance Criteria
- [x] Testing guide updated
- [x] Test cases clear and actionable
- [x] Expected results documented
- [x] Metrics interpretation explained

See reference for test case templates.
```

---

## PHASE 7.3

### Title
Phase 7.3: Add Comprehensive Unit Tests

### Labels
area:unreal, area:webrtc, audio, testing

### Body
```markdown
# Phase 7.3: Add Unit Tests

**Part of Epic**: #TBD  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (Phase 7, Task 7.3)  
**Effort**: 2 hours  
**Dependencies**: Phase 7.2

## Objective
Add comprehensive unit tests for audio sync functionality.

## Implementation

**File**: Create `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Tests/AudioSyncBufferTests.cpp`

Tests:
- Audio clock updates
- Frame ordering and buffering
- Duplicate detection
- Buffer cap enforcement
- Audio synchronization

## Acceptance Criteria
- [x] All tests pass
- [x] Good code coverage
- [x] Edge cases tested
- [x] Tests run in CI

See reference for test implementations.
```

---

# Usage Instructions

1. Copy the body of the Epic Issue and create it first
2. Note the epic issue number
3. For each phase, copy the template body
4. Replace `#TBD` with the actual epic issue number
5. Create the issues in order (dependencies matter)
6. Link each issue to the epic

**Reference Documents**:
- `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` - Complete technical specification
- `AUDIO_SYNC_DEVELOPMENT_PLAN.md` - Development roadmap
