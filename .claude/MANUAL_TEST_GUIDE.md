# Phase 0 Manual Test Guide - WebRTC Architecture Verification

## Quick Start

### What This Test Does
Verifies the per-subject WebRTC architecture by running a sender and receiver connected to the same LiveKit server, then examining the architectural verification logs.

### Prerequisites
- Active LiveKit server instance (development or production)
- Valid room URL and authentication tokens
- ProjectSandbox built and ready to run

---

## Step 1: Configure Test Parameters

Edit the test configuration with your LiveKit server details:

**File:** `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Private/Tests/WebRTCTransportTests.cpp`

Replace the example URLs/tokens with your actual server:
```cpp
TEXT("wss://your-livekit-server.com")  // Your actual LiveKit server URL
TEXT("your-valid-token")                // Your actual authentication token
```

---

## Step 2: Launch the Editor

```bash
"C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UE4Editor.exe" \
  "e:\OtherProjects\Open3DStream\ProjectSandbox\ProjectSandbox.uproject" -log
```

The `-log` flag ensures logs are captured.

---

## Step 3: Open the Output Log

In the Unreal Editor:
1. **Windows** → **Developer Tools** → **Output Log**
2. You should see the Output Log window open at the bottom

---

## Step 4: Run the Automation Tests

1. **Windows** → **Developer Tools** → **Automation**
2. In the Automation window, search for: `Open3DBroadcast.Open3DTransportWebRTC`
3. You'll see several tests listed. Run: `FWebRCTConnectionInitializeTest` or `FWebRTCReceiverInitializeTest`

**Note:** These tests will attempt to connect to your LiveKit server.

---

## Step 5: Capture the Logs

Once the test runs, look in the **Output Log** for messages with these prefixes in this order:

### CRITICAL: Capture Logs from Receiver Connection Start

**Clear the Output Log BEFORE starting the test** to avoid logs from previous runs.

Look for this sequence (in order of appearance):

```
[DIAG] Initialize() called - starting setup
[DIAG] About to call SetupClientHandle()
[DIAG] Registering OnDataReceivedEx callback...
[DIAG] OnDataReceivedEx callback registered successfully  ← CRITICAL: Does this appear?
[DIAG] Registering OnAudioReceivedEx callback...
[DIAG] OnAudioReceivedEx callback registered successfully
[DIAG] SetupClientHandle() SUCCESS - callbacks should be registered
[DIAG] OnConnectionState: LkConnConnecting
[DIAG] OnConnectionState: LkConnConnected ← Data callback should fire AFTER this
```

Then look for these messages prefixed with `[ARCH]`:

### Expected Log Pattern (Success Case)

If data flows correctly through the architecture, you should see:

```
[ARCH] Send() START: Input SubjectList has 1 subjects
[ARCH] Processing subject[0]: name='TestActor' transforms=15
[ARCH] SingleSubjectList created for subject[0]: 15 transforms will be copied
[ARCH] Serialized subject[0]: 8234 bytes (SingleSubjectList buffer)
[ARCH] Sending subject[0] with label='TestActor' (8234 bytes, reliable)
[ARCH] Successfully sent subject[0] with label='TestActor'
[ARCH] Send() END: Processed 1/1 subjects (SUCCESS)

[ARCH] OnDataReceivedEx ENTRY: label='TestActor' len=8234 reliability=Reliable
[ARCH] OnDataReceivedEx ENQUEUED: label='TestActor' 8234 bytes (QueueLen=1)

[ARCH] Poll() START: 1 subjects have pending frames
[ARCH] Poll() DEQUEUED: subject='TestActor' bytes=8234 (dropped 0 intermediate frames)
[ARCH] Poll() SUBMIT: 1 frames ready for consumer
[ARCH] Poll() SUBMITTING: subject='TestActor' bytes=8234 to consumer
[ARCH] Poll() SUBMITTED: subject='TestActor' (FramesProcessed=1)
[ARCH] Poll() END: Processed 1 frames from 1 subjects
```

---

## Step 6: Analyze the Logs

Use the **Failure Point Detection Table** below to identify where the flow breaks:

### Failure Point Detection

| Last Log Message | Status | Root Cause | Investigation |
|---|---|---|---|
| `[ARCH] Send() START` only | ✅ Send called | Check if input list is empty | Look for "Processing subject[0]" |
| `[ARCH] Processing subject[0]` appears | ✅ Subject found | Data structure OK | Look for "SingleSubjectList created" |
| `[ARCH] SingleSubjectList created` appears | ✅ List creation OK | Copy phase proceeding | Look for "Serialized subject[0]" |
| `[ARCH] Serialized subject[0]` appears | ✅ Serialization OK | Data encodes to bytes | Look for "Sending subject[0]" |
| `[ARCH] Sending subject[0]` appears | ✅ About to send | FFI about to be called | Look for "Successfully sent" |
| `[ARCH] Successfully sent` appears | ✅ **SENDER OK** | Data left sender | **Look for receiver logs** |
| `[ARCH] Send() END: ... SUCCESS` appears | ✅ **SEND COMPLETE** | All subjects processed | **Check OnDataReceivedEx** |
| ❌ No `OnDataReceivedEx ENTRY` | 🔴 **CRITICAL** | Receiver not getting data | **Connection issue likely** |
| `[ARCH] OnDataReceivedEx ENTRY` appears | ✅ **RECEIVER WORKING** | Data reached callback | Look for "ENQUEUED" |
| `[ARCH] OnDataReceivedEx ENQUEUED` appears | ✅ **BUFFERING OK** | Frame queued per-subject | Look for "Poll() START" |
| `[ARCH] Poll() START` appears | ✅ **POLL RUNNING** | Dequeue phase starting | Look for "DEQUEUED" |
| `[ARCH] Poll() DEQUEUED` appears | ✅ **FRAME EXTRACTED** | Latest frame retrieved | Look for "SUBMIT" |
| `[ARCH] Poll() SUBMITTING` appears | ✅ **CONSUMER READY** | About to call consumer | Look for "SUBMITTED" |
| `[ARCH] Poll() SUBMITTED` appears | ✅ **COMPLETE FLOW** | All architecture OK | Flow is working correctly |

