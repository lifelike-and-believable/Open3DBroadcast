# Audio-Synchronized Mocap Playback over WebRTC - Development Plan

## Overview

This document provides an actionable development plan for implementing Audio-Synchronized Mocap Playback over WebRTC based on the comprehensive technical specification in `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md`.

## Executive Summary

### Problem Statement

The WebRTC LiveKit receiver experiences jerky, inconsistent animation playback when subscribers connect:
- First few seconds: slow and jerky animation
- Next few seconds: speeds up/catches up
- Eventually settles but never fully stable
- Issue specific to WebRTC path; TCP/UDP/NNG transports work correctly

### Root Causes

1. **Aggressive Frame Coalescing**: WebRTC receiver drops intermediate frames
2. **Strict Out-of-Order Rejection**: Unordered delivery causes valid frames to be dropped
3. **No Jitter Buffer**: Immediate frame application without smoothing
4. **No Audio Synchronization**: Mocap frames play independently of audio

### Solution

Implement an **audio-driven jitter buffer** that:
- Uses sequence numbers for frame ordering
- Uses timestamps for audio/mocap synchronization  
- Provides user-tunable buffer delay (0-2 seconds)
- Ensures smooth, consistent animation with lip-sync

## Implementation Plan

### Total Estimated Effort: ~8.5 hours

The implementation is divided into 7 phases with 13 tasks:

| Phase | Tasks | Effort | Description |
|-------|-------|--------|-------------|
| 1 | 1 | 30 min | Audio Clock Infrastructure |
| 2 | 2 | 1.5 hrs | Jitter Buffer Data Structures |
| 3 | 2 | 1 hr | Frame Pipeline Integration |
| 4 | 2 | 35 min | User Configuration & Settings |
| 5 | 2 | 1.25 hrs | Console Commands & Debugging |
| 6 | 1 | 30 min | Remove Aggressive Coalescing |
| 7 | 3 | 3.75 hrs | Documentation & Testing |

## GitHub Issues Structure

### Epic Issue
**Title**: Epic: Audio-Synchronized Mocap Playback over WebRTC

**Purpose**: Track overall implementation progress, link all phase issues

**Labels**: epic, area:unreal, area:webrtc, enhancement, audio

### Phase 1: Audio Clock Infrastructure

#### Issue 1: Add Global Audio Playback Time Tracking
- **Effort**: 30 minutes
- **Files**: `O3DSAudioBus.h`
- **Goal**: Thread-safe audio clock for synchronization
- **Dependencies**: None

### Phase 2: Jitter Buffer Data Structures

#### Issue 2: Add Timestamped Frame Buffer to LiveLink Source
- **Effort**: 30 minutes
- **Files**: `Open3DStreamSource.h`
- **Goal**: Data structures for buffering frames
- **Dependencies**: Phase 1

#### Issue 3: Implement Frame Buffering Logic
- **Effort**: 1 hour
- **Files**: `Open3DStreamSource.cpp`
- **Goal**: Core buffering and audio-sync algorithms
- **Dependencies**: Issue 2

### Phase 3: Frame Pipeline Integration

#### Issue 4: Refactor OnPackage to Use Buffering
- **Effort**: 45 minutes
- **Files**: `Open3DStreamSource.cpp`
- **Goal**: Route incoming frames to buffer
- **Dependencies**: Issue 3

#### Issue 5: Update Tick to Process Buffer
- **Effort**: 15 minutes
- **Files**: `Open3DStreamSource.cpp`
- **Goal**: Process buffered frames on game thread
- **Dependencies**: Issue 4

### Phase 4: User Configuration

#### Issue 6: Add Audio Sync Settings
- **Effort**: 20 minutes
- **Files**: `Open3DStreamSourceSettings.h`
- **Goal**: User-configurable buffer parameters
- **Dependencies**: Phase 3

#### Issue 7: Wire Settings into Constructor
- **Effort**: 15 minutes
- **Files**: `Open3DStreamSource.cpp`, `Open3DStreamSource.h`
- **Goal**: Apply user settings to buffer
- **Dependencies**: Issue 6

### Phase 5: Console Commands & Debugging

#### Issue 8: Add Console Variables
- **Effort**: 30 minutes
- **Files**: `Open3DStreamSource.cpp`
- **Goal**: Runtime control of audio sync
- **Dependencies**: Phase 4

#### Issue 9: Add Buffer Dump Console Command
- **Effort**: 45 minutes
- **Files**: `Open3DStreamSource.cpp`
- **Goal**: Diagnostic command for troubleshooting
- **Dependencies**: Issue 8

### Phase 6: Remove Aggressive Coalescing

#### Issue 10: Remove Frame Coalescing from WebRTC Receiver
- **Effort**: 30 minutes
- **Files**: `Open3DSWebRtcReceiver.cpp`, `Open3DSWebRtcReceiver.h`
- **Goal**: Let jitter buffer handle frame ordering
- **Dependencies**: Phase 5

