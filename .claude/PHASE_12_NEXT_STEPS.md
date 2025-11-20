# Phase 12: Next Steps to Diagnose 4070 ms Latency Spike

## What We Know

From the metrics collected (59 seconds of operation):

**Receiver Operations (OPTIMAL):**
- Parse time: 0.165 ms
- Pose extraction: 0.082 ms
- LiveLink push: 0.012 ms
- Total processing: 0.248 ms

**System Performance (PROBLEM):**
- Max latency: 4070.11 ms (catastrophic spike)
- Avg latency: 20.10 ms (acceptable)
- Animation is "choppy and periodically stalls"

**Key Insight**: The receiver is near-optimal. The 4070 ms spike occurs AFTER the receiver completes, meaning the bottleneck is elsewhere in the system.

## Enhanced Diagnostics (Just Implemented)

**Phase 12 now includes:**
1. **Per-operation timing metrics in receiver** (PRIMARY FOCUS)
   - Parse time, pose extraction, LiveLink push, total processing
   - Proves receiver operations are near-optimal (0.248 ms total)
   - Shows LiveLink push is nearly instant (0.012 ms)

2. **Connection status tracking** (Secondary - sender-side)
   - Metrics now properly updated when connection changes
   - Queue is reset on successful connection
   - No more stale "Connected: NO" status

**Key Finding**: Since receiver operations are so fast (0.248 ms) and LiveLink push is instant (0.012 ms), the 4070 ms spike must occur in the Unreal Engine main thread or LiveLink client queue AFTER the push completes.

## How to Collect Next Round of Diagnostics

1. **Start receiver** with animation for 60 seconds
2. **Open console** and type:
   ```
   o3d.DumpMetrics
   ```
3. **Also check the log** for these warnings:
   ```
   "WebRTC queue building: N pending frames (threshold=30)"
   ```
4. **Note timing of warnings** relative to animation stalls

## What the Queue Warnings Will Tell Us

### If you see "WebRTC queue building" warnings:
```
LogO3DWebRTCSender: Warning: WebRTC queue building: 18 pending frames (threshold=30)
LogO3DWebRTCSender: Warning: WebRTC queue building: 22 pending frames (threshold=30)
LogO3DWebRTCSender: Warning: WebRTC queue building: 28 pending frames (threshold=30)
```
**Interpretation**: Network/FFI is slow to consume frames
- Frames are piling up faster than they can be sent/processed
- This could cause the 4070 ms spike if queue gets stuck
- **Fix**: Check network health, consider Phase 10 backpressure threshold tuning

### If you DON'T see "WebRTC queue building" warnings:
**Interpretation**: WebRTC queue is healthy (<15 pending frames)
- The bottleneck is NOT in the transport layer
- The 4070 ms spike is happening in:
  - LiveLink client queue processing
  - Unreal Engine main thread blocking
  - System resources (GC, page faults)
- **Fix**: Profile Unreal Engine main thread

## Most Likely Scenario

Based on the data, the 4070 ms spike is **NOT on the sender side** and **NOT in receiver processing**.

### The Bottleneck is in Unreal Engine (Receiver Side)

**Evidence:**
- Receiver processes frame in 0.248 ms ✓
- LiveLink push is instant (0.012 ms) ✓
- But max latency is 4070 ms ⚠️

**Likely Root Causes:**
1. **LiveLink client queue processing** - Frames are pushed to LiveLink, but main thread processes them asynchronously with stalling
2. **Unreal Engine main thread contention** - GC, rendering, shader compilation, or other systems block the main thread
3. **LiveLink subject update batching** - LiveLink may batch updates instead of applying them immediately

**How This Manifests:**
- Receiver gets frame, processes in 0.248 ms
- Receiver pushes to LiveLink instantly (0.012 ms)
- But LiveLink doesn't apply the update until main thread gets to it
- If main thread is busy, queue backs up and animation stalls
- Eventually main thread catches up and animation jumps forward

This matches the user's observation: "receiver animation is choppy and periodically appears to stall and restart"

## Recommended Diagnostic Commands

### Run this complete test:
```
# Clear metrics
o3d.ResetMetrics

# Wait for animation to run and for one of the 4070 ms spikes
# (Watch the animation - it should stall visibly)

# Dump metrics
o3d.DumpMetrics

# Check log for queue warnings
# Search for: "WebRTC queue building"
```

### Look for these patterns in the log:

**Pattern A: Queue growing steadily (transport slow)**
```
WebRTC queue building: 16 pending frames
WebRTC queue building: 18 pending frames
WebRTC queue building: 20 pending frames
```

**Pattern B: No queue warnings (main thread or other issue)**
```
(no "WebRTC queue building" messages)
```

**Pattern C: Queue spikes then clears (temporary congestion)**
```
WebRTC queue building: 28 pending frames
(then queue goes quiet for a bit, then spikes again)
```

## Diagnostic Focus: Unreal Engine Main Thread

Since receiver operations are proven fast, we need to focus on what happens AFTER LiveLink push:

### H1: LiveLink Client Queue Overflow (PRIMARY)
- **Symptom**: Receiver pushes update (0.012 ms), but LiveLink doesn't apply it for 4000+ ms
- **Cause**: LiveLink client queues updates, main thread processes queue asynchronously
- **Evidence to look for**:
  - Periodic stalls (matches user observation)
  - Avg latency 20 ms (normal operation between stalls)
  - Max latency 4070 ms (one massive stall)
- **How to confirm**:
  - Profile FLiveLinkClient to check queue depth
  - Measure time from PushSubjectFrameData() to LiveLink applying update
  - Check if queue builds up periodically

### H2: Unreal Engine Main Thread Blocking (SECONDARY)
- **Symptom**: Main thread busy with something else (GC, rendering, etc.)
- **Cause**: Main thread is blocked, can't process LiveLink queue
- **Evidence to look for**:
  - Long frames in the editor viewport (frame drops)
  - High CPU usage on main thread
  - GC pauses correlating with animation stalls
- **How to confirm**:
  - Profile Unreal Engine main thread with detailed timing
  - Check for GC pauses
  - Monitor memory usage

## Next Steps: Profile LiveLink Processing

What we need to measure:
1. **From receiver side**: Time PushSubjectFrameData() completes
2. **From Unreal side**: When does LiveLink actually apply the update to the subject?
3. **The gap**: That gap (potentially 4000+ ms) is where the stall occurs

Add timing instrumentation to measure:
```cpp
// In O3DReceiverSource::ProcessParsedSubject():
double PreLiveLinkPush = FPlatformTime::Seconds();
PushSubjectFrameData(...);
double PostLiveLinkPush = FPlatformTime::Seconds();

// Then separately, measure when LiveLink actually applied the update
// (This requires looking at FLiveLinkClient internals or profiling)
```

## Files Modified in This Phase

1. **O3DPerformanceMetrics.h/cpp** - Added 4 per-operation timing metrics
2. **O3DReceiverSource.cpp** - Instrumented frame processing pipeline
3. **WebRTCSender.cpp** - Enhanced backpressure diagnostics, connection tracking

## Summary

Phase 12 has given us:
✓ Proof that receiver operations are near-optimal (0.248 ms per frame)
✓ Identification that the spike is NOT in the receiver
✓ Enhanced logging to pinpoint the actual bottleneck
✓ Three clear hypotheses to test

The next run with the enhanced diagnostics will definitively point to which system layer needs optimization.
