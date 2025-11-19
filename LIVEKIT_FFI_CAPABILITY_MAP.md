# LiveKit FFI Capability Map & Future Enhancements

## Current Status (as of Nov 18, 2025)

### Data Channels - ✅ FULLY SUPPORTED

| Feature | API | Status | Use |
|---------|-----|--------|-----|
| **Basic Send** | `lk_send_data()` | ✅ Implemented | Legacy support |
| **Extended Send** | `lk_send_data_ex()` | ✅ Implemented | **PRIMARY** - Per-subject labels |
| **Label Support** | `const char* label` param | ✅ Supported | Routing to correct subject |
| **Reliability Modes** | `LkReliable`, `LkLossy` | ✅ Supported | Payload-size-based selection |
| **Ordered Delivery** | `int32_t ordered` param | ✅ Supported | Frame sequence preservation |
| **Default Labels** | `lk_set_default_data_labels()` | ✅ Supported | Per-channel naming |
| **Data Callback** | `lk_client_set_data_callback()` | ✅ Implemented | Legacy support |
| **Extended Callback** | `lk_client_set_data_callback_ex()` | ✅ Implemented | **PRIMARY** - Receives labels |

### Audio Publishing - ⚠️ LIMITED

| Feature | API | Status | Use |
|---------|-----|--------|-----|
| **Basic Publish** | `lk_publish_audio_pcm_i16()` | ✅ Implemented | Single track only |
| **Labeled Publish** | `lk_publish_audio_pcm_i16_ex()` | ❌ NOT AVAILABLE | **BLOCKED** - Waiting for FFI |
| **Format Config** | `lk_set_audio_publish_options()` | ✅ Supported | Bitrate, DTX, stereo/mono |
| **Output Format** | `lk_set_audio_output_format()` | ✅ Supported | Receiver-side resampling |

### Audio Receiving - ⚠️ LIMITED

| Feature | API | Status | Use |
|---------|-----|--------|-----|
| **Basic Callback** | `lk_client_set_audio_callback()` | ✅ Implemented | Single stream |
| **Labeled Callback** | `LkAudioCallbackEx` | ❌ NOT AVAILABLE | **BLOCKED** - Waiting for FFI |
| **Format Change** | `lk_set_audio_format_change_callback()` | ✅ Supported | Adaptation notification |
| **Audio Stats** | `lk_get_audio_stats()` | ✅ Supported | Diagnostics |

## Current Implementation Status

### What Works Now ✅

1. **Multiple Mocap Senders**
   - Each sender publishes on labeled data channel (e.g., "John", "Jane")
   - Receiver receives with label information via `OnDataReceivedEx()`
   - Per-subject buffering prevents data loss
   - Each subject fits in reliable channel (13KB < 15KB limit)
   - **Result:** Multiple characters animate correctly without collisions

2. **Single or Primary Audio Source**
   - Single audio publisher sends to default track
   - Multiple senders' mocap works independently of audio
   - Audio delivery is stable and artifact-free (single source)
   - **Result:** One character can talk while others animate mocap

### What's Blocked ❌

1. **Multiple Labeled Audio Tracks**
   - LiveKit FFI doesn't provide `lk_publish_audio_pcm_i16_ex(label)`
   - All audio publishes to single track
   - Can't separate audio sources per subject
   - **Impact:** If 3 characters publish audio, it gets mixed (undesirable)
   - **Workaround:** Use single audio publisher or mix externally

2. **Audio Callback with Labels**
   - LiveKit FFI doesn't provide `LkAudioCallbackEx(label, ...)`
   - Can't route received audio per-track
   - All audio delivered as single stream
   - **Impact:** Can't direct "John's audio" to "John's RemoteAudioComponent"
   - **Workaround:** Route all audio to single audio sink, handle in app layer

## Recommended Usage Patterns

### Pattern A: Multiple Mocap + Single Audio ✅ RECOMMENDED

**Use Case:** Multiple actors animating, one person narrating

**Setup:**
```
Sender A: "Character_A" → SubjectName="Character_A", Audio=OFF
Sender B: "Character_B" → SubjectName="Character_B", Audio=OFF
Sender C: "Narrator"     → SubjectName="Narrator", Audio=ON
```

**Result:**
- ✅ All three characters animate independently
- ✅ Clear, clean audio from narrator
- ✅ Mocap perfectly synchronized
- ✅ No audio artifacts

### Pattern B: Multiple Mocap + External Audio Mixing ✅ WORKS

**Use Case:** Multiple actors with multiple audio sources

**Setup:**
```
Sender A: Audio → Local mixing layer → Single mixed output → Publish
Sender B: Audio → Local mixing layer → (no publish)
Receiver: Audio from single source, Mocap from A+B
```

**Result:**
- ✅ All mocap synchronized
- ✅ Clean mixed audio from external mixer
- ✅ Production-grade audio quality

