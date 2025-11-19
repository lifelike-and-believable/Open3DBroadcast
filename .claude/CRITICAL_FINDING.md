# CRITICAL FINDING: OnDataReceivedEx Bug in LiveKit FFI

## Summary

**Root Cause**: The `lk_client_set_data_callback_ex()` function in the LiveKit FFI library has a bug - **it never invokes the callback when data arrives**.

**Evidence**: Source code inspection of `backend_livekit.rs` shows:
- The callback pointer IS stored when registered (line 426)
- But the callback IS NEVER INVOKED (no invocation code exists)
- Only the unlabeled `data_cb` callback is invoked (line 644)

**Impact**: Per-subject labeled data channels are completely broken in current FFI version.

---

## The Issue

### What We Implemented (WebRTC Per-Subject Design)
```cpp
// Sender: Send each subject with a label via labeled channel
lk_send_data_ex(client, buffer, size, reliability, ordered, "Quincy");

// Receiver: Register labeled callback to route by subject
lk_client_set_data_callback_ex(client, OnDataReceivedEx, user);
```

### What the FFI Actually Does
```rust
// FFI: Accepts the callback registration
g.data_cb_ex = cb.map(|f| (f, UserPtr(user)));  // Line 426: ✅ Stores it

// FFI: But ONLY invokes the unlabeled callback
if let Some((cb, user)) = guard.data_cb.as_ref() {  // Line 644: ✅ Invokes unlabeled
    cb(user.0, buf.as_ptr(), buf.len());
}
// data_cb_ex is never invoked! ❌
```

**Result**: Data arrives but callback never fires, causing complete failure of per-subject routing.

---

## Evidence from Your Test

Your logs perfectly demonstrate this:

```
[DIAG] OnDataReceivedEx callback registered successfully  ← FFI accepts registration
[ARCH] Successfully sent subject[0] with label='Quincy'  ← Sender sends data
[ARCH] Poll() START: 0 subjects have pending frames      ← Callback never invoked!
```

The callback was registered (code=0) but never invoked because the FFI implementation has a bug.

---

## Solution Options

### Option 1: Fix the FFI Bug (Proper Solution)
**Requires**: Modifying LiveKit FFI source code to invoke `data_cb_ex` callback

**In backend_livekit.rs** around line 644, change:
```rust
// Current (WRONG): Only invokes data_cb
if let Some((cb, user)) = guard.data_cb.as_ref() {
    cb(user.0, buf.as_ptr(), buf.len());
}

// Should be (FIXED): Also invoke data_cb_ex with label
if let Some((cb, user)) = guard.data_cb_ex.as_ref() {
    let label = CString::new(&topic).unwrap_or_default();
    cb(user.0, label.as_ptr(), reliability, buf.as_ptr(), buf.len());
} else if let Some((cb, user)) = guard.data_cb.as_ref() {
    cb(user.0, buf.as_ptr(), buf.len());
}
```

### Option 2: Use Unlabeled Callback as Workaround (Quick Fix)
**Requires**: Change WebRTC transport to use `lk_client_set_data_callback()` instead

**Trade-off**: Lose per-subject label routing, receive all data on default channel

**Implementation**:
1. Switch from `lk_client_set_data_callback_ex()` to `lk_client_set_data_callback()`
2. Modify `OnDataReceived()` (already implemented as fallback)
3. Use last-observed-subject-name or timestamp to associate data with subjects

---

## Recommended Path Forward

### Immediate (Session): Verify with Fallback Callback
We already implemented the fallback in this session:
1. Build with the fallback `OnDataReceived()` callback
2. Run test - it should fire and data will be received
3. Confirms the issue is specifically with `data_cb_ex` bug

### Short Term: Use Workaround
If FFI won't be fixed immediately:
1. Switch to `lk_client_set_data_callback()`
2. Use unlabeled channel for mocap data
3. Document the limitation

### Long Term: Fix FFI
- Contact LiveKit developers about the bug
- Reference: `lk_client_set_data_callback_ex` never invokes callback
- Provide this analysis and proposed fix

---

## Key Files

**LiveKit FFI Source** (has the bug):
- `E:\OtherProjects\livekit-ffi-ue\livekit_ffi\src\backend_livekit.rs`
- Line 304: `data_cb_ex` definition
- Line 426: Callback registration
- Line 644: Only `data_cb` is invoked (missing `data_cb_ex`)

**Our Implementation** (correct design, blocked by FFI bug):
- `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp`
- Lines 734-748: Labeled callback registration (never fires due to FFI bug)
- Lines 230-284: Fallback unlabeled callback (will work)

---

## Next Step

**Close Live Coding and rebuild** with the fallback callback already implemented. When you run the test, you should see:

```
[DIAG] OnDataReceived INVOKED (call#1): len=12760 (FALLBACK - UNLABELED CHANNEL)
[ARCH] OnDataReceived ENQUEUED: label='default' 12760 bytes
```

This will **prove the diagnosis** and confirm we need to use the workaround.

---

**Status**: Root cause identified with code-level evidence. Solution ready. Awaiting test confirmation.