### Phase 7: Documentation & Testing

#### Issue 11: Create Implementation Documentation
- **Effort**: 1 hour
- **Files**: Create `AUDIO_SYNC_IMPLEMENTATION.md`
- **Goal**: Architecture and usage documentation
- **Dependencies**: Phase 6

#### Issue 12: Update Testing Guide
- **Effort**: 45 minutes
- **Files**: Update `WEBRTC_TESTING_GUIDE.md`
- **Goal**: Test procedures and validation
- **Dependencies**: Issue 11

#### Issue 13: Add Unit Tests
- **Effort**: 2 hours
- **Files**: Create `AudioSyncBufferTests.cpp`
- **Goal**: Comprehensive test coverage
- **Dependencies**: Issue 12

## Success Criteria

### Primary Goals
- ✅ Animation smooth from first frame (no startup jerkiness)
- ✅ Audio/mocap synchronized within user tolerance (0-500ms configurable)
- ✅ No frame drops or memory leaks
- ✅ User can tune buffer delay

### Secondary Goals
- ✅ Console commands for runtime debugging
- ✅ Comprehensive documentation
- ✅ Unit tests for core logic
- ✅ Backward compatible with existing transports

## Testing Requirements

### Functional Tests
- Animation smooth on initial connect
- Audio/mocap lip sync within 100ms
- Buffer grows/shrinks appropriately
- No memory leaks (buffer cap works)
- Console commands work correctly
- Settings persist and apply
- Immediate mode works (AudioSync=0)

### Regression Tests
- TCP/UDP/NNG transports still work
- Non-audio mocap still works
- Multiple LiveLink sources work
- PIE start/stop cycles stable
- Broadcaster restart recovers

### Performance Tests
- No frame drops in buffer
- CPU usage <5% for buffering logic
- Memory stable over 10min session
- 60fps animation maintained

## Risk Mitigation

| Risk | Mitigation |
|------|-----------|
| Audio clock not available | Fallback to immediate mode if GetAudioPlayoutTime() == 0.0 |
| Excessive buffer growth | Hard cap at MaxBufferedFrames with logging |
| Timestamp discontinuities | Detect large jumps (>1s) and reset buffer |
| Performance impact | Profile ProcessBufferedFrames; optimize if >2ms/frame |

## Related Work

### Related Issues
- Issue #149: Protocol hardening with tx_seq and tx_wallclock_us
- Issues #118-#125: WebRTC Audio refactor
- Issues #88, #90: LiveKit backend implementation

### Feature Branch
The `feature/tx-seq-wallclock` branch adds production-quality sequencing:
- `tx_seq`: Monotonic sequence number
- `tx_wallclock_us`: Wall-clock timestamp

**Recommendation**: Consider merging for improved ordering.

## Reference Documents

1. **Technical Specification**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md`
   - Complete implementation details
   - Code snippets for all changes
   - Testing procedures
   
2. **Architecture Guide**: `.github/copilot-instructions.md`
   - Repository standards
   - Unreal Engine practices
   - Build and test procedures

3. **WebRTC Testing**: `plugins/unreal/Open3DStream/docs/WEBRTC_TESTING_GUIDE.md`
   - Current testing procedures
   - Will be updated in Phase 7

## Implementation Notes

### Thread Safety
- `FO3DSAudioBus::UpdateAudioPlayoutTime()` uses `FCriticalSection`
- `BufferMocapFrame()` called only on game thread (after marshal)
- `ProcessBufferedFrames()` called from `Tick()` (game thread)

### Performance Considerations
- Frame buffer sort is O(n log n) but n typically <10
- Consider TCircularBuffer if buffer churn becomes issue
- Audio clock read is lock-protected but fast (<1µs)

### Backward Compatibility
- New settings default to enabled (opt-out)
- CVars allow runtime disable for testing
- Existing transports unaffected (no code changes)

## Next Steps

1. **Create GitHub Issues**: Use provided templates to create all 14 issues (1 epic + 13 tasks)
2. **Review & Prioritize**: Team review of plan and timeline
3. **Assign Tasks**: Distribute work across team members
4. **Begin Implementation**: Start with Phase 1 (foundational work)
5. **Iterate & Test**: Complete each phase with testing before moving to next

## Issue Creation Command

To create all issues programmatically:

```bash
cd /home/runner/work/Open3DStream/Open3DStream
python /tmp/create_audio_sync_issues.py
```

Or use the GitHub CLI manually following the templates in `/tmp/AUDIO_SYNC_GITHUB_ISSUES_PLAN.md`.

---

**Document Status**: Ready for Implementation  
**Last Updated**: 2025-11-07  
**Author**: Planning Agent  
**Reference**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md`
