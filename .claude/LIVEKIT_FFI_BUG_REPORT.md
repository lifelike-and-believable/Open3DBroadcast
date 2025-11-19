# LiveKit FFI Bug Report: OnDataReceivedEx Callback Never Invoked

## Summary

The `lk_client_set_data_callback_ex()` function accepts a labeled data callback but **never invokes it when data arrives**. Only the unlabeled `lk_client_set_data_callback()` callback actually fires.

## Problem Description

### Expected Behavior
When a sender uses `lk_send_data_ex()` with a label parameter:
```cpp
lk_send_data_ex(client, data, len, reliability, ordered, "subject_label");
```

A receiver with `lk_client_set_data_callback_ex()` registered should receive a callback:
```cpp
void OnDataReceivedEx(void* user, const char* label, LkReliability reliability,
                       const uint8_t* bytes, size_t len) {
    // Should be called with label="subject_label"
}
```

### Actual Behavior
The callback is **never invoked**, even though:
- `lk_client_set_data_callback_ex()` returns success (code=0)
- Data IS successfully sent by the sender
- The callback pointer is stored in the client structure

### Impact
Users cannot implement per-label data routing. All labeled data channels are broken.

## Root Cause

**File**: `livekit_ffi/src/backend_livekit.rs`

The `data_cb_ex` callback is defined and stored but never invoked:

```rust
// Line 304: Field defined
pub struct Client {
    data_cb: Option<(extern "C" fn(...), UserPtr)>,
    data_cb_ex: Option<(extern "C" fn(..., label, reliability, ...), UserPtr)>,
    // ...
}

// Line 426: Callback IS stored when set
pub extern "C" fn lk_client_set_data_callback_ex(...) -> LkResult {
    let mut g = c.0.lock().unwrap();
    g.data_cb_ex = cb.map(|f| (f, UserPtr(user)));  // ✅ Stored
    ok()
}

// Line 644+: But ONLY data_cb is invoked
if let Some((cb, user)) = guard.data_cb.as_ref() {
    cb(user.0, buf.as_ptr(), buf.len());           // ✅ This fires
}
// data_cb_ex is never invoked!                    // ❌ Missing code
```

## Required Fix

In the data reception handler (around line 644), add invocation of `data_cb_ex`:

```rust
// After storing the topic from label:
let topic = if !label.is_null() {
    unsafe { cstr(label) }.unwrap_or("custom").to_string()
} else {
    match effective_rel {
        LkReliability::Reliable => g.data_labels.reliable.clone(),
        LkReliability::Lossy => g.data_labels.lossy.clone(),
    }
};

// Invoke the appropriate callback:
if let Some((cb, user)) = guard.data_cb_ex.as_ref() {
    // NEW: Invoke extended callback with label
    let label_cstr = CString::new(&topic).unwrap_or_default();
    cb(user.0, label_cstr.as_ptr(), reliability, buf.as_ptr(), buf.len());
} else if let Some((cb, user)) = guard.data_cb.as_ref() {
    // Fallback to unlabeled callback
    cb(user.0, buf.as_ptr(), buf.len());
}
```

## Workaround

Until this is fixed, use the unlabeled callback only:
```cpp
lk_client_set_data_callback(client, OnDataReceived, user);
// All data arrives without label information
```

## Testing

This bug can be verified by:
1. Registering `lk_client_set_data_callback_ex()`
2. Sending data with `lk_send_data_ex(client, data, len, rel, 1, "test_label")`
3. Observing that the callback is never invoked
4. Comparing to `lk_client_set_data_callback()` which IS invoked

## Files Affected

- `livekit_ffi/src/backend_livekit.rs` - Missing callback invocation
- `livekit_ffi/include/livekit_ffi.h` - Function is declared but broken

## Severity

**High** - Feature is completely non-functional (callbacks never fire)

---

**Discovered**: November 19, 2025
**Environment**: LiveKit FFI wrapper for Unreal Engine
**Affected API**: `lk_client_set_data_callback_ex()` and `lk_send_data_ex()`
