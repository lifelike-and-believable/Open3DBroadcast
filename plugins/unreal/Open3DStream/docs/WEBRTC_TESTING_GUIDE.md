# WebRTC End-to-End Testing Guide

This guide provides step-by-step instructions for testing the WebRTC receiver and broadcaster functionality in Open3DStream.

## Prerequisites

- Unreal Engine 5.6 with Open3DStream plugin installed
- Python 3.x for the signaling server
- Two test scenarios available:
  1. **Two Editor Instances** (recommended for full end-to-end validation)
  2. **Single Editor with Components** (quick connector validation)

---

## Connection Order and Resilience

The receiver and broadcaster now include reconnect logic:

- Client-first is supported. If the client starts before the receiver, it will retry offers with backoff until the receiver appears.
- Receiver survives signaling restarts and Live Link source recreation; it resets the PeerConnection and re-offers as needed.
- If the DataChannel closes while signaling remains connected, the client will re-offer.

Recommended order (for faster connects): signaling → receiver → broadcaster. Not strictly required.

---

## Test Setup 1: Two Editor Instances (Full End-to-End)

### Step 1: Start the Signaling Server

```powershell
cd e:\OtherProjects\Open3DStream\plugins\unreal\Open3DStream\examples\signaling
python signaling-server.py
```

**Expected output:**
```
WebSocket signaling server listening on ws://127.0.0.1:8080
```

Leave this terminal running.

---

### Step 2: Launch Receiver Editor (Server Role)

**Important:** Start the receiver **before** the broadcaster.

1. Open ProjectSandbox in a **first UE Editor instance**
2. Open **Window → Live Link**
3. Click **+ Source** → **Open3D Stream**
4. Configure Live Link Source:
   - **Protocol**: `WebRTC Server`
   - **URL**: `ws://127.0.0.1:8080`
   - **WebRTC Room**: `test-room`
   - **Enable WebRTC Audio**: ✓ (checked)
   - **WebRTC Audio Playout Delay Ms**: `0` (for testing; increase for resilience)
5. Click **Create**

**Enable verbose logging (recommended):**
Open the Output Log console and run:
```
o3ds.Receiver.WebRTC.Log 1
o3ds.Receiver.Audio.Log 1
o3ds.Receiver.DebugParse 1
```

**Expected logs:**
```
O3DS RX: LiveLink OnData bound in Open3DStreamSource
O3DS WebRTC Receiver: started (audio=enabled)
```

The receiver is now **waiting for a client to connect**.

---

### Step 3: Add Audio Playback Component (Receiver)

To hear the audio stream:

1. In the receiver editor's level, add a new actor (or use an existing one)
2. Add **O3DSRemoteAudioComponent** (now a SceneComponent so it can be attached):
    - Top section:
       - **Receive Mode**: `Mix` (receives all audio) or `Subject`
       - **LiveLink Subject Name**: set when using `Subject`
       - **Stream Label Filter**: optional substring filter
       - **Gain**: `1.0` (source-side scaling before enqueue)
       - **Volume Multiplier** / **Pitch Multiplier**: component-level mix controls
    - Attachment:
       - **Attach Parent**: optional parent scene component (e.g., SkeletalMeshComponent)
       - **Attach Socket Name**: optional socket on parent
    - Attenuation:
       - **Allow Spatialization**: enable for 3D placement
       - **Override Attenuation**: when enabled, **Attenuation Overrides** become editable; otherwise use **Attenuation Settings** asset
    - Routing & processing:
       - **Submix Sends**: route to one or more submixes
       - **Source Effect Chain**: optional source effects chain
       - **Concurrency Set/Overrides**: concurrency behavior
    - Activation:
       - **Auto Activate**: ✓ (default; auto-plays when the procedural wave is ready)
3. (Optional) Enable audio debug logging:
   ```
   o3ds.RemoteAudio.Debug 1
   ```

---

### Step 4: Launch Broadcaster Editor (Client Role)

**Important:** Start the broadcaster **after** the receiver is running.

