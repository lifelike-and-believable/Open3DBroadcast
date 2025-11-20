# Phase 12: Detailed Per-Operation Timing Instrumentation

## Problem Statement

The receiver animation is experiencing 2000+ ms latency spikes that cause stalling and choppiness. Previous optimization attempts (Phases 1, 4, 6, 7, 10) have not resolved this issue because we were optimizing **speculatively** without knowing where the actual bottleneck is.

The high-level metrics (frames received, bytes deserialized, round-trip latency) tell us WHAT is happening, but not WHERE the time is being spent.

## Solution: Precise Operation Timing

Implemented four complementary timing metrics to pinpoint bottlenecks:

### 1. **AvgParseTimeMs** - FlatBuffer Deserialization
- **Measures**: Duration of `ParseSubjectListBuffer()` call
- **What it tells us**: If this is high (>5ms), deserialization is expensive
- **Possible causes**: Complex FlatBuffer structures, large payload sizes
- **If high**: Consider FlatBuffer optimization or pre-allocation (Phase 3)

### 2. **AvgPoseExtractionTimeMs** - Skeleton/Bone Extraction
- **Measures**: Duration of pose extraction loop in `HandleSerializedFrame()` (calls to `ProcessParsedSubject()`)
- **What it tells us**: If this is high (>10ms), skeleton/transform extraction is expensive
- **Possible causes**:
  - Large skeleton hierarchies (100+ bones)
  - Phase 4 cache misses (frequent skeleton changes)
  - Hash computation or validation
- **If high**: Phase 4 skeleton caching is helping, but may need optimization

### 3. **AvgLiveLinkPushTimeMs** - LiveLink Client Push
- **Measures**: Duration of `PushSubjectStaticData()` + `PushSubjectFrameData()` calls
- **What it tells us**: If this is high (>20ms), the **LiveLink client may be blocking**
- **Possible causes**:
  - LiveLink client queue buildup
  - Subject initialization overhead
  - LiveLink processing bottleneck
- **If high**: This is the PRIMARY SUSPECT for the 2000+ ms spikes
  - LiveLink may be a blocking client that queues frames
  - May need to detect and drop frames when LiveLink falls behind

### 4. **AvgTotalProcessingTimeMs** - Total Frame Processing
- **Measures**: Total duration from frame reception to LiveLink update completion
- **What it tells us**: Overall frame processing time in receiver thread
- **Should be**: <20ms per frame at 30 FPS (33ms frame time)
- **If high**: Combined bottleneck across all operations

## How to Interpret the Metrics

Run the system and periodically dump metrics with the console command:
```
o3d.DumpMetrics
```

You will see output like:
```
[RECEIVER - LIVELINK]
  Avg Round-Trip Latency: 18.45 ms
  Max Latency: 2145.67 ms
  ...

[RECEIVER - PER-OPERATION TIMING]
  Avg FlatBuffer Parse Time: 0.324 ms
  Avg Pose Extraction Time: 2.147 ms
  Avg LiveLink Push Time: 18.752 ms
  Avg Total Processing Time: 21.223 ms
```

### Interpretation Example:

**Scenario 1: LiveLink is the bottleneck**
```
Parse Time:     0.3 ms  (low ✓)
Pose Time:      2.1 ms  (reasonable)
LiveLink Push:  45.8 ms (VERY HIGH ⚠️)
Total Time:     48.2 ms (HIGH, mostly LiveLink)
Max Latency:    2000+ ms
```
→ **Diagnosis**: LiveLink client is blocking or queuing. Frames are waiting for LiveLink to catch up.
→ **Action**: Check LiveLink queue depth, implement backpressure for receiver

**Scenario 2: Deserialization is expensive**
```
Parse Time:     8.5 ms  (HIGH ⚠️)
Pose Time:      3.2 ms  (reasonable)
LiveLink Push:  1.4 ms  (very low ✓)
Total Time:     13.1 ms (still acceptable)
Max Latency:    200 ms
```
→ **Diagnosis**: Deserialization is expensive but not causing 2000+ ms spikes
→ **Action**: Profile FlatBuffer, consider pre-allocation (Phase 3)

**Scenario 3: All operations normal**
```
Parse Time:     0.4 ms  (good)
Pose Time:      1.8 ms  (good)
LiveLink Push:  2.1 ms  (good)
Total Time:     4.3 ms  (excellent)
Max Latency:    200 ms
```
→ **Diagnosis**: Receiver operations are fast. Spikes must be elsewhere:
  - Network transport layer (WebRTC/NNG buffering)
  - LiveLink main thread update (outside receiver source)
  - Unreal Engine rendering pipeline
→ **Action**: Check WebRTC pending frames, profile Unreal Engine LiveLink integration

## Implementation Details

### Metrics Storage
- Located in `FO3DPerformanceMetrics::FReceiverMetrics` struct
- Fields: `AvgParseTimeMs`, `AvgPoseExtractionTimeMs`, `AvgLiveLinkPushTimeMs`, `AvgTotalProcessingTimeMs`
- Updated with exponential moving average (α = 0.2) for low-variance rolling averages

### Instrumentation Code
- **O3DPerformanceMetrics.h/cpp**: Recording functions for each metric
- **O3DReceiverSource.cpp**: Timing calls around key operations
  - `ParseTimingStart/ParseTimeMs`: Around `ParseSubjectListBuffer()`
  - `PoseExtractionStartTime/PoseExtractionTimeMs`: Around pose loop
  - `LiveLinkStartTime/LiveLinkPushTimeMs`: Around `PushSubjectStaticData()` and `PushSubjectFrameData()`
  - `TotalProcessingTimeMs`: From frame entry to completion

### Console Output
```cpp
[RECEIVER - PER-OPERATION TIMING]
  Avg FlatBuffer Parse Time: X.XXX ms
  Avg Pose Extraction Time: X.XXX ms
  Avg LiveLink Push Time: X.XXX ms
  Avg Total Processing Time: X.XXX ms
```

## Next Steps

1. **Collect baseline metrics** with current implementation
2. **Analyze timing breakdown** to identify primary bottleneck
3. **If LiveLink is blocking** (>20ms push time):
   - Check WebRTC backpressure (Phase 10)
   - Consider frame dropping strategy
   - Profile LiveLink client queue depth
4. **If deserialization is expensive** (>5ms parse time):
   - Implement buffer pre-allocation (Phase 3)
   - Profile FlatBuffer field access patterns
5. **If pose extraction is expensive** (>10ms extraction time):
   - Verify Phase 4 skeleton cache is working
   - Profile hash computation and validation

## Key Insight

The user explicitly stated: "I think we need to determine exactly where the stalling is happening before trying to guess what might fix it."

This implementation provides the precise data needed to answer that question. Rather than implementing more blind optimizations, we now have concrete timing measurements that will reveal the actual bottleneck.

**These metrics are the prerequisite for making informed optimization decisions.**

## Commit Information

- **Commit**: Added Phase 12 timing instrumentation
- **Files Modified**:
  - O3DPerformanceMetrics.h: Added recording function declarations
  - O3DPerformanceMetrics.cpp: Implemented timing recording with exponential moving average
  - O3DReceiverSource.cpp: Added timing instrumentation around frame processing pipeline
- **Build Status**: ✓ Compiles successfully with UE 5.7
- **Ready For**: Baseline collection and bottleneck diagnosis
