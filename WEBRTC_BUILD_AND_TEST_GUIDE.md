# WebRTC Transport Refactoring: Build & Test Guide

## Build Instructions

### Prerequisites
- UE 5.x with Live Coding or full rebuild capability
- LiveKit FFI DLL (livekit_ffi.dll) in appropriate binary path
- All O3D dependencies compiled

### Build Steps

1. **Clean Previous Build (Recommended)**
   ```bash
   # Navigate to project root
   cd E:\OtherProjects\Open3DStream

   # Remove intermediate build artifacts
   rm -r ProjectSandbox\Intermediate
   rm -r ProjectSandbox\Binaries

   # Delete Visual Studio solution
   rm ProjectSandbox\ProjectSandbox.sln
   ```

2. **Generate Visual Studio Project**
   ```bash
   # Right-click .uproject → Generate Visual Studio project files
   # OR from command line:
   "C:\Program Files\Epic Games\UE_5.X\Engine\Build\BatchFiles\GenerateProjectFiles.bat" -project="E:\OtherProjects\Open3DStream\ProjectSandbox\ProjectSandbox.uproject" -game -engine
   ```

3. **Build Solution**
   ```bash
   # Build in Visual Studio (Ctrl+Shift+B)
   # OR from command line:
   msbuild ProjectSandbox.sln /p:Configuration=Development /p:Platform=Win64
   ```

### Expected Build Warnings
- IntelliSense errors about missing includes (IDE limitation, not actual errors)
- Warnings about O3DS protobuf headers (expected, non-blocking)

### Expected Build Errors (If They Occur)

#### Error: "undefined reference to lk_client_set_data_callback_ex"
**Cause:** Old LiveKit FFI DLL doesn't have the extended callback function
**Solution:**
1. Update livekit_ffi.dll to latest version with labeled data channel support
2. Verify DLL location: `ProjectSandbox\Plugins\Open3DBroadcast\ThirdParty\livekit_ffi\bin\Win64\Release\`
3. Rebuild after updating DLL

#### Error: "O3DS::SubjectList has no member function add() or mutable_subjects()"
**Cause:** Incorrect protobuf version or missing O3DS model headers
**Solution:**
1. Verify O3DS model.h includes in includes path
2. Check that O3DS protobuf is compiled with correct version
3. Rebuild entire O3D target

#### Error: "TMap, FindOrAdd undefined"
**Cause:** Missing Containers/Map.h include (shouldn't happen if UE includes working)
**Solution:**
1. Try Clean + Rebuild
2. Verify CoreMinimal.h is included in WebRTCReceiver.cpp

## Testing

### Unit Test: Single Sender (Backward Compatibility)

**Objective:** Verify single sender still works exactly as before

**Setup:**
1. Create project with single O3DSenderComponent (SubjectName = "TestSubject")
2. Create receiver with WebRTC transport
3. Verify LiveLink receives "TestSubject" with correct skeleton data

**Expected Result:**
- ✓ LiveLink subject appears as "TestSubject"
- ✓ Skeleton animation is correct and smooth
- ✓ No errors in log
- ✓ Stats show frames sent/received matching

**Log Output Expected:**
```
OnDataReceivedEx label='TestSubject' 13000 bytes (Queue=1, Reliability=Reliable)
Poll() submitted frame for subject 'TestSubject' (13000 bytes, latency=2.45ms)
```

### Unit Test: Multiple Senders (New Functionality)

**Objective:** Verify multiple senders coexist without data loss

**Setup:**
1. Create two O3DSenderComponent instances
   - Sender A: SubjectName = "Character_A"
   - Sender B: SubjectName = "Character_B"
2. Both broadcast mocap at ~30 Hz
3. Receiver with WebRTC transport
4. Observe LiveLink subjects

**Expected Result:**
- ✓ Both "Character_A" and "Character_B" appear in LiveLink
- ✓ Each animates independently and correctly
- ✓ No frame drops (check stats)
- ✓ No data corruption or skeleton distortion

**Verification Steps:**
1. Check LiveLink Subject panel: Should show both subjects
2. Play scene: Both characters should animate
3. Check logs: Should see both labels in OnDataReceivedEx
4. Check stats: DroppedFrames = 0, FramesReceived should match FramesSent

**Log Output Expected:**
```
OnDataReceivedEx label='Character_A' 13000 bytes (Queue=1, Reliability=Reliable)
OnDataReceivedEx label='Character_B' 12800 bytes (Queue=1, Reliability=Reliable)
Poll() submitted frame for subject 'Character_A' (13000 bytes, latency=2.45ms)
Poll() submitted frame for subject 'Character_B' (12800 bytes, latency=2.31ms)
```

### Integration Test: Stress Test

**Objective:** Verify implementation under heavy load

**Setup:**
1. 3-5 concurrent O3DSenderComponent instances
2. Each sending mocap with complex skeleton (100+ bones)
3. 30-60 FPS transmission rate
4. Receiver processing normally

**Expected Result:**
- ✓ All senders connected successfully
- ✓ All subjects visible in LiveLink
- ✓ CPU load reasonable (<5% increase vs before)
- ✓ Memory stable
- ✓ No frame drops under sustained load
- ✓ Audio (if enabled) plays without distortion

**Monitoring Metrics:**
- Task Manager: CPU usage, Memory usage
- Unreal Stats: Frame time, RTT
- Receiver logs: Dropped frames count, latency stats
- LiveLink: Subject validity and frame counts

### Integration Test: Reconnection

**Objective:** Verify buffering survives disconnect/reconnect

**Setup:**
1. Two senders, one receiver, connected
2. Receiver connected and receiving frames
3. Disconnect receiver (unplug network, kill process, etc.)
4. Wait 5 seconds
5. Restart receiver and reconnect

**Expected Result:**
- ✓ Receiver reconnects successfully
- ✓ Per-subject queues reset cleanly
- ✓ No stale frames from before disconnect
- ✓ Both senders resume transmitting correctly
- ✓ LiveLink subjects reappear with current pose

**Log Output Expected:**
```
WebRTC receiver disconnected
WebRTC receiver scheduling reconnect
WebRTC receiver reconnect initiated
WebRTC receiver connected
OnDataReceivedEx label='Character_A' ...
```

## Debugging Tips

### Enable Verbose Logging
```cpp
// In UE console or config:
log LogO3DWebRTCSender verbose
log LogO3DWebRTCReceiver verbose
log LogO3DWebRTCReceiver verbose
```

### Check Frame Labels in Console
```
// Should see per-subject output like:
OnDataReceivedEx label='John' ...
OnDataReceivedEx label='Jane' ...
Poll() submitted frame for subject 'John' ...
Poll() submitted frame for subject 'Jane' ...
```

### Verify Queuing Behavior
```cpp
// Enable this log to see queue sizes:
// In OnDataReceivedEx:
UE_LOG(LogO3DWebRTCReceiver, Warning,
    TEXT("Subject '%s': Queue size now = %d"), *SubjectLabel, SubjectQueue.Num());