1. Open ProjectSandbox in a **second UE Editor instance**
2. Create or open a test level with an animated character
3. Add **O3DSBroadcastComponent** to an actor with a skeletal mesh:
   - **Transport**: `WebRTC`
   - **Signaling URL**: `ws://127.0.0.1:8080`
   - **WebRTC Role**: `Client`
   - **WebRTC Room**: `test-room` (must match receiver)
   - **Subject**: Select the skeletal mesh component to stream
   - **Capture Mode**: Choose audio source:
     - `Input` - microphone
     - `Mix` - game audio/submix
     - `Disabled` - animation only
   - **Enable Audio**: ✓ (if using Input or Mix mode)

**Enable verbose logging (recommended):**
```
o3ds.Broadcast.DebugSend 1
o3ds.Broadcast.Audio.Log 1
```

4. Hit **Play (PIE)**

**Expected logs (broadcaster):**
```
O3DS Broadcast: Starting WebRTC client
[LibDataChannelConnector] PeerConnection created
[LibDataChannelConnector] DataChannel open
[LibDataChannelConnector] Audio track open
O3DS Broadcast Audio: Starting capture (mode=Mix/Input)
```

**Expected logs (receiver, within 2-10 seconds):**
```
O3DS WebRTC Receiver: state=connected error=0
O3DS WebRTC Receiver: data bytes=<N>
O3DS Receiver: Parse OK subjects=1 time=<timestamp>
O3DS Receiver: Created subject '<SkeletonName>'
O3DS Opus Decoder: decoded <N> samples (payload=<N> bytes)
O3DS Opus Decoder: published <N> samples (<N> bytes) to Audio Bus
```

---

### Step 5: Verify Data Flow

#### Live Link Animation (Mocap)

1. In the **receiver editor**, open **Window → Live Link**
2. You should see a subject named after the sender's skeleton
3. **Green indicator** = actively receiving frames
4. Click on the subject to see:
   - Bone transform values updating in real-time
   - Frame timestamp incrementing
   - Subject properties (bone count, curves)

#### Audio Playback

With the sender's audio enabled (microphone or game audio):

1. You should **hear audio** playing in the receiver editor (with O3DSRemoteAudioComponent added)
2. Check receiver logs for audio activity:
   ```
   [Opus Decoder] decoded 960 samples (payload=93 bytes, ts=<N>)
   O3DS RemoteAudio: Queued frames=1 ch=1 sr=48000
   ```

3. **Packet size variation** (indicates audio encoding is working):
   - **Silence**: ~15 bytes (3-byte Opus DTX + 12-byte RTP header)
   - **Speech/Music**: ~60-120 bytes (variable bitrate Opus encoding)

---

### Step 6: Test Scenarios

#### A. Animation Only
1. On sender, set **Capture Mode** = `Disabled` or **Enable Audio** = unchecked
2. Verify Live Link subjects still update in receiver
3. Logs should show DataChannel traffic, no audio RTP

#### B. Audio Only
1. On sender, unbind skeleton/subject (or use actor without mesh)
2. Enable audio with **Capture Mode** = `Mix` or `Input`
3. Play music or speak into microphone
4. Verify audio playback in receiver
5. Check packet size variation (15 bytes → 80-120 bytes)

#### C. Mixed Content (Animation + Audio)
1. Enable both skeleton streaming and audio
2. Verify both streams flow concurrently
3. Check Live Link subjects update **and** audio plays
4. No interference between data/audio channels

#### D. Debug Tone Test (Audio Pipeline Validation)
1. On sender, set CVar:
   ```
   o3ds.Broadcast.WebRTC.DebugTone 1
   ```
2. Should hear a **440 Hz sine wave** in receiver for configured duration
3. Packet sizes should be consistent ~90-100 bytes during tone
4. Useful for validating audio pipeline without microphone

---

### Step 7: Monitor Performance

**Sender Logs (with `o3ds.Broadcast.DebugSend 1`):**
```
O3DS Broadcast: Sent frame size=<N> bytes subjects=1
[LibDC] DataChannel send: <N> bytes
[LibDC] Audio: Opus encoded <N> samples → <N> bytes (pt=111)
```

