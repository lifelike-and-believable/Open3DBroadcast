# WebRTC Audio Track Fix - Testing Guide

## Overview

This document describes how to test the WebRTC audio track ordering fix implemented to resolve audio negotiation failures.

Note: Examples below using URLs with `?role=` refer to the legacy P2P libdatachannel path. In current builds, backend-specific URL semantics are handled inside connectors. Do not append `role=` or backend hints to URLs when using the unified WebRTC configuration; set backend/role in the UI and provide URL/Room (and Token if required).

## Problem Statement

The WebRTC audio track was failing to open because tracks were not being added before data channel creation, causing them to be omitted from the initial SDP offer.

## Fix Summary

**Key Change**: Audio tracks are now added BEFORE data channel creation in `SetupPeerConnection()`.

**Files Modified**:
- `plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTCConnector.cpp`
- `src/o3ds/webrtc_connector.cpp` (documentation only)

## Testing Prerequisites

1. Unreal Engine 5.6 or compatible version
2. Open3DStream plugin built with `O3DS_WITH_OPUS=ON`
3. libdatachannel with media support
4. A WebRTC signaling server (e.g., examples/signaling-server-python from libdatachannel)
5. Network access between broadcaster and receiver

## Test Setup

### Option A: Two Unreal Instances

**Setup 1: Broadcaster (Client)**
1. Open a project with Open3DStream plugin
2. Create a WebRTC broadcaster component
3. Configure URL: `webrtc://localhost:8080/testroom?role=client`
4. Enable audio send via blueprint or C++:
   ```cpp
   IWebRTCConnector::FAudioSendConfig AudioConfig;
   AudioConfig.bEnable = true;
   AudioConfig.SampleRate = 48000;
   AudioConfig.NumChannels = 1;
   AudioConfig.BitrateKbps = 32;
   AudioConfig.StreamLabel = TEXT("o3ds:mix");
   Connector->EnableAudioSend(AudioConfig);
   ```

**Setup 2: Receiver (Server)**
1. Open another project instance
2. Configure LiveLink source: `webrtc://localhost:8080/testroom?role=server`
3. Monitor received audio

### Option B: Unreal + libdatachannel Example

**Setup 1: Run libdatachannel server**
```bash
cd libdatachannel/build/examples/audio-comm-test
./audio-comm-server
```

**Setup 2: Unreal Broadcaster**
1. Configure as client mode (see Option A, Setup 1)
2. Connect to the test server

## Test Procedure

### Test 1: Initial Connection - Audio in Offer SDP

**Objective**: Verify audio track is included in initial SDP offer

**Steps**:
1. Start signaling server: `python3 signaling-server.py 8080`
2. Start broadcaster with audio enabled BEFORE connecting
3. Start receiver
4. Initiate connection

**Expected Results**:
- [ ] Broadcaster logs show: `"WebRTC Connector: Opus audio track added"`
- [ ] Broadcaster's local SDP contains `m=audio` line
- [ ] Broadcaster's local SDP contains `a=rtpmap:111 opus`
- [ ] No warning: `"Local SDP has no m=audio"`
- [ ] Audio track opens: `"Audio track opened (mid=...)"`
- [ ] No renegotiation required for audio

**Validation**:
```
# Check logs for correct order
grep -A5 "Opus audio track added" YourProject/Saved/Logs/YourProject.log
grep "Local SDP" YourProject/Saved/Logs/YourProject.log
```

### Test 2: Audio Track Opens Successfully

**Objective**: Verify audio track negotiates and opens

**Steps**:
1. Complete Test 1 setup
2. Wait for connection to establish
3. Push audio samples via `PushAudioPCM16()` or `PushPcm()`

**Expected Results**:
- [ ] Log shows: `"Audio track opened (mid=0)"` or similar
- [ ] No error: `"sendFrame threw exception"`
- [ ] Audio packets increment: Check `SentPackets` and `SentBytes` counters
- [ ] No repeated reoffer attempts after initial connection

**Validation**:
```
# Run console command in Unreal
o3ds.WebRTC.Audio.Status

# Expected output (sample):
# Signaling=1 PeerConnected=1 LocalDesc=1 RemoteDesc=1 DataChannelOpen=1
# LocalSDP.m=audio=1 RemoteSDP.m=audio=1
# AudioEnabled=1 TrackPresent=1 TrackOpen=1 OpusReady=1
# SentPackets=250 SentBytes=48000
```