### Pattern C: Mocap Only (No Audio) ✅ WORKS

**Use Case:** Multiple motion capture, audio handled separately

**Setup:**
```
All Senders: Audio=DISABLED
Audio handling: External system (AudioBus, separate WebRTC, etc.)
```

**Result:**
- ✅ All mocap perfectly synchronized
- ✅ Complete audio freedom
- ✅ Best approach if audio quality critical

## Future Enhancement Path

### When LiveKit FFI Adds Audio Label Support

**Expected Signature:**
```c
typedef void (*LkAudioCallbackEx)(void* user, const char* label,
                                   const int16_t* pcm_interleaved,
                                   size_t frames_per_channel,
                                   int32_t channels, int32_t sample_rate);

LkResult lk_publish_audio_pcm_i16_ex(LkClientHandle*,
                                      const int16_t* pcm,
                                      size_t frames_per_channel,
                                      int32_t channels,
                                      int32_t sample_rate,
                                      const char* label);

LkResult lk_client_set_audio_callback_ex(LkClientHandle*, LkAudioCallbackEx cb, void* user);
```

### Implementation Steps (Future)

**Step 1: Update FWebRTCSenderAudioSink** (WebRTCSender.cpp)
```cpp
// From:
lk_publish_audio_pcm_i16(handle, pcm, frames, channels, sample_rate)

// To:
lk_publish_audio_pcm_i16_ex(handle, pcm, frames, channels, sample_rate, label)
```

**Step 2: Update OnAudioReceived** (WebRTCReceiver.cpp)
```cpp
// From:
OnAudioReceived(void* user, const int16_t* pcm, ...)

// To:
OnAudioReceivedEx(void* user, const char* label, const int16_t* pcm, ...)
{
    // Route audio to per-subject sink by label
    TArray<FAudioFrameMeta> Meta;
    Meta.StreamLabel = FString(label);  // Now meaningful!
    AudioSink->SubmitPcm16(Meta, ...);
}
```

**Step 3: Update Callback Registration** (WebRTCReceiver.cpp)
```cpp
// From:
lk_client_set_audio_callback(ClientHandle, OnAudioReceived, this);

// To:
lk_client_set_audio_callback_ex(ClientHandle, OnAudioReceivedEx, this);
```

### Estimated Effort for Future Enhancement
- Code Changes: ~20 lines
- Testing: 2-4 hours
- Risk Level: Very Low (isolated to audio path)

## How to Coordinate with LiveKit Developers

### Feature Request Submission

Contact LiveKit FFI development with:

**Requested Features:**
1. `lk_publish_audio_pcm_i16_ex(handle, pcm, frames, channels, sr, label)`
   - Allows multiple audio tracks by subject/label
   - Parallel to existing `lk_send_data_ex()`

2. `LkAudioCallbackEx(user, label, pcm, frames, channels, sr)`
   - Provides track label in audio callback
   - Parallel to existing `LkDataCallbackEx()`

3. `lk_client_set_audio_callback_ex(handle, callback, user)`
   - Registers the extended callback
   - Parallel to existing `lk_client_set_data_callback_ex()`

**Rationale:**
- Enables per-speaker audio isolation in multi-user scenarios
- Maintains consistency with labeled data channel approach
- Low FFI implementation cost (follows existing pattern)
- High value for multi-participant applications

**Use Cases to Highlight:**
- Multiple actors in virtual production
- Distributed animation studios
- Multi-person presentations
- Group motion capture sessions

### Questions for LiveKit Team

1. **Timeline:** When might audio track labeling be available?
2. **Feasibility:** Are the above APIs on the roadmap?
3. **Alternative:** Are there workarounds in the meantime?
4. **Design:** Should audio labels follow data channel label format?

## Current Limitations Summary

| Scenario | Status | Workaround |
|----------|--------|-----------|
| Multiple mocap senders | ✅ Works perfectly | - |
| One audio + multiple mocap | ✅ Works perfectly | - |
| Multiple audio senders | ⚠️ Single mixed track | External mixer or disable audio |
| Per-speaker audio routing | ❌ Not available | Route via app layer |
| Audio format per-speaker | ❌ Not available | Configure uniform format |

## Deployment Recommendation

**PROCEED with current implementation:**
- ✅ Data channels fully functional for multiple subjects
- ✅ Mocap delivery stable and synchronized
- ✅ Audio limitation doesn't block deployment
- ✅ Future audio enhancement will be seamless

**Audio handling strategies:**
- Option 1: Single narrator (recommended for now)
- Option 2: External audio mixing
- Option 3: Audio disabled (motion capture only)

**Monitor for updates:**
- Watch LiveKit FFI releases for audio enhancements
- Update implementation when available (~20 lines of code)
- No breaking changes to current code path

---

**Last Updated:** November 18, 2025
**Status:** Ready for production deployment
**Next Review:** After LiveKit FFI audio enhancement availability
