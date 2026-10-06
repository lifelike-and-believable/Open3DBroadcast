# Open3DBroadcast WebRTC add-on - User Guide

How to install, configure, use and troubleshoot the WebRTC (LiveKit) transport for Open3DBroadcast. The transport is the free **Open3DBroadcastWebRTC** add-on plugin; it is not part of Open3DBroadcast itself.

## Table of Contents

1. [Requirements and Installation](#requirements-and-installation)
2. [Quick Start](#quick-start)
3. [Configuration Guide](#configuration-guide)
   - [Transport Options](#transport-options)
   - [Credentials](#credentials)
   - [Automatic Token Fetch](#automatic-token-fetch)
4. [Audio Configuration](#audio-configuration)
5. [Control Channel](#control-channel)
6. [Platform Support](#platform-support)
7. [Troubleshooting](#troubleshooting)
8. [Performance Tuning](#performance-tuning)
9. [FAQ](#faq)

---

## Requirements and Installation

- **Open3DBroadcast:** required, installed from Fab or from a GitHub release. The add-on must be the build made for your Open3DBroadcast release (see [Version matching](#version-matching)).
- **Unreal Engine:** 5.7. **Platform:** Win64 only, for editor and game targets. Server and Program targets are not supported.
- **Status:** Beta, like Open3DBroadcast.
- **Download:** **[DOWNLOAD LINK PLACEHOLDER: support-site URL for Open3DBroadcastWebRTC, to be added before release]**

### Installing

1. Install Open3DBroadcast first (from Fab, or by copying it into your project's `Plugins/` folder).
2. Copy the `Open3DBroadcastWebRTC` folder into your **project's** `Plugins/` folder. Do not copy anything into the Open3DBroadcast folder: the add-on is a plugin of its own, so a Fab update of Open3DBroadcast leaves it in place.
3. Open the project, go to **Edit → Plugins**, enable **Open3DBroadcast WebRTC** (Beta) and restart the editor.
4. "WebRTC" now appears in the transport list of the Open3D sender component and of the **Open3DStream Receiver** LiveLink source.

The add-on contains the WebRTC transport module (`Open3DTransportWebRTC`) and its LiveKit client library (`livekit_ffi.dll`). Its settings panel is drawn by Open3DBroadcast's editor module from the options the transport declares, so it looks and behaves like every other transport panel.

### Version matching

The add-on links against Open3DBroadcast's C++ transport interface, which carries a version number (`O3D_TRANSPORT_API_VERSION`). At startup the add-on compares the number it was built with against the installed Open3DBroadcast. If they differ it registers nothing, and the Output Log shows one error:

```
LogO3DWebRTCSender: Error: WebRTC transport not registered: Open3DBroadcastWebRTC was built for Open3DBroadcast transport API version 1, but the loaded Open3DBroadcast provides version 2. Open3DBroadcastWebRTC registers nothing. Install the Open3DBroadcastWebRTC build made for this Open3DBroadcast release.
```

Download the add-on build that matches your Open3DBroadcast version. Every other transport keeps working in the meantime. Updating both keeps your WebRTC settings: they are saved as the `webrtc.*` options of the sender or LiveLink source, and those did not change.

### Removing the add-on

Disable **Open3DBroadcast WebRTC** in **Edit → Plugins** (or delete its folder) and restart. Open3DBroadcast and its other transports keep working. Senders and LiveLink sources that were set to WebRTC keep their settings and report that the transport is not registered until you pick another transport or reinstall the add-on.

---

## Quick Start

The WebRTC transport streams motion capture data, audio and control through a LiveKit server, so senders and receivers on different networks can share one room.

### Basic Setup

1. **Get a LiveKit server.** Use a hosted service or run your own. Its URL looks like `wss://your-server.com`; a local development server is usually `ws://127.0.0.1:7880`.

2. **Get access tokens.** A token is a JWT that names the room, the participant identity and the participant's permissions. Create one for the sender and one for the receiver (see [Access Token](#access-token-jwt)), or let the plugin fetch them ([Automatic Token Fetch](#automatic-token-fetch)).

3. **Configure the transport** on the sender component and on the LiveLink source:
   ```
   LiveKit Host: wss://your-server.com
   Access Token: <token from step 2>
   ```
   The token is a credential. It is kept out of the level, the Blueprint, the ini files and LiveLink presets; see [Credentials](#credentials).

4. **Audio (optional).** On the sender component, tick **Enable Audio** and set the audio options (see [Audio Configuration](#audio-configuration)). On the LiveLink source, tick **Enable Audio** to play what arrives.

---

## Configuration Guide

### Server URL

**LiveKit Host** (`webrtc.url`) is the server's WebSocket URL:
- `wss://livekit.example.com` or `wss://livekit.example.com:7880`: an encrypted connection.
- `ws://127.0.0.1:7880`: an unencrypted connection, for a local development server.
- Without a scheme, the transport adds one: `ws://` for `127.0.0.1`, `0.0.0.0`, `localhost` and `[::1]` (with or without a port), and `wss://` for every other host.
- An explicit `ws://` or `wss://` is kept as written. Do not use `https://`: the transport would add `wss://` in front of it.

### Transport Options

The panel of the sender component and of the LiveLink source shows the same options for WebRTC. Options without a **Shown as** entry are not in the panel. Set them with **Set Transport Option** on the sender component, or in the options map of **Create Open3DStream LiveLink Source**.

| Key | Shown as | Default | Notes |
|---|---|---|---|
| `webrtc.url` | **LiveKit Host** | none | Required. See [Server URL](#server-url). |
| `webrtc.useAutoTokenFetch` | **Use Auto Token Fetch** | false | Fetch tokens from your token endpoint instead of using **Access Token**. |
| `webrtc.credentialProfile` | **Credential Profile** | `default` | Which stored credentials to use. Saved with the component or source. |
| `webrtc.token` | **Access Token** | none | Secret. Shown when Auto Token Fetch is off. Environment variable `O3DB_WEBRTC_TOKEN`. |
| `webrtc.tokenEndpointUrl` | **Token Endpoint URL** | none | Shown when Auto Token Fetch is on. Plain `http://` only for `localhost`, `127.0.0.1` and `::1`. |
| `webrtc.tokenEndpointAuth` | **Token Endpoint Credential** | none | Secret. Shown when Auto Token Fetch is on. Sent as `Authorization: Bearer <value>`. Environment variable `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH`. |
| `webrtc.room` | **Room** | none | Shown when Auto Token Fetch is on, and required then. Use the same room on the sender and the receiver. |
| `webrtc.tokenRefreshLeadTimeSec` | **Token Refresh Lead Time (seconds)** | 300 | Shown when Auto Token Fetch is on. Seconds before expiry to fetch the next token. The panel takes 60 to 3600. |
| `webrtc.prefer_lossy` | | false | Sender only. Send frames of up to 1300 bytes on LiveKit's lossy data channel. See [Frames Being Dropped](#frames-being-dropped). |
| `webrtc.reconnect_timeout` | | 2 | Receiver only. Seconds without data before the receiver reconnects; 0 turns it off. Clamped to 0 to 300. |

Secrets are never saved with the component or source; see [Credentials](#credentials).

### Access Token (JWT)

A LiveKit access token authenticates a participant to the server. It names the room and the identity, and grants permissions such as publishing and subscribing.

**Creating tokens**
- Use the LiveKit CLI, a LiveKit server SDK, or your LiveKit provider's console. See the LiveKit documentation: https://docs.livekit.io
- With the LiveKit CLI (`lk`), for example:
  ```bash
  lk token create --api-key <api-key> --api-secret <api-secret> --join --room MyRoom --identity Sender1 --valid-for 1h
  ```
  Give the sender and the receiver different identities.

**Token expiration**
- A token stops working when it expires; the server sets or accepts its lifetime.
- With a manual token, create a new one and set it before the old one expires. Automatic token fetch refreshes tokens for you.

### Credentials

The access token (`webrtc.token`) and the token endpoint credential (`webrtc.tokenEndpointAuth`) are secret options. The plugin never writes them to a level, a Blueprint, `GameUserSettings.ini`, a LiveLink connection string or preset, or a log.

**Where the value comes from**, in this order:
1. **This session:** the value typed into the password field in the editor, or set at runtime with the Blueprint node **Set Transport Secret** (`UO3DCredentialLibrary::SetTransportSecret`). It is gone after a restart.
2. **Environment variable:** `O3DB_WEBRTC_TOKEN` for the token, `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH` for the endpoint credential. With a credential profile other than `default`, `<NAME>__<PROFILE>` is tried first (for profile `stage`: `O3DB_WEBRTC_TOKEN__STAGE`). This works in packaged games and on render nodes.
3. **Remembered on this machine** (editor only): tick **Remember on this machine** next to the field. The value is stored in your per-user editor settings under `Saved/Config` (Base64-encoded, not encrypted), never in the project's `Config/` folder.

**In the editor panel**, each secret field opens empty. The line below it says where the current value comes from: "Not set", "Set for this session", "Remembered on this machine" or "From environment variable ...". **Clear** forgets the session value and the remembered copy; an environment variable still applies. With Auto Token Fetch off, a warning appears when no token is available.

**Credential Profile** (`webrtc.credentialProfile`, default `default`) is saved with the component or source. It names which stored credentials to use, so two senders can use different tokens without either token being saved.

**Upgrading older projects:** a level, Blueprint, `GameUserSettings.ini` or LiveLink preset saved by an older version may still contain a token. When it is loaded, the token is moved into this session's store and removed from the loaded data, and one warning names the asset or source (never the value). Resave the asset, or recreate the LiveLink source, so the token is removed from disk.

### Automatic Token Fetch

The plugin can fetch LiveKit tokens from your own token endpoint instead of using a token you paste in. It fetches a new token before the current one expires, and your LiveKit API secret stays on your server.

**Setup:**

1. **Deploy a token endpoint**
   - A service of yours that creates and signs LiveKit tokens.
   - It accepts POST requests, for example at `https://your-server.com/token`.
   - `docs/dev/Open3DTransportWebRTC/Tests/mock-token-server.py` in the Open3DBroadcast repository is a reference implementation for local testing.

2. **Configure in the Unreal Editor**
   - Select your Open3D sender component, or open the LiveLink source settings.
   - Choose "WebRTC" as the transport.
   - Tick **Use Auto Token Fetch**.
   - Enter the **Token Endpoint URL**: `https://your-server.com/token`, or set it once for the whole project as `webrtc.tokenEndpointUrl` in **Project Settings > Plugins > Open3DBroadcast** (Sender Defaults and Receiver Defaults, transport `webrtc`); a component or source that leaves it empty uses that value. The endpoint credential is never set there.
   - Enter the **Token Endpoint Credential** your endpoint expects, or set `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH` (see [Credentials](#credentials)).
   - Set **Room** to the same value on the sender and the receiver.
   - **Token Refresh Lead Time (seconds)** defaults to 300.

3. **How it works**
   - On start, the transport requests a token from your endpoint.
   - The request carries the room, the identity, the role (`publisher` for a sender, `subscriber` for a receiver), and `Authorization: Bearer <endpoint credential>` when one is set.
   - The request carries no grants; your endpoint decides them.
   - Your endpoint creates and signs the token with your LiveKit API key and secret.
   - The transport fetches the next token the lead time before the current one expires.

**Token endpoint requirements:**

The endpoint described here is a reference contract. `docs/dev/Open3DTransportWebRTC/Tests/mock-token-server.py` in the Open3DBroadcast repository implements it for local testing only. A real endpoint holds your LiveKit API secret, so anyone who can call it can join your rooms. It must:
- **Authenticate every caller.** The plugin sends `Authorization: Bearer <value>` from the `webrtc.tokenEndpointAuth` secret. An endpoint that answers without checking the caller hands out LiveKit tokens to anyone who can reach it.
- **Decide the grants itself** from the authenticated caller. The client sends room, identity and role as a request only; it does not send grants, and the endpoint must not accept grants from a client (a client that picks its own grants can publish).
- **Use HTTPS.** The plugin refuses a plain `http://` endpoint unless the host is `localhost`, `127.0.0.1` or `::1`.
- Keep the LiveKit API key and secret on the server.
- Accept POST with JSON: `{room, identity, role}`
- Return JSON: `{token, expiresAt}` or `{token, ttl}`

**Example token endpoint request and response:**
```json
// Request
POST https://your-server.com/token
Content-Type: application/json
Authorization: Bearer <endpoint credential>
{
  "room": "MyRoom",
  "identity": "sender-12345",
  "role": "publisher"
}

// Response
{
  "token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "expiresAt": 1700000000,
  "ttl": 3600
}
```

**Troubleshooting Auto Fetch:**
- Check the log category `LogO3DWebRTCTokenManager` for the fetch status. Logs show the endpoint without its query string and never show the response body or the token.
- "refused: use https://": the endpoint is plain `http://` on a host other than localhost.
- HTTP 401 or 403: the endpoint rejected the credential; check the **Token Endpoint Credential** or `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH`.
- "Token fetch timed out after 30.0 seconds": no token arrived in time. Check the endpoint URL and that the endpoint is reachable.
- Make sure the endpoint returns JSON with a `token` field.
- Failed fetches are retried; the retries show in the log.

### Room Configuration

**Room name**
- Manual token mode: the room is part of the token.
- Auto Token Fetch: set the **Room** field (transport option `webrtc.room`) to the same value on the sender and the receiver. It is required; the transport does not start without it.
- Several senders and receivers can join the same room.

**Identity**
- Each participant in a room needs its own identity.
- Auto Token Fetch generates one per sender or receiver instance (`sender-<pid>-<id>`, `receiver-<pid>-<id>`), so several in one editor do not replace each other.

---

## Audio Configuration

Audio is set on the sender component, under **Audio**: tick **Enable Audio**, then set the capture options. The WebRTC transport publishes the audio as a LiveKit audio track.

| Setting | Default | Notes |
|---|---|---|
| **Sample Rate** | 48000 Hz | 8000 to 48000 Hz. |
| **Num Channels** | 1 | 1 for mono, 2 for stereo. |
| **Bitrate Kbps** | 64 | The WebRTC transport passes it to LiveKit clamped to 16 to 128 kbps; 0 becomes 16. |

The receiver plays the audio when **Enable Audio** is ticked on the LiveLink source.

### Audio Quality Troubleshooting

**Audio too quiet**
- Raise the gain at the microphone or capture device, or the sender's **Game Gain** or **Mic Gain**.
- Check the speaker volume.

**Audio distorted or clipping**
- Lower the input gain.
- The transport clamps float samples at ±1.0; it does not reduce gain automatically.

**Audio drops out or is silent**
- Check the bitrate against the available bandwidth.
- Check that **Enable Audio** is ticked on the receiving LiveLink source. Without it, the receiver discards audio frames; with `LogO3DWebRTCReceiver` set to Verbose it logs "WebRTC audio frame discarded (no sink)".
- Audio is sent only while the sender is connected.

---

## Control Channel

Control events and values (`Fire Control Event`, `Set Control Value` on the sender component; `UO3DRemoteControlComponent` on the receiving side) work over WebRTC like over the other transports. Receiving control is off by default; a project turns it on itself, with **Accept Control** under Project Settings › Plugins › Open3DBroadcast Control or with `Set Control Receive Enabled` at runtime.

- Control travels on its own reliable, ordered LiveKit data channel labelled `__o3d.ctl`, next to the per-subject mocap channels. It is never counted as a frame, so frame, byte and drop statistics show mocap only.
- Each control message is at most 1,100 bytes; larger ones are refused by the sender (the control publisher splits snapshots to fit).
- Several senders can share one room: each sender's control carries its own source id, so receivers keep their events and values apart.
- A subject literally named `__o3d.ctl` still streams as mocap; the receiver recognises control by its contents, not by the channel name. Avoid the name anyway, to keep logs readable.
- **Update WebRTC receivers first.** A receiver built before control support (CTL-6) treats control messages as malformed mocap frames and logs warnings for them. Mocap is unaffected.

---

## Platform Support

### Current Status
- **Windows 64-bit:** supported.
- Linux and macOS: not supported. `livekit_ffi` exists for Win64 only, and the plugin's module is limited to Win64 (`PlatformAllowList`), so targets for other platforms leave it out.

### Alternative Transports
These are part of Open3DBroadcast itself:
- **NNG:** publish/subscribe, pair or push/pull over TCP; works with the Repeater.
- **TCP:** one sender to one receiver, reliable and ordered.
- **UDP:** unreliable datagrams, with optional broadcast on a LAN.
- **Loopback:** sender and receiver in the same process, for testing.

---

## Troubleshooting

### WebRTC Is Missing From the Transport List

Search the Output Log for `WebRTC transport not registered`:
- **"... was built for Open3DBroadcast transport API version ..."**: the add-on does not match your Open3DBroadcast release. Install the matching add-on build ([Version matching](#version-matching)).
- **"LiveKit FFI: library not found at ..."** or **"failed to load ..."**: `livekit_ffi.dll` is missing from `Open3DBroadcastWebRTC/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/bin/Win64/`. Reinstall the add-on folder unchanged.
- **No such line**: check that **Open3DBroadcast WebRTC** is enabled in **Edit → Plugins**. A developer build made with `O3D_WITH_TRANSPORT_WEBRTC=0` logs `Open3D WebRTC transport is not available in this build` instead.

Log categories for this add-on: `LogO3DWebRTCSender`, `LogO3DWebRTCReceiver` and `LogO3DWebRTCTokenManager` (for example `log LogO3DWebRTCSender Verbose`).

### Connection Failures

#### "WebRTC connection failed (code=N): ..."

LiveKit refused or lost the connection. The text after the code is LiveKit's own message. Check:
1. The **LiveKit Host**: see [Server URL](#server-url).
2. That the server is running and reachable from this machine, and that a firewall lets the port through (443 for `wss://` without a port).
3. The token: not expired, for the right room, and with the right permissions (publish for a sender, subscribe for a receiver).
4. That the sender and the receiver use different identities.

#### "Failed to connect (code=N): ..."

The connect call itself failed before LiveKit reported a state. Check the same points as above.

### Data Transfer Issues

#### Frames Being Dropped

**Symptoms:** the sender's `DroppedFrames` counter rises (**Get Transport Stats** on the sender component).

**Cause 1: not connected**
- While the sender is connecting or reconnecting, frames are dropped and counted.
- **Get Connection State** on the sender component returns **Connected** once frames can be sent.

**Cause 2: payload too large**
- Frames go on LiveKit's reliable data channel, which takes up to 15,000 bytes per frame.
- A larger frame is refused, and the log shows "Subject '...' payload size (N bytes) exceeds maximum (15000 bytes), consider simplifying skeleton". The message is throttled and reports how many similar ones it skipped.
- With `webrtc.prefer_lossy=true`, frames of up to 1,300 bytes go on the lossy data channel and larger ones on the reliable channel; the 15,000-byte limit still applies.
- **Solution:** send fewer bones or curves for that subject.

**Cause 3: LiveKit is full**
- When LiveKit's data channel cannot take a frame, the frame is dropped and counted in `SendErrors` and `DroppedFrames`. Lower the frame rate (see [Data Send Rate](#data-send-rate)).

**Cause 4: receiver falling behind**
- On the receiver, frames wait for the next LiveLink update. When more than 16 MiB is waiting, new frames are refused and counted in the receiver's `DroppedFrames`.
- **Solutions:** keep the receiving editor or game ticking (a stalled game thread stops LiveLink updates), or lower the send rate.

#### Latency Higher Than Expected

Latency is mostly network time to and from the LiveKit server.

**Solutions:**
1. Use a wired network rather than Wi-Fi.
2. Use a LiveKit server close to both ends.
3. Lower the frame rate if you do not need it.

#### The Receiver Reconnects While the Sender Is Idle

The receiver reconnects when no data arrived for `webrtc.reconnect_timeout` seconds (default 2). Raise it, or set 0 to turn it off.

### Audio Issues

#### "Failed to publish audio to track ..." Warning
LiveKit refused an audio frame. The message after the frame details is LiveKit's own reason. The warning is throttled. Audio submitted while the sender is not connected is dropped without a warning.

#### Audio Frame Discarded (No Sink)
**Message:** "WebRTC audio frame discarded (no sink)" (Verbose, `LogO3DWebRTCReceiver`)
**Cause:** the receiver has no audio output.
**Solution:** tick **Enable Audio** on the LiveLink source.

---

## Performance Tuning

### Data Send Rate

The sender component's **Capture Rate Hz** sets how often it captures and sends a pose (default 60). At most one capture happens per tick, so the frame rate also limits it.

**For heavy scenes (many bones):**
- Lower **Capture Rate Hz** to 30. That halves the frame data rate.
- Send fewer bones or curves.

### Bandwidth Estimation

**Frame data per second:**
```
Data rate = payload size in bytes × capture rate in Hz

Example:
  Payload = 5,000 bytes
  Capture rate = 60 Hz
  Data rate = 5,000 × 60 = 300,000 bytes/s = 2.4 Mbit/s
```

**Plus audio:**
```
Audio rate = bitrate in kbit/s ÷ 8, in kB/s
Example: 64 kbit/s = 8 kB/s

Total = data rate + audio rate
```

**To use less bandwidth:**
- Send fewer bones.
- Lower **Capture Rate Hz** (for example 60 to 30).
- Lower **Bitrate Kbps** (for example 64 to 32).

---

## FAQ

### Q: Can I use WebRTC on Linux?
**A:** No. The add-on supports Windows 64-bit only, because the LiveKit FFI library exists for Win64 only. Use the NNG, TCP or UDP transports instead.

### Q: How long are tokens valid?
**A:** As long as the server or token issuer allows; each token carries its expiry. With Automatic Token Fetch, the plugin fetches a new token before the current one expires.

### Q: Can several senders use the same room?
**A:** Yes. Senders with publish permission in the same room send at the same time, and receivers subscribe to all of them.

### Q: What if I can't reach the server?
**A:**
1. Check that the server is running.
2. Check the **LiveKit Host** (see [Server URL](#server-url)).
3. Check that a firewall lets WebSocket connections through.
4. Check that the host name resolves.

### Q: Can I switch transports at runtime?
**A:** Yes. Call **Set Transport Name** on the sender component (for example `webrtc` or `udp`). The change applies the next time capture starts, so call **Stop Capture** and **Start Capture** after it. Each transport keeps its own options.

### Q: How do I monitor the sender?
**A:** Call **Get Transport Stats** on the Open3D sender component. It returns the connection state and the counters `FramesSent`, `BytesSent`, `DroppedFrames`, `SendErrors` and `PendingFrames`. **Get Connection State** returns the state alone.

The WebRTC receiver also measures `AverageLatencyMs` and `MaxLatencyMs`: the time from a frame's arrival at the receiver until the receiver hands it to LiveLink. It is not network round-trip time, and the sender does not fill these fields. The receiver has no Blueprint node for its counters.

### Q: Can I rate-limit data sending?
**A:** Yes. Lower the sender component's **Capture Rate Hz** (for example from 60 to 30).

---

## Additional Resources

- **LiveKit Documentation:** https://docs.livekit.io
- **Open3DBroadcast User Guide:** `USER_GUIDE.md` in the Open3DBroadcast plugin folder
- **Report Issues:** [GitHub Issues](https://github.com/lifelike-and-believable/Open3DBroadcast/issues)