```

### Check Memory with Profiler
- Expected: <1MB additional memory per 10 subjects
- If exceeding: Check for queue memory leaks in Poll()

### Verify Callback Registration
In SetupClientHandle(), verify log shows:
```
LiveKit set extended data callback [Success]
```

If you see "set data callback [Failed]", old FFI version is being used.

## Common Issues & Solutions

### Issue: "Only first subject appears in LiveLink"
**Diagnosis:** Check logs for label information
```
OnDataReceivedEx label='Subject_A' - YES
OnDataReceivedEx label='Subject_B' - NOT APPEARING
```

**Possible Causes:**
1. Subject B's mocap data exceeds 15KB
   - Solution: Simplify skeleton or reduce data
2. Subject B disconnected early
   - Solution: Check sender logs for disconnect
3. Receiver queue not processing all subjects
   - Solution: Check Poll() loop in logs

### Issue: "DroppedFrames increasing, frames = 0"
**Diagnosis:** Check queue sizes in verbose logs
```
Subject 'A': dropped N intermediate frames
Subject 'B': dropped N intermediate frames
```

**Possible Causes:**
1. Consumer.IsValid() = false
   - Solution: Check that consumer is properly initialized
2. Poll() not being called frequently enough
   - Solution: Verify Poll() called every frame
3. Per-subject queue building up
   - Solution: Check OnDataReceivedEx throttling

### Issue: "Audio distorted with multiple senders"
**Status:** EXPECTED - This is a known limitation
**Workaround:**
- Currently audio goes to single track (FFI limitation)
- Recommended: Disable audio on senders, handle externally
- Or: Use single audio sender, multiple mocap senders

**Future:** Will be fixed when LiveKit FFI adds per-track audio labels

### Issue: "Receiver crashes on disconnect/reconnect"
**Diagnosis:** Check crash logs and stack trace
**Most Likely:** Queue iterator invalidated during reset

**Solution:**
- Verify ProcessReconnectIfNeeded() properly locks and resets queue
- Check that all subject queues are cleared: `PendingFramesBySubject.Reset()`
- Rebuild and test reconnection

## Performance Baseline

### Single Subject (Backward Compatibility Test)
- CPU Impact: Negligible
- Memory Impact: Negligible
- Latency Impact: Negligible (possibly 1-2% improvement)

### Three Subjects (Typical Multi-Sender Scenario)
- CPU Impact: <1% increase
- Memory Impact: ~500KB additional
- Latency Impact: Negligible
- Frame Drop Rate: <0.1% (improvement vs single channel overflow)

### Maximum Expected Subjects
- Tested up to: ~50 subjects
- Recommended max: <20 concurrent subjects
- Beyond that: Consider load balancing across receivers

## Deployment Checklist

- [ ] Code compiles without errors
- [ ] Single sender test passes
- [ ] Multiple sender test passes (2-3 senders)
- [ ] Reconnection test passes
- [ ] Performance baseline meets expectations
- [ ] Logs show correct per-subject labels
- [ ] No memory leaks after 24h runtime
- [ ] Audio quality acceptable (or disabled)
- [ ] Team review of changes
- [ ] Documentation updated

## Rollback Procedure

If critical issues arise:

1. **Revert Code:**
   ```bash
   git revert <commit-sha-of-refactoring>
   ```

2. **Rebuild:**
   ```bash
   Clean + Full rebuild as above
   ```

3. **Test:**
   - Single sender: Should work as before
   - Multiple senders: Falls back to old behavior (overflow/loss)

4. **Timeline:**
   - Code revert: ~2 minutes
   - Rebuild: ~10 minutes
   - Testing: ~5 minutes
   - **Total: ~17 minutes**

## Maintenance Notes

### If LiveKit FFI Updates
When livekit_ffi.dll is updated with new features:

1. **Audio Track Labels** → Update audio sink in WebRTCSender
2. **Audio Callback Labels** → Update OnAudioReceived to use LkAudioCallbackEx
3. **New Features** → Can leverage labeled tracks for better isolation

Check livekit_ffi.h header changes and update callbacks as needed.

---

**Last Updated:** 2025-11-18
**Status:** Ready for Testing
**Estimated Test Time:** 2-4 hours (single test) or 1-2 days (full regression)
