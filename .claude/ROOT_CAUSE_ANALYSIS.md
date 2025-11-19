# Root Cause Analysis - OnDataReceivedEx Not Firing

## Summary

**Test Results Confirm**: The `OnDataReceivedEx` callback is **registered successfully** but **NEVER invoked**.

### Evidence from Test Logs

```
[DIAG] Registering OnDataReceivedEx callback...
[DIAG] OnDataReceivedEx callback registered successfully     ← ✅ Callback IS registered
[DIAG] SetupClientHandle() SUCCESS - callbacks should be registered

[ARCH] Send() START: Input SubjectList has 1 subjects
[ARCH] Sending subject[0] with label='Quincy' (12760 bytes, reliable)
[ARCH] Successfully sent subject[0] with label='Quincy'    ← ✅ Data IS sent

[ARCH] Poll() START: 0 subjects have pending frames         ← ❌ NO DATA RECEIVED
[ARCH] Poll() END: Processed 0 frames from 0 subjects

⚠️  NO [DIAG] OnDataReceivedEx INVOKED logs ever appear
```

## Root Cause Determination

**Most Likely**: **LiveKit server does NOT support labeled data channels** via `lk_send_data_ex()`.

### Supporting Evidence

1. **Same Connection**: Both audio and mocap data use the same LiveKit connection
2. **Audio Works**: OnAudioReceivedEx callback fires correctly with per-subject track labels
3. **Mocap Fails**: OnDataReceivedEx callback never fires despite successful send
4. **Different Mechanism**:
   - Audio: Uses per-subject **tracks** created with `lk_audio_track_create()`
   - Mocap: Uses **labeled data channels** via `lk_send_data_ex()`

### ROOT CAUSE IDENTIFIED: LiveKit FFI Implementation Bug ⚠️

**The `OnDataReceivedEx` callback IS NEVER INVOKED in the FFI library.**

**Evidence from LiveKit FFI source code** (`E:\OtherProjects\livekit-ffi-ue\livekit_ffi\src\backend_livekit.rs`):
- Line 304: `data_cb_ex` field defined in Client struct
- Line 346: Initialized to None
- Line 426: Set when `lk_client_set_data_callback_ex()` is called ✅
- **Line 644+**: Only `data_cb` (unlabeled) is invoked when data arrives
- **MISSING**: No code path that calls `data_cb_ex` callback ❌

**Impact**: The FFI accepts the extended callback registration but has a bug - **it never calls the callback when data arrives**, regardless of whether it has a label or not.

**This explains everything:**
- ✅ Callback registration succeeds (returns code=0) - callback pointer is stored
- ❌ Callback never fires - FFI never invokes it
- ✅ Unlabeled callback will work - that's what actually gets invoked by the FFI

**Solution Path**: Use the unlabeled callback (`lk_client_set_data_callback()`) and handle all data on default channel.

## Diagnostic Solution Implemented

Added a **fallback callback** to test this hypothesis:

### What's New

**File**: `WebRTCReceiver.cpp`

**New Callback**: `OnDataReceived()` - unlabeled data callback
- Listens on default/unnamed channel (no label parameter)
- Queues any data that arrives without a label
- Logs every invocation with `[DIAG] OnDataReceived INVOKED`

**Fallback Registration**: In `SetupClientHandle()`
```cpp
// Try labeled callback first
lk_client_set_data_callback_ex(ClientHandle, OnDataReceivedEx, this);

// Also register unlabeled callback as diagnostic fallback
if (labeled_registration_succeeded)
{
    lk_client_set_data_callback(ClientHandle, OnDataReceived, this);  // Fallback
}
```

**Expected Outcomes**:

### Scenario 1: Fallback Callback Fires ✅
```
[DIAG] OnDataReceived INVOKED (call#1): len=12760 (FALLBACK - UNLABELED CHANNEL)
[ARCH] OnDataReceived ENQUEUED: label='default' 12760 bytes
[ARCH] Poll() START: 1 subjects have pending frames
```

**Conclusion**: Server sends data on DEFAULT channel, not labeled channels
**Solution**: Accept all data on default channel, lose per-subject routing

### Scenario 2: Neither Callback Fires ❌
```
[DIAG] OnDataReceivedEx INVOKED - NEVER APPEARS
[DIAG] OnDataReceived INVOKED - NEVER APPEARS
[ARCH] Poll() START: 0 subjects have pending frames
```

**Conclusion**: Data not reaching client at all
**Solution**: Check network, connection, or server version

### Scenario 3: Labeled Callback Fires ✅
```
[DIAG] OnDataReceivedEx INVOKED (call#1): label='Quincy' len=12760
[ARCH] OnDataReceivedEx ENQUEUED: label='Quincy' 12760 bytes
```

**Conclusion**: Previous logs were misleading, callback works correctly
**Solution**: Data IS arriving, issue is elsewhere (consumer/animation)

## Next Steps

### 1. Close Live Coding
- In Unreal Editor: **Ctrl+Alt+F11** to disable Live Coding
- Or close and reopen the editor

### 2. Rebuild Project
```bash
"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" ProjectSandboxEditor Win64 Development -Project="e:\OtherProjects\Open3DStream\ProjectSandbox\ProjectSandbox.uproject"
```

### 3. Run Test Again
- Clear Output Log
- Run receiver test
- Look for:
  - `[DIAG] Also registering OnDataReceived fallback`
  - `[DIAG] OnDataReceived INVOKED` (if fallback callback fires)
  - `[DIAG] OnDataReceivedEx INVOKED` (if labeled callback fires)

### 4. Share Results
Post the logs showing which callback(s) fire, and we can determine the exact issue.

---

## Key Insight

**We have two diagnostics now**:
1. **Labeled callback** (`OnDataReceivedEx`) - for per-subject routing
2. **Unlabeled fallback** (`OnDataReceived`) - to detect if server uses default channel

**This test will definitively answer**: Does the server support labeled data channels?

**If labeled channels work**: We need to find why callback isn't being invoked (FFI issue)
**If unlabeled channel works**: We need to modify sender to use `lk_send_data()` instead

---

## Files Modified

- `WebRTCReceiver.h` - Added `OnDataReceived` declaration
- `WebRTCReceiver.cpp` - Implemented `OnDataReceived` callback + fallback registration

**Status**: Code ready, waiting for build + test
