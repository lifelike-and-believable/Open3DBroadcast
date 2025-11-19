# WebRTC Transport Refactoring: Multi-Sender Support

## Overview
The Open3DTransportWebRTC module has been refactored to support multiple labeled data channels and audio tracks, enabling multiple mocap senders and audio publishers to coexist without overwhelming a single channel or causing audio distortion.

## Problem Statement
The original implementation had critical limitations:
1. **Single Data Channel**: All mocap from all subjects was sent through one channel (max 15KB), causing overflow when multiple senders transmitted simultaneously (each sender's mocap ~13KB)
2. **Single Audio Track**: All audio streams collided on one track, creating distortion and preventing per-subject audio routing
3. **No Subject-Level Buffering**: A single frame queue meant data loss when multiple senders transmitted concurrently
4. **Hardcoded Subject Name**: Receiver used a fixed name from config, preventing dynamic per-sender identification

## Solution Architecture

### Sender-Side Changes (WebRTCSender)

#### Key Changes:
1. **Per-Subject Serialization** (WebRTCSender::Send)
   - Changed from serializing entire SubjectList in one frame to serializing each Subject individually
   - Each subject gets its own buffer and is sent independently
   - This keeps individual mocap payloads well under the 15KB limit even with complex skeletons

2. **Labeled Data Channels**
   - Uses `lk_send_data_ex()` instead of `lk_send_data()`
   - Each subject's mocap is labeled with its subject name: `lk_send_data_ex(..., label=subject_name, ...)`
   - Labels enable receiver-side routing to correct LiveLink subjects
   - Preserves frame ordering with `ordered=1` parameter

3. **Reliability Determination**
   - Per-subject payload size validation
   - Automatically falls back to reliable channel if exceeding lossy limit (1300 bytes)
   - Logs warnings for subjects approaching or exceeding reliable limit (15KB)

#### Code Location:
- File: `WebRTCSender.cpp`, method `Send()`
- Lines: 317-435

#### Pseudo-code:
```cpp
for each subject in SubjectList:
    - Serialize single subject to buffer
    - Determine reliability based on payload size
    - Send via: lk_send_data_ex(handle, buffer, size, reliability, ordered=1, label=subject_name)
```

### Receiver-Side Changes (WebRTCReceiver)

#### Key Changes:

1. **Extended Data Callback** (WebRTCReceiver::OnDataReceivedEx)
   - Replaced `OnDataReceived()` with `OnDataReceivedEx()`
   - New signature: `OnDataReceivedEx(void* user, const char* label, LkReliability reliability, ...)`
   - Receives label information indicating which subject this data belongs to
   - Enables per-subject routing of incoming mocap data

2. **Per-Subject Frame Buffering**
   - Changed from single queue (`TArray<FPendingFrame> PendingFrames`) to per-subject queues
   - New structure: `TMap<FString, TArray<FPendingFrame>> PendingFramesBySubject`
   - Each labeled data stream has its own queue
   - Prevents data loss when multiple subjects transmit simultaneously

3. **Enhanced Poll() Method**
   - Extracts latest frame from each subject's queue (not just one global frame)
   - Processes each subject independently
   - Submits each subject's frame with its label to the consumer
   - Returns count of frames processed (can now be >1 when multiple subjects have frames)

#### Code Locations:
- **Header**: `WebRTCReceiver.h`
  - Line 67: New `PendingFramesBySubject` map
  - Line 90: New `OnDataReceivedEx()` callback signature

- **Implementation**: `WebRTCReceiver.cpp`
  - Lines 163-193: `OnDataReceivedEx()` implementation
  - Lines 352-381: Updated `Stop()` to use extended callback
  - Lines 383-465: Refactored `Poll()` for per-subject processing
  - Lines 514: Updated `SetupClientHandle()` to register extended callback
  - Lines 588-589: Updated `ProcessReconnectIfNeeded()` for extended callback

#### Pseudo-code Poll():
```cpp
for each subject in PendingFramesBySubject:
    - Extract latest frame from queue
    - Track dropped intermediate frames
    - Submit frame to consumer with subject label as subject name
    return count of subjects processed
```

### Audio Handling

#### Sender-Side (No Changes Yet)
- Current `lk_publish_audio_pcm_i16()` sends to single audio track
- StreamLabel parameter in `SubmitPcm()` is documented but not yet used
- **Future**: When LiveKit FFI adds `lk_publish_audio_pcm_i16_ex(label)`, senders can publish to labeled audio tracks

#### Receiver-Side (Limited by LiveKit FFI)
- Current `OnAudioReceived()` callback does not provide per-track label information
- Audio from all sources is received in single stream
- StreamLabel set to `"audio_default"` as placeholder
- **Future**: When LiveKit FFI adds `LkAudioCallbackEx(label, ...)`, receiver can route audio per subject

## Interface Compatibility

### No Breaking Changes to Interfaces
- `IOpen3DSender::Send(SubjectList)` - Signature unchanged, same behavior
- `IOpen3DReceiver::Poll()` - Signature unchanged, return value now reflects per-subject processing
- `ISerializedFrameConsumer::SubmitFrame(SubjectName, Payload, Timestamp)` - No changes

### Configuration Changes
- No changes to `FO3DTransportConfig` or `FO3DTransportAudioConfig`
- Existing configurations continue to work without modification

## Benefits

### Data Channel Improvements
1. **No More Overflow**: Each subject's ~13KB mocap fits safely in reliable channel
2. **Multiple Concurrent Senders**: No data loss when 2+ senders transmit simultaneously
3. **Per-Subject Queuing**: Each subject independently buffered, independent processing
4. **Latency**: Latest frame always used for each subject (consistent with original strategy)

### Audio Improvements (Future)
1. **Audio Separation**: When FFI is updated, each sender's audio goes to separate track
2. **No Crosstalk**: Audio won't collide/distort from multiple speakers
3. **Per-Subject Routing**: Can route audio to correct RemoteAudioComponent by label

## Testing Recommendations

### Unit Tests
- [ ] Multiple senders in single frame (different subject names)
- [ ] Subject name with special characters (sanitization)
- [ ] Payload size boundary testing (lossy/reliable transition)
- [ ] Per-subject queue isolation (one subject dropping frames shouldn't affect others)

### Integration Tests
- [ ] Two O3DSenderComponents publishing simultaneously
- [ ] Audio + mocap from multiple senders
- [ ] LiveLink integration receiving multiple subjects correctly
- [ ] RemoteAudioComponent receiving audio without distortion

### Edge Cases
- [ ] Empty subject names → fallback to "subject_N"
- [ ] Very large number of subjects (>10)
- [ ] Intermittent sender disconnections
- [ ] Receiver reconnection with buffered frames

## Future Enhancements

### 1. Audio Track Labeling (LiveKit FFI Dependent)
```cpp
// When FFI provides this:
LkResult lk_publish_audio_pcm_i16_ex(
    LkClientHandle*,
    const int16_t* pcm, size_t frames,
    int32_t channels, int32_t sample_rate,
    const char* label  // NEW: audio track label
);

LkResult lk_client_set_audio_callback_ex(
    LkClientHandle*,
    LkAudioCallbackEx cb,  // NEW: with label parameter
    void* user
);
```

### 2. Label Validation Utility (Optional)
Could add helper function for sanitizing subject names to valid label format:
```cpp
FString SanitizeSubjectLabel(const FString& SubjectName);
```

### 3. Per-Subject Statistics
Could track separate stats per subject label:
- Frames sent/received per subject
- Bytes per subject
- Latency histogram per subject

## Migration Guide

### For Existing Code
No changes required! The refactoring:
- Maintains all existing interface contracts
- Accepts same configuration format
- Works with existing O3DSenderComponent and RemoteAudioComponent

### For New Features
To take advantage of multiple senders:
1. Create multiple O3DSenderComponent instances (each with different SubjectName)
2. Each will automatically create its own data channel labeled with its SubjectName
3. Receiver automatically routes each label to appropriate LiveLink subject
4. No code changes needed in existing sender/receiver setup

## Files Modified

1. **WebRTCSender.h** (1 line change)
   - No structural changes

2. **WebRTCSender.cpp** (118 lines changed)
   - Send() method refactored for per-subject serialization
   - Uses lk_send_data_ex() with labels

3. **WebRTCReceiver.h** (15 lines changed)
   - Added PendingFramesBySubject map
   - Updated callback signature for extended data callback

4. **WebRTCReceiver.cpp** (182 lines changed)
   - OnDataReceivedEx() replaces OnDataReceived()
   - Poll() refactored for per-subject processing
   - Updated Stop(), ProcessReconnectIfNeeded(), SetupClientHandle()
   - OnAudioReceived() updated with explanatory comments

## Performance Considerations

### Memory
- Slight increase: One TArray per active subject label vs single TArray
- Typical: 1-5 subjects → negligible impact

### CPU
- Per-subject loop in Poll(): O(n) where n = number of active subjects
- Lock granularity: Same as before (single PendingFramesMutex)
- Overall: Negligible impact for <10 concurrent subjects

### Latency
- Same as before: Latest frame always processed
- No additional serialization overhead (actually slightly better: per-subject serialization)

---

**Last Updated**: 2025-11-18
**Status**: Implementation Complete, Ready for Testing
