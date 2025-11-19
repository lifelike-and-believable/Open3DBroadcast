# Multi-Track Audio Implementation Plan

## Current Status

### What We Know
From the dev team, the following APIs exist in LiveKit FFI:
- `lk_audio_track_create()` - Creates a dedicated audio track
- `lk_audio_track_publish_pcm_i16()` - Publishes PCM i16 to a specific track
- `lk_audio_track_destroy()` - Cleans up a track

**However:** These APIs are not yet in the current header file you have.

### Current Implementation Status
✅ **Data Channels:** Per-subject labeled channels working perfectly
⚠️ **Audio Tracks:** Single default track only (will be enhanced when APIs available)

## Action Items

### 1. IMMEDIATE: Verify Audio Track APIs with Dev Team

**Ask the devs:**
1. What is the exact function signature for each API?
   ```c
   typedef struct LkAudioTrack LkAudioTrack;

   LkAudioTrack* lk_audio_track_create(LkClientHandle*, ...);
   LkResult lk_audio_track_publish_pcm_i16(LkAudioTrack*, ...);
   LkResult lk_audio_track_destroy(LkAudioTrack*);
   ```

2. What are the full parameter lists for each?
3. When will these be available in the livekit_ffi.h header?
4. What sample rate/channel configurations are supported?
5. Can multiple tracks coexist on a single client connection?

### 2. Update livekit_ffi.h Header

Once you get the signatures from the dev team, add them to:
```
E:\OtherProjects\Open3DStream\plugins\unreal\Open3DStream\ThirdParty\livekit_ffi\include\livekit_ffi.h
```

Expected additions (around line 255):
```cpp
// Audio Track Management
typedef struct LkAudioTrack LkAudioTrack;

LkAudioTrack* lk_audio_track_create(
  LkClientHandle*,
  int32_t sample_rate,
  int32_t channels,
  size_t buffer_size);

LkResult lk_audio_track_publish_pcm_i16(
  LkAudioTrack*,
  const int16_t* pcm_interleaved,
  size_t frames_per_channel,
  int32_t channels,
  int32_t sample_rate);

LkResult lk_audio_track_destroy(LkAudioTrack*);
```

### 3. Refactor WebRTCSender to Use Audio Tracks

**File:** `WebRTCSender.h`

Add to `FO3DWebRTCSender` class:
```cpp
private:
    // Per-subject audio tracks (when lk_audio_track APIs available)
    // Map from StreamLabel to audio track handle
    TMap<FString, LkAudioTrack*> AudioTracks;
    mutable FCriticalSection AudioTracksMutex;
```

**File:** `WebRTCSender.cpp`

Update `FWebRTCSenderAudioSink::SubmitPcm()`:
```cpp
virtual bool SubmitPcm(const FString& StreamLabel, ...) override
{
    // NEW: Use labeled audio track
    LkAudioTrack* Track = GetOrCreateAudioTrack(StreamLabel);
    if (!Track)
    {
        return false;  // Audio track creation failed
    }

    LkResult Result = lk_audio_track_publish_pcm_i16(
        Track,
        PcmConversionBuffer.GetData(),
        NumFrames,
        NumChannels,
        SampleRate
    );

    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Warning,
            TEXT("Failed to publish audio to track '%s': %s"),
            *StreamLabel, *FromAnsi(Result.message));
        return false;
    }

    return true;
}

private:
LkAudioTrack* GetOrCreateAudioTrack(const FString& StreamLabel)
{
    FScopeLock Lock(&Owner.AudioTracksMutex);

    LkAudioTrack** ExistingTrack = Owner.AudioTracks.Find(StreamLabel);
    if (ExistingTrack && *ExistingTrack)
    {
        return *ExistingTrack;
    }

    // Create new track for this stream
    LkAudioTrack* NewTrack = lk_audio_track_create(
        Owner.ClientHandle,
        Owner.ActiveAudioConfig.SampleRate,
        Owner.ActiveAudioConfig.NumChannels,
        48000  // Or configurable buffer size
    );

    if (!NewTrack)
    {
        UE_LOG(LogO3DWebRTCSender, Error,
            TEXT("Failed to create audio track for '%s'"), *StreamLabel);
        return nullptr;
    }

    Owner.AudioTracks.Add(StreamLabel, NewTrack);
    UE_LOG(LogO3DWebRTCSender, Log,
        TEXT("Created audio track for '%s'"), *StreamLabel);

    return NewTrack;
}
```

