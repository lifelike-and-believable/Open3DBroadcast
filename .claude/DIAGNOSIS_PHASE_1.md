# Phase 1.3: Critical Diagnosis - OnDataReceivedEx Not Firing

## Summary of Findings

### Test Results (November 19, 2025)

**Sender Side: ✅ FULLY WORKING**
- Serialization: 89 transforms → 12,760 bytes (correct)
- Labeling: "Quincy" (correct)
- Transmission: lk_send_data_ex() returns success
- Status: 100% functional

**Receiver Mocap Data: ❌ CRITICAL FAILURE**
- OnDataReceivedEx callback: **NEVER INVOKED**
- Poll() shows 0 subjects with pending frames
- No data buffered in PendingFramesBySubject
- Status: Complete failure to receive

**Receiver Audio: ✅ FULLY WORKING**
- OnAudioReceivedEx callback: **INVOKED MULTIPLE TIMES**
- Correct parameters: track='Quincy', participant='ue_publisher1'
- Audio plays correctly
- Status: 100% functional

---

## Critical Observation

**Same receiver connection. Same LiveKit server. Same room session.**

- Audio callback fires ✅
- Data callback doesn't fire ❌

This tells us:
1. ✅ Receiver is connected to LiveKit
2. ✅ Callbacks ARE being registered (audio works)
3. ❌ Labeled data channels NOT being delivered to receiver
4. ❌ LiveKit server not routing sender's labeled data to receiver

---

## Hypothesis: Labeled Data Channel Support

**Most Likely Root Cause:** LiveKit server doesn't support or isn't routing labeled data channels with `lk_send_data_ex()`.

Evidence:
- Sender uses `lk_send_data_ex()` with label parameter
- Receiver registers `lk_client_set_data_callback_ex()` with label support
- Audio (which also uses per-subject labels via track creation) WORKS
- Data (via labeled channels) DOES NOT work

**Possible Explanations:**

1. **LiveKit Server Version**: Server may not support labeled data channels
   - lk_send_data_ex() accepts the data but ignores the label
   - Data sent as unlabeled to default channel
   - Receiver callback never fires for unlabeled data

2. **Callback Registration Failure**: Data callback failed silently
   - lk_client_set_data_callback_ex() returned success (code=0)
   - But callback internally failed to register
   - Audio callback works because it uses different mechanism

3. **Data Delivery Issue**: LiveKit FFI level bug
   - Data callback registered correctly
   - LiveKit server receives data
   - FFI doesn't route to callback for labeled data

---

## Next Test: Enhanced Diagnostics

Build includes new logging to answer:

### Phase 1.3a: Callback Registration Verification
```
[DIAG] Initialize() called - starting setup
[DIAG] About to call SetupClientHandle()
[DIAG] Registering OnDataReceivedEx callback...
[DIAG] OnDataReceivedEx callback registered successfully  <- Will appear if success
[DIAG] OnDataReceivedEx callback registration FAILED: code=X <- Will appear if error
[DIAG] SetupClientHandle() SUCCESS - callbacks should be registered
```

**What we're looking for:** Did callback registration succeed? Any error codes?

### Phase 1.3b: Callback Invocation Verification
```
[DIAG] OnDataReceivedEx INVOKED (call#1): label='Quincy' len=12760 user=0x...
```

**What we're looking for:** Does callback get called AT ALL with any data?

---

## Test Procedure

1. **Launch editor** with latest build
2. **Capture logs** with [DIAG] prefix in Output Log
3. **Run test** (same as before)
4. **Share logs** showing:
   - Callback registration diagnostics
   - Any OnDataReceivedEx invocation attempts
   - Connection state confirmation

---

## If Diagnostics Show...

### Scenario A: Callback registers successfully but NEVER invoked
- Root cause: LiveKit server not routing labeled data
- Solution: Check if server version supports labeled channels
- Next: Try fallback to unlabeled channel (lk_send_data)

### Scenario B: Callback registration fails (code != 0)
- Root cause: LiveKit client doesn't support labeled data callback
- Solution: Check FFI version, check for new callback API
- Next: Implement fallback mechanism

### Scenario C: Callback invoked with NULL/empty label
- Root cause: Data arriving but label getting lost
- Solution: Verify label encoding (UTF-8 round-trip)
- Next: Debug label format at sender

### Scenario D: Everything works as expected
- Indicates previous logs were misleading or cache issue
- Solution: Clear all caches, rebuild clean
- Next: Run full 2-subject test

---

## Key Files Modified This Session

- [WebRTCReceiver.cpp:365-387](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp#L365)
  - Added Initialize() tracing
  - Added SetupClientHandle() success/failure confirmation

- [WebRTCReceiver.cpp:168-193](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp#L168)
  - Added [DIAG] OnDataReceivedEx INVOKED log at entry
  - Will show if callback ever gets called

---

## Critical Next Step

**RUN TEST WITH NEW DIAGNOSTICS** and share complete log output including:
- All [DIAG] lines from Initialize()
- All [DIAG] lines from SetupClientHandle()
- Any [DIAG] OnDataReceivedEx INVOKED lines
- All [ARCH] lines from Sender and Receiver

This will definitively answer: **Is the callback being called at all?**

---

**Session Status**: Diagnostic enhancements applied, ready for next test run.
