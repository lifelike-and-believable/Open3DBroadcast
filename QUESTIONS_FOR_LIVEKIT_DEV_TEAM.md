# Questions for LiveKit FFI Dev Team

## Context
We're implementing multi-sender motion capture and audio support in Unreal Engine using LiveKit FFI. We need to support multiple simultaneous audio sources (e.g., multiple actors speaking) without audio distortion.

You mentioned these APIs exist:
- `lk_audio_track_create()`
- `lk_audio_track_publish_pcm_i16()`
- `lk_audio_track_destroy()`

We need the exact signatures and additional details to integrate them.

---

## Critical Questions

### 1. Audio Track Creation

**Question:** What is the exact C signature for `lk_audio_track_create()`?

**What we need:**
- Full parameter list (all parameters, types, and purposes)
- Return type (is it a pointer to opaque struct?)
- Configuration options (sample rate, channels, buffer size, etc.)
- Error conditions (what return values indicate failure?)

**Example of what we expect:**
```c
typedef struct LkAudioTrack LkAudioTrack;

LkAudioTrack* lk_audio_track_create(
    LkClientHandle* client,
    int32_t sample_rate,
    int32_t channels,
    size_t buffer_size);
```

**Is this correct, or different?**

---

### 2. Audio Track Publishing

**Question:** What is the exact C signature for `lk_audio_track_publish_pcm_i16()`?

**What we need:**
- Full parameter list
- PCM format details (interleaved vs. planar? bit depth?)
- Frame count semantics (frames per channel or total?)
- Return type and error handling
- Buffer ownership (do we free it, or does FFI keep reference?)

**Example of what we expect:**
```c
LkResult lk_audio_track_publish_pcm_i16(
    LkAudioTrack* track,
    const int16_t* pcm_interleaved,
    size_t frames_per_channel,
    int32_t channels,
    int32_t sample_rate);
```

**Is this correct? What about:**
- Can sample_rate differ from track creation?
- Can channels differ from track creation?
- What's the maximum frames per call?

---

### 3. Track Cleanup

**Question:** What is the exact C signature for `lk_audio_track_destroy()`?

**What we need:**
- Parameter: is it just the `LkAudioTrack*`?
- Timing: when is it safe to call? (during connection, after disconnect?)
- Cleanup: does it stop publishing immediately?
- Error handling: what if called twice?

**Example of what we expect:**
```c
LkResult lk_audio_track_destroy(LkAudioTrack* track);
```

**Questions:**
- Is it synchronous?
- What happens to in-flight audio frames?
- Can we recreate a track with same label after destroy?

---

## Capability Questions

### 4. Multiple Tracks Per Connection

**Question:** Can we create and publish to multiple audio tracks simultaneously on a single `LkClientHandle`?

**Scenario:**
```
Client connects to room
Create Track A (for "Speaker_1")
Create Track B (for "Speaker_2")
Publish audio to Track A concurrently with Track B
Receiver hears both speakers cleanly separated?
```

**Answer needed:**
- YES / NO / WITH LIMITATIONS
- If limitations, what are they?
- Maximum number of concurrent tracks?
- Any bandwidth/performance implications?

---

### 5. Track Identification for Receiver

**Question:** How does the receiver identify which audio is from which track?

**Context:** We label data channels as "John", "Jane", etc. Can we label audio tracks similarly?

**Answer needed:**
- Do tracks have labels/identifiers?
- How are they communicated to receivers?
- Are they tied to data channel labels or separate?
- Example usage?

---

### 6. Sample Rate & Channel Flexibility

**Question:** Can we create tracks with different configurations?

**Scenario:**
```
Track A: 48kHz, 2 channels (stereo singer)
Track B: 16kHz, 1 channel (mono narrator)
Track C: 44.1kHz, 2 channels (stereo musician)
All on same connection?
```

**Answer needed:**
- YES / NO / WITH CONSTRAINTS
- Any performance impact from mixed configs?
- Are there recommended/optimal configs?
- Any fixed requirements?

---

### 7. Lifecycle and Reconnection

**Question:** What happens to audio tracks when connection drops/reconnects?

**Scenario:**
```
Connection established, Track A created and publishing
Network drops for 5 seconds
Reconnect completes
Can we continue using Track A, or must we recreate?
```

**Answer needed:**
- Must we recreate tracks after reconnect?
- Do old track handles become invalid?
- Any state we need to manage?

---

### 8. Timeline for Header Availability

**Question:** When will these APIs be available in the public livekit_ffi.h header?

**Context:** We can add the declarations manually if needed, but prefer official inclusion.

**Answer needed:**
- Already available (if so, why not in header version we have?)
- Available in next release (when?)
- Still in development (when expected?)
- Any way to get early access to .h file?

---

## Nice-to-Have Questions

### 9. Error Handling Details

**What do these error codes mean?**
```c
LkResult Result = lk_audio_track_publish_pcm_i16(...);
if (Result.code != 0) {
    // What does code 0x201 mean?
    // Is there a defined enum?
    // What's the message?
}
```

---

### 10. Performance Characteristics

**What are realistic performance expectations?**
- CPU overhead per track?
- Latency from capture to receiver?
- Maximum sample rate?
- Recommended buffer sizes?
- Any settings we should tune?

---

## Follow-Up

### Immediate Next Steps
Once we have answers, we will:
1. Update `livekit_ffi.h` with exact signatures
2. Implement multi-track audio publishing in WebRTC sender
3. Add automatic track creation per audio source
4. Test with 2-3 concurrent audio sources
5. Validate audio quality and separation

### Expected Implementation Time
- With signatures: 2-3 hours
- Testing: 2-4 hours
- Total: Same day if we get answers by morning

---

## Contact & Context

**Project:** Open3DStream / Open3DBroadcast
**Use Case:** Multi-actor motion capture with independent audio sources
**Platform:** Windows 64-bit, Unreal Engine 5.x
**Current Status:** Data channels working, audio distortion is blocker

**Please respond to:** [Your Contact]
**Timeline:** ASAP preferred (blocking production deployment)

---

## Reference: What We've Already Implemented

For context, here's what's working:

**Data Channels (Complete):**
- ✅ Multiple senders, each on labeled data channel
- ✅ Per-subject buffering prevents data loss
- ✅ Working with mocap payloads ~13KB each

**Audio (Limited):**
- ⚠️ Single default audio track
- ⚠️ Multiple audio sources cause distortion
- ✅ Will work perfectly once track APIs available

**Code Ready:**
- ✅ Structure prepared to use audio track APIs
- ✅ Just need FFI signatures to complete

---

**Sent:** November 18, 2025
**Urgency:** CRITICAL (blocks production deployment)