### Test 3: Data and Audio Coexist

**Objective**: Verify both data channel and audio track work simultaneously

**Steps**:
1. Complete Test 2 setup
2. Send animation data via data channel
3. Send audio via audio track
4. Monitor both streams on receiver

**Expected Results**:
- [ ] Data channel remains open
- [ ] Audio track remains open
- [ ] Animation data is received correctly
- [ ] Audio data is received correctly
- [ ] No interference between streams

### Test 4: Negotiated Channel Mode

**Objective**: Verify fix works with negotiated data channels

**Steps**:
1. Enable negotiated channel: `o3ds.Broadcast.WebRTC.NegoChannel 1`
2. Repeat Test 1-3

**Expected Results**:
- [ ] Same as Test 1-3, but with negotiated channel
- [ ] Audio track still added before data channel
- [ ] Both negotiate successfully

### Test 5: Late Audio Enable

**Objective**: Verify audio can be enabled after initial connection

**Steps**:
1. Start connection WITHOUT audio enabled
2. Wait for connection to establish
3. Enable audio: `Connector->EnableAudioSend(AudioConfig)`

**Expected Results**:
- [ ] Log shows: `"Audio Track Added"`
- [ ] Renegotiation occurs (expected)
- [ ] New offer includes `m=audio`
- [ ] Audio track opens after renegotiation
- [ ] Existing data channel remains functional

## Debugging Failed Tests

### Audio Track Not in SDP Offer

**Symptom**: No `m=audio` line in local SDP

**Possible Causes**:
1. Audio not enabled before `SetupPeerConnection()`
2. `O3DS_WITH_OPUS` not defined
3. OpusEncoder creation failed

**Debug Steps**:
1. Check `bAudioSendEnabled` flag
2. Verify Opus library is linked
3. Check for encoder creation errors

### Audio Track Not Opening

**Symptom**: `bAudioTrackOpen=0` persists after connection

**Possible Causes**:
1. Remote peer doesn't support audio
2. Audio direction mismatch (sendonly vs recvonly)
3. Codec negotiation failed

**Debug Steps**:
1. Check remote SDP for `m=audio`
2. Verify both sides have compatible codecs
3. Check audio direction attributes in SDP

### Audio Send Exceptions

**Symptom**: `"sendFrame threw exception"` in logs

**Possible Causes**:
1. Track not fully open yet
2. Network congestion
3. Invalid audio data

**Debug Steps**:
1. Check `AudioRt.bTrackReady` flag
2. Verify RTP packetizer is set up correctly
3. Validate PCM data format

## Success Criteria

A successful test run should show:

✅ Audio track in initial SDP offer (no renegotiation)
✅ Audio track opens within 2-3 seconds
✅ Audio packets transmitted continuously
✅ Data channel and audio track coexist
✅ No exceptions during audio send
✅ Both negotiated and non-negotiated modes work

## Reference

**libdatachannel Test Code**:
- `examples/audio-comm-test/client.cpp` - Reference implementation
- `examples/audio-comm-test/README.md` - Test documentation

**Key Finding from Tests**:
> Audio track must be added BEFORE creating the data channel to be included in the initial offer

## Console Commands

Useful Unreal console commands for testing:

```
# Enable verbose WebRTC logging
o3ds.WebRTC.Verbose 1

# Enable audio debug logging
o3ds.WebRTC.Audio.Debug 1

# Check audio send status
o3ds.WebRTC.Audio.Status

# Enable debug receive logging
o3ds.WebRTC.DebugRx 1

# Force SendRecv mode (if needed for testing)
o3ds.Broadcast.WebRTC.Audio.ForceSendRecv 1
```

## Reporting Issues

If tests fail after this fix, please report:

1. Which test failed
2. Relevant log excerpts showing:
   - PeerConnection setup order
   - SDP offer/answer content
   - Audio track state changes
3. Console command output: `o3ds.WebRTC.Audio.Status`
4. Network topology (client/server roles)
5. Unreal Engine version
6. libdatachannel version