Add cleanup to `FO3DWebRTCSender::Stop()`:
```cpp
void FO3DWebRTCSender::Stop()
{
    FScopeLock Lock(&StateMutex);

    // Clean up audio tracks first
    {
        FScopeLock AudioLock(&AudioTracksMutex);
        for (auto& TrackEntry : AudioTracks)
        {
            if (TrackEntry.Value)
            {
                lk_audio_track_destroy(TrackEntry.Value);
                TrackEntry.Value = nullptr;
            }
        }
        AudioTracks.Reset();
    }

    // ... rest of Stop() logic
}
```

### 4. Refactor WebRTCReceiver for Per-Track Audio

**Current Status:**
- Receiver uses single `OnAudioReceived()` callback
- All audio mixed together

**With Audio Tracks:**
- Each sender will publish to separate track
- Receiver doesn't need changes (LiveKit FFI handles track separation)
- Audio will naturally route to correct RemoteAudioComponent by mixing

## Expected Behavior After Implementation

### Before (Current)
```
Sender A (John): Mocap ✓ Audio(mono) ✗ (distorted by Sender B)
Sender B (Jane): Mocap ✓ Audio(mono) ✗ (distorted by Sender A)
Receiver: Both animate perfectly, audio is muddy/garbled
```

### After (With Audio Tracks)
```
Sender A (John): Mocap ✓ Audio Track "John" ✓
Sender B (Jane): Mocap ✓ Audio Track "Jane" ✓
Receiver: Both animate perfectly, audio is clean and separated
```

## Implementation Checklist

- [ ] Contact dev team for exact audio track API signatures
- [ ] Update livekit_ffi.h with audio track APIs
- [ ] Update WebRTCSender.h to add AudioTracks map
- [ ] Refactor FWebRTCSenderAudioSink::SubmitPcm() to use tracks
- [ ] Add GetOrCreateAudioTrack() helper method
- [ ] Update FO3DWebRTCSender::Stop() to cleanup tracks
- [ ] Update FO3DWebRTCSender::Initialize() if needed
- [ ] Test: Single audio sender (backward compat)
- [ ] Test: Two audio senders (new functionality)
- [ ] Test: Audio + mocap from same sender
- [ ] Verify audio quality (no distortion, proper isolation)

## Code Changes Estimate

| Component | Lines | Priority |
|-----------|-------|----------|
| livekit_ffi.h | 15-20 | CRITICAL |
| WebRTCSender.h | 5-10 | HIGH |
| WebRTCSender.cpp | 40-60 | HIGH |
| WebRTCReceiver.h | 0 | N/A (no changes needed) |
| WebRTCReceiver.cpp | 0 | N/A (no changes needed) |

**Total Estimated Changes:** 60-90 lines

**Estimated Implementation Time:** 2-4 hours

**Risk Level:** Low (isolated to sender audio path, mocap unaffected)

## Rollback Plan

If issues arise:
1. Revert to single track: Comment out `GetOrCreateAudioTrack()` call
2. Fallback to `lk_publish_audio_pcm_i16()` for all audio
3. No impact to mocap delivery

**Rollback Time:** <15 minutes

## Testing Strategy

### Unit Test 1: Single Audio Sender (Backward Compat)
```
Setup: One sender with audio
Expected: Audio clean, no distortion
```

### Unit Test 2: Two Audio Senders
```
Setup: Two senders, each with audio at different pitches/volumes
Expected: Both audio streams clear and separate, no mixing/distortion
```

### Integration Test: Mixed Scenario
```
Setup:
  - Sender A: Mocap + Audio (speaking)
  - Sender B: Mocap only (listening)
  - Sender C: Mocap + Audio (speaking)
Expected:
  - All three animate correctly
  - Audio from A and C clear and separate
  - No audio artifacts
```

## Next Steps

1. **Get FFI Signatures** - Contact dev team ASAP for exact API signatures
2. **Update Header** - Add APIs to livekit_ffi.h once you have signatures
3. **Implement** - Follow the refactoring plan above
4. **Test** - Verify with multi-sender scenario
5. **Deploy** - Roll out enhanced WebRTC transport

---

**Status:** Awaiting audio track API signatures from dev team
**Blocker:** livekit_ffi.h needs audio track function declarations
**Expected Timeline:** 1-2 days for signatures + 2-4 hours for implementation