**Receiver Logs (with verbose logging):**
```
O3DS WebRTC Receiver: data bytes=<N>
O3DS Receiver: packet bytes=<N> tcp_magic=false first_64=<hex dump>
O3DS Receiver: Parse OK subjects=1 time=<timestamp>
[Opus Decoder] RTP packet: PT=111 seq=<N> payload=<N> bytes
```

**Expected Metrics:**
- **Latency**: 50-150 ms end-to-end (network + decode + render)
- **DataChannel throughput**: ~10-50 KB/s for 1 character @ 60 fps
- **Audio packet rate**: ~50 packets/sec (20 ms frames @ 48 kHz)
- **Audio packet size**: 15 bytes (silence), 60-120 bytes (active audio)
- **CPU (sender)**: < 5% for 1 character + audio encode
- **CPU (receiver)**: < 3% for decode + playback

---

### Step 8: Clean Teardown

1. **Stop sender (broadcaster):**
   - Stop PIE in broadcaster editor
   - Expected logs:
     ```
     O3DS Broadcast: Stopping
     [LibDataChannelConnector] Stopped
     ```
   - No crashes or hangs

2. **Remove receiver (Live Link source):**
   - In receiver editor, open Live Link
   - Right-click on Open3DStream source → Remove
   - Expected logs:
     ```
     O3DS WebRTC Receiver: stopped
     O3DS: Stopped
     ```
   - No errors or exceptions

3. **Stop signaling server:**
   - In signaling terminal: `Ctrl+C`

---

## Test Setup 2: Single Editor with Components (Quick Validation)

This setup validates the connector behavior without full Live Link integration.

### Step 1: Start Signaling Server

```powershell
cd e:\OtherProjects\Open3DStream\plugins\unreal\Open3DStream\examples\signaling
python signaling-server.py
```

### Step 2: Single Editor Setup

1. Open ProjectSandbox in UE Editor
2. Create two actors in the level

**Actor A (Server):**
- Add **O3DSWebRTCConnectorComponent**:
  - **Server Mode**: ✓ (checked)
  - **Signaling URL**: `ws://127.0.0.1:8080`
  - **Room**: `test-room`
  - **Enable Audio**: ✓ (checked)

**Actor B (Client):**
- Add **O3DSWebRTCConnectorComponent**:
  - **Server Mode**: ☐ (unchecked - client mode)
  - **Signaling URL**: `ws://127.0.0.1:8080`
  - **Room**: `test-room`
  - **Enable Audio**: ✓ (checked)
  - **Send Debug Tone**: ✓ (checked - for easy audio validation)

### Step 3: Run and Verify

1. Hit **Play (PIE)**
   - **Note:** Both components start simultaneously in this setup; server component initializes slightly earlier due to registration order
2. Check Output Log for both components:
   ```
   [WebRTCConnectorComponent] Starting as server
   [WebRTCConnectorComponent] Starting as client
   [WebRTCConnectorComponent] DataChannel open
   [WebRTCConnectorComponent] Audio track open
   [WebRTCConnectorComponent] Received RTP: 93 bytes (from tone)
   ```
3. Verify DataChannel bidirectional flow (both sides log received data)
4. Verify audio RTP received on both sides

**Limitation:** This validates connector behavior only. It does **not** test Live Link integration, animation retargeting, or Audio Bus routing. Use Setup 1 for full end-to-end validation.

---

## Troubleshooting

### No DataChannel Open

**Symptoms:**
- Logs show "Starting..." but no "DataChannel open"
- Timeout after 30+ seconds

**Solutions:**
1. Verify signaling server is running on `ws://127.0.0.1:8080`
2. Check room names match **exactly** (case-sensitive)
3. Ensure **server started before client** (critical for MVP)
4. Check firewall isn't blocking localhost:8080
5. Enable verbose logging:
   ```
   o3ds.Broadcast.DebugSend 1
   o3ds.Receiver.WebRTC.Log 1
   ```
6. Restart in correct order: signaling → receiver → broadcaster

