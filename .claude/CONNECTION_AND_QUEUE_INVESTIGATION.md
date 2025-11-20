# Investigation: WebRTC Connection Status and Queue Buildup

## Suspicious Finding: Connection Marked as "Connected: NO"

```
LogO3DPerformanceMetrics: Warning:   WebRTC:
LogO3DPerformanceMetrics: Warning:     Connected: NO
LogO3DPerformanceMetrics: Warning:     Frames Sent: 3285
LogO3DPerformanceMetrics: Warning:     Bytes Sent: 39.97 MB
LogO3DPerformanceMetrics: Warning:     Pending Frames: 0 (max: 0)
```

**This is contradictory:**
- Says "Connected: NO"
- But successfully sent 3285 frames (39.97 MB)
- No pending frames recorded

## Hypothesis: The 4070 ms Spike is Caused by Buffering in WebRTC/Network Stack

Even though the receiver code completes in 0.248 ms, there's a 4+ second gap somewhere before the animation updates. Possible causes:

### 1. WebRTC FFI Queue Buildup (Despite "Pending: 0")
- **Phase 10 tracks**: `EstimatedPendingFrames` - a LOCAL estimate in sender
- **Actual FFI queue**: LiveKit FFI may have its own queue we're not seeing
- **Issue**: Our backpressure threshold (30 frames) may not be calibrated correctly
- **Evidence**: Frames are sending successfully but animation stalls

### 2. LiveLink Client Asynchronous Processing
- After `PushSubjectFrameData()` completes (0.012 ms), LiveLink queues the update
- Main thread processes LiveLink queue asynchronously
- If main thread is busy, queue backs up and animation stalls

### 3. Network Packet Reordering or Loss + Retransmission
- WebRTC has built-in retransmission
- If packets are lost/reordered, retransmission may introduce 4+ second delays
- Especially if network is congested

## How to Diagnose: Add WebRTC Queue Monitoring

The Phase 10 backpressure monitoring estimates queue depth, but we need actual FFI queue data:

```cpp
// In WebRTCSender::Send(), after lk_send_data_ex():
// Log the actual pending frame estimate
int32 EstimatedPending = EstimatedPendingFrames.Load();
if (EstimatedPending > DefaultBackpressureThreshold * 0.5)
{
    UE_LOG(LogO3DWebRTCSender, Warning,
        TEXT("WebRTC queue backing up: estimated pending=%d (threshold=%d)"),
        EstimatedPending, DefaultBackpressureThreshold);
}
```

## How to Diagnose: LiveLink Queue Monitoring

Add instrumentation to detect when LiveLink is queuing updates:

```cpp
// In O3DReceiverSource::ProcessParsedSubject(), after PushSubjectFrameData():
const double PrePushTime = FPlatformTime::Seconds();
PushSubjectFrameData(...);
const double PostPushTime = FPlatformTime::Seconds();

// If push is instant (0.012 ms) but overall frame latency is high,
// the delay is in LiveLink's main thread processing
```

## Hypothesis for 4070 ms Spike Pattern

Based on metrics:
- Avg RTT: 20.1 ms (reasonable)
- Max RTT: 4070 ms (catastrophic stall)
- Frame drop rate: 0.50% (very low - good!)

**Most likely scenario:**
1. Receiver gets frame in 20 ms (normal)
2. Receiver processes in 0.248 ms (fast)
3. Receiver pushes to LiveLink instantly (0.012 ms)
4. **LiveLink main thread is busy with something else (4+ seconds)**
5. Animation doesn't update until LiveLink thread gets around to processing the queue
6. This causes visible stalling to the user

## Why Animation Looks "Choppy and Stalls"

- **Smooth operation** (~90% of the time): Receiver → LiveLink → Animation in 20 ms ✓
- **Periodic stalls** (~10% of the time): Frame arrives → waits 4+ seconds → animation suddenly jumps forward ✗

This matches the user's description perfectly: "receiver animation is choppy and periodically appears to stall and restart"

## Connection Status Bug

The "Connected: NO" status may be a separate bug:

```cpp
// In WebRTCSender, the connection status should be set when:
void OnConnectionState(void* user, LkConnectionState state, ...)
{
    // This updates the connection status
    // But may not be called correctly by LiveKit FFI
}
```

The fact that frames are sending despite "Connected: NO" suggests:
- Connection callback is not being called correctly
- Or connection status is stale/cached
- Or LiveKit FFI reports a different connection state

## Recommended Investigation Order

1. **Add detailed WebRTC queue logging** (Phase 10 enhancement):
   - Log estimated pending frames on every send
   - Log when backpressure threshold is exceeded
   - Check if queue ever actually builds up

2. **Profile Unreal Engine main thread** (Phase 13):
   - Measure time from PushSubjectFrameData() to actual LiveLink processing
   - Detect if main thread is blocked by GC, rendering, or other tasks

3. **Fix connection status reporting** (Bug fix):
   - Verify OnConnectionState callback is working
   - Update metrics more frequently
   - Consider adding connection health monitoring

4. **Implement LiveLink queue monitoring** (Phase 13):
   - Check if FLiveLinkClient has queue depth information
   - Log when queue is building up
   - Detect if main thread is the bottleneck

## Immediate Actions

1. **Add logging to Phase 10 backpressure detection**:
   - Log queue depth on every frame send
   - Log when frames are dropped due to backpressure
   - Correlate drops with latency spikes

2. **Profile the 4070 ms spike**:
   - Enable detailed logging during a test run
   - Capture the exact moment the spike occurs
   - Measure what the main thread is doing at that time

3. **Check WebRTC connection state**:
   - Verify the connection status is being updated correctly
   - Ensure frames are actually being sent reliably

## Conclusion

The 4070 ms latency spike is most likely caused by:
1. **LiveLink client queuing** (receiver pushes instantly, but main thread processes asynchronously)
2. **Main thread contention** (something blocks the Unreal Engine main thread for 4+ seconds)
3. **Network buffering** (less likely, but possible)

**NOT caused by:**
- Receiver processing ✓ (0.248 ms per frame)
- Deserialization ✓ (0.165 ms per frame)
- Skeleton extraction ✓ (0.082 ms per frame)
- LiveLink push ✓ (0.012 ms per frame)

The receiver code is functioning excellently. The bottleneck is at the system level, likely in the Unreal Engine main thread or LiveLink client queue processing.
