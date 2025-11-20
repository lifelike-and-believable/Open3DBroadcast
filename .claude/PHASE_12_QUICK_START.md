# Phase 12: Quick Start - Collect Timing Metrics

## What Was Just Implemented

Added precise per-operation timing instrumentation to the receiver to identify what's causing the 2000+ ms latency spikes and animation stalling.

**Four new metrics** track where time is spent during frame processing:
1. **Parse Time** - FlatBuffer deserialization duration
2. **Pose Extraction Time** - Skeleton/bone structure extraction duration
3. **LiveLink Push Time** - LiveLink client update duration (primary suspect)
4. **Total Processing Time** - Complete frame reception to update time

## How to Collect Metrics

1. **Start the receiver** with animation running
2. **Run the editor** for 30-60 seconds to let metrics accumulate
3. **Open the console** (backtick key)
4. **Type this command**:
   ```
   o3d.DumpMetrics
   ```
5. **Look for the timing breakdown section**:
   ```
   [RECEIVER - PER-OPERATION TIMING]
     Avg FlatBuffer Parse Time: X.XXX ms
     Avg Pose Extraction Time: X.XXX ms
     Avg LiveLink Push Time: X.XXX ms
     Avg Total Processing Time: X.XXX ms
   ```

## What the Numbers Mean

### Expected "Good" Performance (30 FPS = 33ms frame time)
- Parse Time: < 2 ms
- Pose Extraction: < 5 ms
- LiveLink Push: < 5 ms
- **Total: < 12 ms** (leaving ~20ms for other systems)

### Warning Signs (May Cause Stalling)

**LiveLink Push > 20 ms** ⚠️
- LiveLink client is backing up or blocking
- Likely cause of 2000+ ms spikes
- Fix: Implement frame dropping or backpressure for receiver

**Parse Time > 5 ms** ⚠️
- Deserialization is expensive
- May benefit from Phase 3 (buffer pre-allocation)

**Pose Extraction > 10 ms** ⚠️
- Skeleton extraction is expensive
- Verify Phase 4 skeleton cache is working
- May need to profile hash computation

## Quick Diagnosis Table

| Scenario | Metrics | Likely Cause | Next Action |
|----------|---------|--------------|-------------|
| All low (<5ms each), max latency 2000+ ms | Parse: 1ms, Pose: 2ms, Push: 2ms, Total: 5ms | **Transport or LiveLink main thread** | Check WebRTC queue depth; profile Unreal Engine LiveLink |
| Push time very high | Parse: 1ms, Pose: 2ms, **Push: 50ms**, Total: 55ms | **LiveLink client blocking or queue** | Implement frame dropping strategy; check if LiveLink is synchronous |
| All metrics moderate-high | Parse: 5ms, Pose: 8ms, Push: 8ms, Total: 22ms | **Multiple bottlenecks** | Profile each component separately |
| Parse time high | **Parse: 10ms**, Pose: 2ms, Push: 2ms, Total: 14ms | **FlatBuffer deserialization** | Implement Phase 3 buffer pre-allocation |

## Reset Metrics Between Tests

To start fresh metrics collection:
```
o3d.ResetMetrics
```

Then run your test, then dump metrics.

## Commands Reference

```
o3d.DumpMetrics    # Display all performance metrics including timing breakdown
o3d.ResetMetrics   # Reset all metrics to zero (start fresh collection)
```

## Files That Changed

- **O3DPerformanceMetrics.h**: New recording functions
- **O3DPerformanceMetrics.cpp**: Timing metric implementation with exponential moving average
- **O3DReceiverSource.cpp**: Instrumentation around frame processing pipeline

Build: ✓ Succeeded with UE 5.7

## Key Insight

This gives us the data to answer: **"Where is the stalling happening?"**

The metrics show exactly which operation is consuming the most time, enabling informed optimization decisions instead of speculative changes.

## Next Steps After Collecting Metrics

1. Note the per-operation timing breakdown
2. Identify which operation has the highest timing
3. Share the metrics output - we can then:
   - Diagnose the bottleneck
   - Decide if LiveLink is the culprit
   - Plan specific optimizations based on data
   - Implement targeted fixes rather than guessing
