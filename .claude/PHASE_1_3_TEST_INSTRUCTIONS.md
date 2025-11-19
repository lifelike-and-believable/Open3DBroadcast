# Phase 1.3: Critical Data Callback Diagnostic - Test Instructions

## What We Know

✅ **Sender**: Successfully sends mocap data with label='Quincy' (12,760 bytes, reliable)
✅ **Audio**: OnAudioReceivedEx callback fires multiple times with correct labels
❌ **Mocap Data**: OnDataReceivedEx callback NEVER invoked

**Question:** Why does audio callback work but data callback doesn't?

---

## Test Instructions

### Step 1: Clear the Output Log
In Unreal Editor, go to **Windows** → **Developer Tools** → **Output Log**
Click the **Clear** button to remove previous test logs

### Step 2: Run the Test
- Open **Windows** → **Developer Tools** → **Automation**
- Search for `Open3DBroadcast.Open3DTransportWebRTC`
- Run the test (e.g., `FWebRTCReceiverInitializeTest`)

### Step 3: Capture Logs
Once the test completes, capture **everything starting from the first [DIAG] line**.

Look specifically for this sequence:

```
[DIAG] Initialize() called - starting setup
[DIAG] About to call SetupClientHandle()
[DIAG] Registering OnDataReceivedEx callback...
[DIAG] OnDataReceivedEx callback registered successfully  ← CRITICAL
[DIAG] Registering OnAudioReceivedEx callback...
[DIAG] OnAudioReceivedEx callback registered successfully
[DIAG] SetupClientHandle() SUCCESS - callbacks should be registered
[DIAG] BeginConnect: URL=... Token=...
[DIAG] OnConnectionState: LkConnConnecting
[DIAG] OnConnectionState: LkConnConnected
```

Then look for:
```
[ARCH] Send() START: Input SubjectList has 1 subjects
[ARCH] Processing subject[0]: name='Quincy' transforms=...
[ARCH] Serialized subject[0]: 12760 bytes (SingleSubjectList buffer)
[ARCH] Sending subject[0] with label='Quincy' (12760 bytes, reliable)
[ARCH] Successfully sent subject[0] with label='Quincy'
```

Then look for (THIS IS THE CRITICAL PART):
```
[DIAG] OnDataReceivedEx INVOKED (call#1): label='Quincy' len=12760 user=0x...
```

**OR if callback never fires:**
```
[ARCH] Poll() START: 0 subjects have pending frames
[ARCH] Poll() END: Processed 0 frames from 0 subjects
```

---

## What to Share

Copy and paste all log lines that contain:
- `[DIAG]` (diagnostic messages)
- `[ARCH]` (architectural messages)

From when receiver initializes through at least one full Send/Poll cycle.

---

## What Each Outcome Means

### Outcome A: Callback Registered ✅ But Never Invoked ❌
```
[DIAG] OnDataReceivedEx callback registered successfully
[DIAG] OnConnectionState: LkConnConnected
[ARCH] Send() ... Successfully sent ...
[ARCH] Poll() START: 0 subjects have pending frames
[DIAG] OnDataReceivedEx INVOKED ← NOT PRESENT
```

**Root Cause:** LiveKit server not delivering labeled data to callback
**Solution:** May need to use fallback (unlabeled data) or check server version

---

### Outcome B: Callback Registration Failed ❌
```
[DIAG] OnDataReceivedEx callback registration FAILED: code=123
```

**Root Cause:** LiveKit FFI doesn't support `lk_client_set_data_callback_ex()` on this version
**Solution:** May need fallback to `lk_client_set_data_callback()` (unlabeled)

---

### Outcome C: Callback Invoked ✅ (Unexpected)
```
[DIAG] OnDataReceivedEx INVOKED (call#1): label='Quincy' len=12760
[ARCH] OnDataReceivedEx ENQUEUED: label='Quincy' 12760 bytes
[ARCH] Poll() START: 1 subjects have pending frames
```

**Root Cause:** Previous logs were misleading, callback IS working
**Solution:** Issue elsewhere, possibly in consumer/animation integration

---

## Ready to Test?

1. ✅ Build is updated with enhanced diagnostics
2. ✅ Test guide is prepared
3. ✅ Know what to look for

**Next:** Run the test and capture logs from receiver initialization through one full test cycle.

Share the logs and we'll identify the root cause!

---

**Build Status**: ✅ Ready
**Diagnostics**: ✅ Enhanced
**Test Guide**: ✅ Updated
