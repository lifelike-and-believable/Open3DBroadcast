# WebRTC Audio Track Late-Binding Fix

## Summary

This document describes the fix for the WebRTC audio track late-binding issue where audio tracks could be incorrectly added AFTER datachannel creation, violating the critical requirement that audio tracks must be configured before datachannels.

## Problem

### Original Issue
The `bAudioSendEnabled` flag existed to gate whether an audio track should be created. This allowed scenarios where:

1. `SetupPeerConnection()` was called (creating the PeerConnection)
2. Data channel was created
3. `EnableAudioSend()` was called later, adding the audio track AFTER the datachannel

This violated the libdatachannel requirement that **audio tracks must be added BEFORE datachannel creation** to be included in the initial SDP offer.

### Code Duplication
Additionally, the audio track setup logic (RTP/RTCP handler configuration) was duplicated in three locations:
- `SetupPeerConnection()` (~63 lines)
- `EnableAudioSend()` (~71 lines)
- `PushAudioPCM16()` (~65 lines)

Total: ~200 lines of duplicated code.

## Solution

### 1. Removed `bAudioSendEnabled` Flag
Instead of using a boolean flag, audio configuration is now determined by checking if `AudioRt.Config.StreamLabel` is set (non-empty). This provides a more robust way to track audio enablement.

**Before:**
```cpp
bool bAudioSendEnabled = false;  // Member variable
if (bAudioSendEnabled && !AudioTrack) { /* create track */ }
```

**After:**
```cpp
// Audio is enabled if config has a valid StreamLabel
if (!AudioTrack && !AudioRt.Config.StreamLabel.IsEmpty()) { /* create track */ }
```

### 2. Created Helper Function
A new helper function `SetupAudioTrackAndHandlers()` consolidates all audio track setup logic:

```cpp
bool SetupAudioTrackAndHandlers(const FAudioConfig& Config, std::shared_ptr<rtc::PeerConnection> PC)
{
    // Creates audio track (labeled or unlabeled)
    // Sets up RTP packetization config
    // Adds RTCP reporters (SR, NACK)
    // Installs media handler
    // Sets up open/close callbacks
}
```

This reduces ~200 lines of duplication to a single ~80 line function.

### 3. Guaranteed Early Audio Configuration
Audio track is ALWAYS configured in `SetupPeerConnection()` before datachannel creation:

```cpp
bool FWebRTCConnector::SetupPeerConnection()
{
    // ... PeerConnection setup ...
    
    #if O3DS_WITH_OPUS
    // CRITICAL: Add audio track BEFORE creating data channel
    if (!AudioTrack && !AudioRt.Config.StreamLabel.IsEmpty())
    {
        SetupAudioTrackAndHandlers(AudioRt.Config, PeerConnection);
    }
    #endif
    
    // AFTER audio track setup, create datachannel
    if (bNegotiatedChannelEnabled)
    {
        CreateDataChannel();
    }
    
    return true;
}
```

### 4. Simplified `EnableAudioSend()` - Edit-Time Configuration Only
`EnableAudioSend()` is now strictly for configuration storage and **must be called before `Start()`**:
- Stores the audio configuration for use when connection starts
- Ensures Opus encoder is ready
- **If called after `Start()` is called, logs an error** - this is invalid usage
- Audio is edit-time configuration only - never enabled at runtime

```cpp
void EnableAudioSend(const FAudioConfig& InConfig)
{
    // Store audio configuration - must be called BEFORE Start()
    AudioRt.Config = InConfig;
    AudioRt.FrameSizeSamples = FMath::Max(1, (InConfig.SampleRate * InConfig.FrameSizeMs) / 1000);
    
    #if O3DS_WITH_OPUS
    EnsureOpusEncoder(InConfig);
    
    // Audio configuration is edit-time only - must be set before Start()
    if (PeerConnection)
    {
        UE_LOG(Error, TEXT("EnableAudioSend called after Start() - audio must be configured before connection!"));
        LastError = TEXT("EnableAudioSend must be called before Start()");
    }
    #endif
}
```

**Key Principle**: Audio is either enabled or not at edit time. It never becomes enabled after `Start()` is called.