---

### DataChannel Opens, No Data Received

**Symptoms:**
- "DataChannel open" logged on both sides
- No "data bytes=<N>" or "Parse OK" logs on receiver

**Solutions (Sender):**
1. Verify subject/skeleton is bound in broadcast component
2. Check actor has a skeletal mesh component
3. Ensure skeletal mesh is animating (play animation or use ControlRig)
4. Enable `o3ds.Broadcast.DebugSend 1` - should see "Sent frame size=<N>"

**Solutions (Receiver):**
1. Enable `o3ds.Receiver.DebugParse 1` to see parse errors
2. Check for FlatBuffers parse failures in logs
3. Verify protocol is set to "WebRTC" (not TCP/UDP/NNG)

---

### Audio Not Playing

**Symptoms:**
- DataChannel works, Live Link subjects update
- No audio heard in receiver editor
- Logs show only 15-byte packets (silence)

**Solutions (Sender):**
1. Check audio **Capture Mode**:
   - `Input`: Requires microphone, may need permission prompt
   - `Mix`: Requires game audio (play music, use AudioComponent)
   - `Disabled`: No audio sent (by design)
2. Verify **Enable Audio** is checked
3. Test with debug tone first:
   ```
   o3ds.Broadcast.WebRTC.DebugTone 1
   ```
4. Check `o3ds.Broadcast.Audio.Log 1` for capture errors
5. For `Input` mode: speak/make noise into microphone
6. For `Mix` mode: play music in UE (add AudioComponent with SoundWave)

**Solutions (Receiver):**
1. Verify **O3DSRemoteAudioComponent** is added and attached where you expect
2. Check component settings:
   - **Receive Mode** = `Mix` (receives all audio) or a valid **Subject**
   - **Auto Activate** = true (or call Play() manually if false)
   - **Gain** > 0 and **Volume Multiplier** > 0
3. Check routing/processing:
   - If using Submix Sends, confirm the target submix is audible
   - If using Source Effects Chain, disable temporarily to test
4. Enable audio debug:
   ```
   o3ds.RemoteAudio.Debug 1
   o3ds.Receiver.Audio.Log 1
   ```
4. Check logs for audio frames:
   ```
   [Opus Decoder] decoded <N> samples
   O3DS RemoteAudio: Queued frames=<N>
   ```

---

### Only 15-Byte Packets (Constant Silence)

**Symptoms:**
- All audio packets are exactly 15 bytes
- No variation with audio content

**Root Cause:**
Opus encoder is receiving silent input (all zeros).

**Solutions:**
1. **Microphone mode (`Input`):**
   - Check microphone is not muted in Windows
   - Grant microphone permission to UE if prompted
   - Speak/make noise while monitoring packet size
   - Try different microphone in Windows settings

2. **Mix mode (`Mix`):**
   - Ensure game audio is actually playing
   - Add AudioComponent with SoundWave to level
   - Set SoundWave to loop, auto-activate
   - Check mixer submix is not muted

3. **Quick validation:**
   Use debug tone to confirm pipeline works:
   ```
   o3ds.Broadcast.WebRTC.DebugTone 1
   ```
   Packets should jump to ~90-100 bytes during tone

---

### Live Link Subject Not Appearing

**Symptoms:**
- DataChannel connected
- Receiver logs show "data bytes=<N>"
- No subject visible in Live Link panel

**Solutions:**
1. Verify receiver protocol is `WebRTC Server` (not Client)
2. Check for parse errors:
   ```
   o3ds.Receiver.DebugParse 1
   ```
   Should see "Parse OK subjects=1"
3. Verify sender is streaming a valid skeleton (not just empty actor)
4. Check sender logs for "Sent frame size=<N> subjects=1"
5. Try removing and re-adding the Live Link source

---

### Connection Fails After Restart

**Symptoms:**
- Worked once, fails on subsequent attempts
- "Timeout" or no connection after 30 seconds

**Solutions:**
1. **Stop all components** before restarting:
   - Stop PIE on sender
   - Remove Live Link source on receiver
   - Wait 5 seconds for cleanup
