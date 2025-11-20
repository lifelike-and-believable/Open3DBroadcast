# LiveLink Queue Diagnosis: How to Confirm the Bottleneck

## The Hypothesis

The 4070 ms latency spike is caused by:
1. Receiver pushing data to LiveLink instantly (0.012 ms)
2. LiveLink queuing the update for main thread processing
3. Main thread being busy and not processing the queue
4. When main thread finally gets around to it (4+ seconds later), all queued updates are applied at once

This causes the visible stalling behavior: animation freezes, then suddenly jumps forward.

## How to Test This Hypothesis

### Method 1: Profile LiveLink Push Duration

Add timing AROUND the LiveLink push, not just the receiver side:

```cpp
// In O3DReceiverSource::ProcessParsedSubject(), add timing around LiveLink operations:

double PushStartTime = FPlatformTime::Seconds();

// These are the potentially blocking calls
if (!InitializedSubjects.Contains(SubjectFName) || bNeedStaticUpdate)
{
    PushSubjectStaticData(...);  // May block on LiveLink client
}

PushSubjectFrameData(...);  // May block on LiveLink client

double PushEndTime = FPlatformTime::Seconds();
double LiveLinkTimeMs = (PushEndTime - PushStartTime) * 1000.0;

// If this ever exceeds 5 ms, LiveLink is blocking
if (LiveLinkTimeMs > 5.0)
{
    UE_LOG(LogO3DReceiverSource, Warning,
        TEXT("LiveLink operations took %.2f ms (subject='%s', may indicate main thread contention)"),
        LiveLinkTimeMs, *SubjectFName.ToString());
}
```

**Expected Results:**
- Normally: < 1 ms
- Under main thread contention: 50+ ms, eventually 4000+ ms

### Method 2: Measure Gap Between Push and Actual Update

When does LiveLink actually apply the update to the subject?

```cpp
// In a separate thread or deferred task, measure when the skeleton/curves change:
// Check if the subject's data changed after PushSubjectFrameData

// This requires peeking into FLiveLinkClient internals:
// ILiveLinkClient* Client = ...;
// FLiveLinkSubjectKey Key = ...;
// Check if Client->HasSubject(Key) returns data with new transforms

// The gap between push time and when data actually changes = queue latency
```

### Method 3: Simple Visual Timing

Add logging at both the push and the update application:

```cpp
// Before push
UE_LOG(LogO3DReceiverSource, Warning,
    TEXT("LIVELINK_PUSH_START: subject='%s' frame=%d time=%.2f"),
    *SubjectFName.ToString(), FrameNumber, FPlatformTime::Seconds());

PushSubjectFrameData(...);

// After push completes
UE_LOG(LogO3DReceiverSource, Warning,
    TEXT("LIVELINK_PUSH_END: subject='%s' frame=%d time=%.2f"),
    *SubjectFName.ToString(), FrameNumber, FPlatformTime::Seconds());
```

Then compare timestamps to find gaps.

## Evidence to Look For

### If LiveLink Queue is the Bottleneck:

**In the logs, you'll see:**
```
LIVELINK_PUSH_END: subject='Hips' frame=100 time=1.234
LIVELINK_PUSH_END: subject='Hips' frame=101 time=1.235
LIVELINK_PUSH_END: subject='Hips' frame=102 time=1.236
...
LIVELINK_PUSH_START: subject='Hips' frame=150 time=5.234  <-- 4 second gap!
```

The gap indicates LiveLink is processing a huge queue of queued updates.

### If Main Thread is Blocked:

**LiveLink push itself might hang:**
```
LIVELINK_PUSH_START: subject='Hips' frame=100 time=1.234
(4000 ms passes...)
LIVELINK_PUSH_END: subject='Hips' frame=100 time=5.234
```

The push call itself might block if it tries to enqueue on a full queue or if LiveLink tries to process immediately.

## Quick Diagnostic Code to Add

Here's minimal code to test the hypothesis quickly:

```cpp
// In O3DReceiverSource.h, add:
TAtomic<int32> MaxLiveLinkOperationTimeMs{ 0 };

// In O3DReceiverSource.cpp, in ProcessParsedSubject():
{
    FScopeLock Lock(&LiveLinkTimingLock);
    double T0 = FPlatformTime::Seconds();

    if (!InitializedSubjects.Contains(SubjectFName) || bNeedStaticUpdate)
    {
        PushSubjectStaticData(SubjectKey, BoneNames, BoneParents, CurveNames, SkeletonHash);
    }

    PushSubjectFrameData(SubjectKey, BoneTransforms, CurveNames, CurveValues, SubjectListTime, CurveHash);

    double T1 = FPlatformTime::Seconds();
    int32 TimeMs = static_cast<int32>((T1 - T0) * 1000.0);

    if (TimeMs > MaxLiveLinkOperationTimeMs.Load())
    {
        MaxLiveLinkOperationTimeMs.Store(TimeMs);

        if (TimeMs > 5)
        {
            UE_LOG(LogO3DReceiverSource, Warning,
                TEXT("LiveLink operations took %d ms (frame=%d, subject='%s')"),
                TimeMs, FrameNumber, *SubjectFName.ToString());
        }
    }
}

// Add to metrics dump:
int32 MaxLLOpTime = MaxLiveLinkOperationTimeMs.Load();
if (MaxLLOpTime > 0)
{
    UE_LOG(LogO3DReceiverSource, Warning,
        TEXT("  Max LiveLink Operation Time: %d ms"), MaxLLOpTime);
}
```

Then run `o3d.DumpMetrics` and look for the max LiveLink operation time.

**If this is 4000+ ms, LiveLink is the bottleneck.**

## Expected Behavior

### Normal (Without LiveLink Queue Buildup)
- LiveLink operations: < 1 ms each frame
- Smooth animation
- Avg latency 20 ms, max latency 100-200 ms

### With LiveLink Queue Buildup (Expected Problem)
- Most frames: < 1 ms
- Some frames: 100+ ms (queue being flushed)
- Max frame: 4000+ ms (massive queue dump)
- Animation: smooth for 30 frames, then stalls while queue is being processed

## Summary

To confirm the LiveLink hypothesis:
1. Add timing around `PushSubjectStaticData` and `PushSubjectFrameData`
2. Record the max time any push operation takes
3. Correlate push time spikes with animation stalls
4. If push times spike to 4000+ ms, LiveLink queue is the culprit

This will definitively prove whether:
- ✓ LiveLink queue is the bottleneck
- ✗ Something else (Unreal Engine, GC, rendering) is blocking main thread

Once confirmed, we can implement targeted fixes:
- Batch LiveLink updates
- Defer updates to avoid main thread blocking
- Implement our own frame queue with backpressure
- Configure Unreal Engine for lower latency