### 5. Removed Late-Binding from `PushAudioPCM16()`
`PushAudioPCM16()` no longer attempts to create audio tracks. If no track exists, it logs a warning and buffers the audio.

```cpp
bool PushAudioPCM16(const int16* Samples, int32 NumSamples)
{
    // Check if audio is configured
    if (AudioRt.Config.StreamLabel.IsEmpty())
    {
        return false;  // Audio not configured
    }
    
    // If no track exists at this point, something went wrong
    if (!LocalAudioTrack)
    {
        UE_LOG(Warning, TEXT("No audio track - was EnableAudioSend called before Start()?"));
        // Buffer audio but don't create track
        return true;
    }
    
    // Normal encoding and sending
    // ...
}
```

## Benefits

### Correctness
✅ Audio tracks are ALWAYS added before datachannel creation
✅ No more late-binding that could violate WebRTC requirements
✅ Deterministic behavior - audio configuration happens at connection start

### Code Quality
✅ ~200 lines of duplication reduced to single helper function
✅ Easier to maintain and update audio track logic
✅ Clearer separation of concerns

### Diagnostics
✅ Clear warnings when audio is enabled late (requiring renegotiation)
✅ Better logging of audio track lifecycle
✅ Status function now derives enabled state from config

## Migration Notes

### For Users
**Important API Change**: `EnableAudioSend()` **must now be called BEFORE `Start()`**. This reflects the reality that audio configuration is edit-time only.

**Correct Usage**:
```cpp
// 1. Create connector
auto Connector = MakeShared<FWebRTCConnector>();

// 2. Configure audio BEFORE starting connection
IWebRTCConnector::FAudioConfig AudioConfig;
AudioConfig.SampleRate = 48000;
AudioConfig.NumChannels = 1;
AudioConfig.BitrateKbps = 32;
AudioConfig.StreamLabel = TEXT("o3ds:mix");
Connector->EnableAudioSend(AudioConfig);

// 3. Start connection
Connector->Start("webrtc://localhost:8080/room", false);
```

**Incorrect Usage** (will fail with error):
```cpp
auto Connector = MakeShared<FWebRTCConnector>();
Connector->Start("webrtc://localhost:8080/room", false);
Connector->EnableAudioSend(AudioConfig);  // ❌ ERROR - too late!
```

### For Developers
If you're working with the WebRTC connector code:

1. **Audio enablement state**: Check `AudioRt.Config.StreamLabel.IsEmpty()` instead of `bAudioSendEnabled`
2. **Adding features**: Use `SetupAudioTrackAndHandlers()` helper to avoid duplication
3. **Testing**: Ensure `EnableAudioSend()` is called before `Start()` in all tests
4. **Edit-time principle**: Audio is never enabled at runtime - only at edit/setup time

## Testing

### Automated Tests
- `O3DSWebRTCAudioTrackOrderingTest` - Validates correct ordering (audio before datachannel)
- `FO3DSWebRTC_AudioSendReceive` - Tests end-to-end audio transmission
- `FO3DSWebRTC_AudioAnnounce` - Tests audio metadata exchange

### Manual Validation
1. Start broadcaster with audio enabled BEFORE connection
2. Check logs for: `"Opus audio track added"` BEFORE any datachannel messages
3. Run console command: `o3ds.WebRTC.Audio.Status`
4. Verify `bAudioSendEnabled=1` and `bAudioTrackPresent=1`

### Regression Prevention
The refactored code makes it impossible to add audio tracks after datachannel creation in the normal flow, as the logic is centralized in `SetupPeerConnection()`.

## References

- **Original audio ordering fix**: WEBRTC_AUDIO_FIX_SUMMARY.md
- **Testing guide**: WEBRTC_AUDIO_FIX_TESTING.md
- **libdatachannel reference**: @lifelike-and-believable/libdatachannel/examples/audio-comm-test

## Commit History

```
ba2e93e Refactor: Remove bAudioSendEnabled flag and consolidate duplicate audio track setup code
09eb565 Update plan with detailed analysis of late-binding and code duplication
f883d25 Initial plan: Remove bAudioSendEnabled flag and ensure audio track is always configured before datachannel
```