---

## Step 7: Report Your Results

Share the following in your response:

### Minimal Report Template

```
TEST RESULTS: [SUCCESS / PARTIAL / FAILURE]

Last visible [ARCH] log message:
[Copy the last log line you see]

Sender logs present: [YES / NO / PARTIAL]
Receiver logs present: [YES / NO]

Failure point (if applicable):
[Where the logs stop appearing]

Full log output:
[Paste all logs prefixed with [ARCH]]
```

### Detailed Report Template

```
TEST DATE: [Date]
TEST DURATION: [How long test ran]

ARCHITECTURE VERIFICATION RESULTS
==================================

SENDER SIDE:
- Send() called: [YES / NO]
- Subject processing: [COUNT]
- Serialization succeeded: [YES / NO]
- Data sent to LiveKit: [YES / NO]

RECEIVER SIDE:
- Connected to server: [YES / NO / UNKNOWN]
- OnDataReceivedEx called: [YES / NO]
- Frames queued: [COUNT]
- Poll() executed: [YES / NO]
- Consumer called: [YES / NO]

ANALYSIS:
[Describe what you observe]

FULL LOG OUTPUT:
[Paste all relevant logs]
```

---

## Troubleshooting

### No [ARCH] logs appearing at all?
- Check that logs are being captured (Window > Output Log)
- Verify the project was built with the latest code (includes logging)
- Try running a simple initialization test first

### Only sender logs, no receiver logs?
- **Most likely:** Sender and receiver not connected to same LiveKit server
- Check LiveKit server is running and accessible
- Verify room URL and tokens are correct
- Check network connectivity

### Sender logs but no OnDataReceivedEx?
- **Likely:** Data sent but not reaching receiver callback
- Check LiveKit FFI version (must support labeled data channels)
- Verify lk_client_set_data_callback_ex() was registered

### OnDataReceivedEx logs but no Poll()?
- **Possible:** Game loop not ticking receiver's Poll()
- Check that receiver is being updated each frame
- Verify receiver was added to the scene

### All logs present but no animation?
- Architecture is working correctly
- Issue is likely in consumer/animation layer (beyond Phase 0)
- Proceed to Phase 1 diagnostics

---

## Phase 1.3 Diagnostic: Data Callback Status (CRITICAL)

**Current Status:** Sender successfully sends mocap data, but receiver's OnDataReceivedEx callback never fires.

**What This Test Will Determine:**
1. **Is callback registered?** Check for `[DIAG] OnDataReceivedEx callback registered successfully`
2. **Does callback ever get invoked?** Search for `[DIAG] OnDataReceivedEx INVOKED`
3. **Is connection working?** Check for `[DIAG] OnConnectionState: LkConnConnected`

**If you see:**
- ✅ Callback registered + LkConnConnected + [ARCH] Send() SUCCESS
- ❌ NO [DIAG] OnDataReceivedEx INVOKED
- **Conclusion:** LiveKit server not routing labeled data channels

**If you see:**
- ✅ Callback registered + LkConnConnected
- ✅ [DIAG] OnDataReceivedEx INVOKED (call#1): label='Quincy' ...
- **Conclusion:** Callback IS working, we can proceed to frame buffering issues

---

## Next Steps After Test

Once you have the log output:

1. **Post the logs** starting from `[DIAG] Initialize()` call
2. **Focus on:** Callback registration lines + first occurrence of [DIAG] OnDataReceivedEx INVOKED (if any)
3. **I will analyze** to determine if issue is at LiveKit server level or in our callback handling
4. **We will identify** root cause and proceed to appropriate fix

---

## Additional Testing (Optional)

### Test Multiple Subjects

To test per-subject isolation, modify the test to create 2 subjects:

```cpp
O3DS::SubjectList List;
O3DS::Subject* Subject1 = List.addSubject("Actor_A", nullptr);
O3DS::Subject* Subject2 = List.addSubject("Actor_B", nullptr);

// Add some transforms to each subject
// ...

Sender.Send(List);
```

You should then see:
```
[ARCH] Send() START: Input SubjectList has 2 subjects
[ARCH] Processing subject[0]: name='Actor_A' ...
[ARCH] Processing subject[1]: name='Actor_B' ...
[ARCH] Send() END: Processed 2/2 subjects (SUCCESS)

[ARCH] OnDataReceivedEx ENTRY: label='Actor_A' ...
[ARCH] OnDataReceivedEx ENTRY: label='Actor_B' ...
```

This confirms per-subject routing is working.

---

## Questions?

If you encounter issues or have questions while running the test:
1. Share the full log output with `[ARCH]` messages
2. Include any error messages from LiveKit
3. Describe what you expected vs. what happened