2. **Restart in correct order:** signaling → receiver → broadcaster
3. If still failing:
   - Restart both UE editors
   - Restart signaling server
   - Check for port conflicts (another app on 8080)

---

## CVars Reference

### Receiver (Live Link Source)

```cpp
// WebRTC receiver adapter
o3ds.Receiver.WebRTC.Log 0/1          // Enable receiver adapter logging

// Opus audio decoder
o3ds.Receiver.Audio.Log 0/1           // Enable Opus decoder logging

// Packet parsing (FlatBuffers)
o3ds.Receiver.DebugParse 0/1          // Enable packet parse diagnostics
```

### Broadcaster (O3DSBroadcastComponent)

```cpp
// Frame send debugging
o3ds.Broadcast.DebugSend 0/1          // Log every frame sent

// Audio capture/encode
o3ds.Broadcast.Audio.Log 0/1          // Log audio capture/encode details

// Debug tone generator
o3ds.Broadcast.WebRTC.DebugTone 0/1   // Send 440 Hz test tone
```

### Audio Playback (O3DSRemoteAudioComponent)

```cpp
// Remote audio component
o3ds.RemoteAudio.Debug 0/1            // Log audio bus events and playback
```

---

## Known Limitations

These are the current limits/caveats:

1. **No jitter buffer:**
   - Audio decoded per-packet (no reordering)
   - May cause rare artifacts on lossy/reordered networks
   - Future: add jitter buffer for production deployments

2. **48 kHz audio only:**
   - Non-48kHz input is dropped with verbose log
   - Future: add resampler for arbitrary sample rates

3. **No persistent signaling:**
   - Sample signaling server doesn't queue messages
   - Clients/servers must overlap connection window
   - Future: upgrade to production signaling with persistence

4. **Single room per source:**
   - One Live Link source = one room
   - Multiple sources require multiple Live Link sources
   - Future: multi-room receiver or dynamic room switching

5. **Modulation destinations (UE 5.6):**
   - Volume/Pitch modulation properties are exposed on the component but may not be wired by default on 5.6 API surface.
   - Future: version-aware wiring when engine exposes destinations on the component or sound asset.

---

## Performance Baselines

Expected resource usage for reference:

| Metric | Expected Value | Notes |
|--------|---------------|-------|
| **End-to-End Latency** | 50-150 ms | Network + encode/decode + render |
| **DataChannel Throughput** | 10-50 KB/s | 1 character @ 60 fps |
| **Audio Packet Rate** | ~50 packets/sec | 20 ms frames @ 48 kHz |
| **Audio Packet Size (Silence)** | 15 bytes | Opus DTX (3 bytes) + RTP (12 bytes) |
| **Audio Packet Size (Active)** | 60-120 bytes | Opus VBR based on content complexity |
| **CPU (Sender)** | < 5% | 1 character + audio encode |
| **CPU (Receiver)** | < 3% | Decode + playback |
| **Memory (Sender)** | < 50 MB | Audio buffers + encode state |
| **Memory (Receiver)** | < 30 MB | Decode buffers + Audio Bus |

---

## Next Steps

After successful validation:

1. **Production signaling:** Deploy persistent signaling server (e.g., Node.js + Redis)
2. **Reconnect logic:** Implement exponential backoff in connector
3. **Jitter buffer:** Add audio frame reordering for lossy networks
4. **Multi-room:** Support dynamic room switching in receiver
5. **Metrics dashboard:** Expose connection health (latency, drops, bitrate) in UI

---

## Additional Resources

- **WebRTC Receiver Plan:** `docs/webrtc-receiver-plan.md`
- **Connector Refactor:** `docs/WEBRTC_CONNECTOR_REFACTOR_PLAN_ISSUE134.md`
- **Plugin README:** `README.md`
- **Troubleshooting:** `docs/TROUBLESHOOTING.md` (general plugin issues)
- **GitHub Issues:** https://github.com/lifelike-and-believable/Open3DStream/issues

---

**Happy testing! 🎉**
