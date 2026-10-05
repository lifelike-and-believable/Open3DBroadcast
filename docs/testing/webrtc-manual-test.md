# WebRTC transport: manual test against a local LiveKit server (WP-S7)

These steps check the WP-S7 fixes (TRF-2, 3, 4, 5, 15, 16, 19, 23, 24, 25, 31) and, in case 12,
the control channel (CTL-6, ADR 0011) end to end with a real LiveKit server. They complement the fake-FFI automation tests
(`Open3DBroadcast.Transport.WebRTC.*`), which need no server. Run them on Win64 in the editor.

## Setup

1. Start LiveKit in dev mode. It listens on `ws://127.0.0.1:7880` with API key `devkey` and
   secret `secret`:

   ```bash
   livekit-server --dev
   ```

2. Start the mock token server so its tokens are accepted by that LiveKit server. `TOKEN_TTL`
   is short on purpose so a refresh happens during the test:

   ```bash
   cd docs/dev/Open3DTransportWebRTC/Tests
   pip install flask pyjwt
   LIVEKIT_API_KEY=devkey API_SECRET=secret TOKEN_TTL=180 python mock-token-server.py --port 8080
   ```

   Check it: `curl -s -X POST http://localhost:8080/token -H "Content-Type: application/json" -d '{"room":"wp-s7","identity":"probe","role":"publisher"}'`
   returns a `token`.

3. For the manual-token cases, create tokens with the LiveKit CLI:

   ```bash
   lk token create --api-key devkey --api-secret secret --join --room wp-s7 --identity manual-sender --valid-for 1h
   lk token create --api-key devkey --api-secret secret --join --room wp-s7 --identity manual-receiver --valid-for 1h
   ```

4. In the editor, set `LogO3DWebRTCSender`, `LogO3DWebRTCReceiver` and
   `LogO3DWebRTCTokenManager` to `Log` (the default) and keep the Output Log open.

Sender settings (Open3D sender component, transport **WebRTC**): LiveKit Host `127.0.0.1:7880`.
Receiver settings (LiveLink source, transport **WebRTC**): the same host.

## Cases

Record pass or fail and paste the relevant log lines for each case into the PR.

### 1. Manual token connects (baseline)

- Sender: Auto Token Fetch off, Access Token = the `manual-sender` token. Receiver: the
  `manual-receiver` token.
- Start PIE (or start capture) and create the receiver source.
- Expect: `WebRTC sender connecting...`, then `WebRTC connected`; `WebRTC receiver connected`.
  The LiveLink subject animates.

### 2. Auto-fetch connects (TRF-3)

- Both sides: Auto Token Fetch on, Token Endpoint URL `http://localhost:8080/token`, Room `wp-s7`.
- Start the sender and the receiver.
- Expect on each side, once: `Fetching token...`, `Token fetch completed successfully`, then a
  connect and `connected`. The mock server prints one `[TOKEN] Generated` line per side, both with
  `room=wp-s7`. The subject animates.
- Before WP-S7 this case never connected.

### 3. Room is required in auto-fetch mode (TRF-25)

- Clear the Room field on the sender and start it.
- Expect: `Auto-fetch enabled but no room set (transport option 'webrtc.room')` and the sender
  does not start. Restore the room afterwards.

### 4. Two senders in one editor (TRF-25)

- Place two actors with sender components (different subject names), both auto-fetch, room `wp-s7`.
- Expect: two `[TOKEN] Generated` lines with different identities (`sender-<pid>-<id>`), both
  senders stay connected, and the LiveKit server log shows no `DUPLICATE_IDENTITY` disconnect.

### 5. Token refresh (TRF-23)

- Keep case 2 running. Set Token Refresh Lead Time to `60` on both sides before starting
  (with `TOKEN_TTL=180` the refresh starts about 120 s after connect).
- Expect after about 120 s, on each side: `Fetching token...`, then
  `Applied refreshed LiveKit token`. There is no reconnect and the subject keeps animating.
- If LiveKit's FFI does not support refresh, expect on the sender:
  `lk_refresh_token failed ... The new token will be used on the next connect.` and on the
  receiver: `... Reconnecting with the new token.` followed by a reconnect. Note which one you saw;
  it tells us whether the shipped `livekit_ffi.dll` implements `lk_refresh_token`.
- Leave it running past 180 s (the first token's expiry). Streaming continues.

### 6. Token server down, then back (TRF-24)

- Stop the mock token server. Start the sender in auto-fetch mode.
- Expect: `Retrying token fetch (attempt n/5)` with growing delays (1, 2, 4, 8, 16 s), then
  `Token fetch failed after 6 attempts` or `Token fetch timed out after 30.0 seconds`. A new fetch
  cycle then starts; fetches start at most once every 5 s. No log line repeats every frame.
- Restart the mock server. Expect the next attempt to succeed and the sender to connect.
- While a retry is pending, stop PIE. Expect no crash and no token log lines after the stop.
  Repeat once with a PIE restart in between (retries no longer depend on `GWorld`).

### 7. Expired manual token logs once (TRF-23)

- Create a token with `--valid-for 5s`, wait 10 s, then use it as the sender's manual token.
- Expect `Current token is expired` once, not once per frame, and no connect.

### 8. UTF-8 subject names (TRF-2, TRF-31)

- Rename the sender's subject to a name with non-ASCII characters, for example `Subjé_角色`,
  and enable audio on both sides.
- Expect: the LiveLink subject on the receiver has exactly the same name, and the audio for that
  subject reaches the same subject (the LiveKit track name is the same UTF-8 string; check it in
  the LiveKit server log or with `lk room participants list` if available).

### 9. Data callbacks (TRF-16, FFI request section 13)

- With case 2 running, check the receiver stats and LiveLink subjects after connect.
- Expect: frames arrive (the receiver registers both data callbacks, because livekit_ffi's
  async-connect loop calls only the unlabeled one), no LiveLink subject named `default`, and frame
  counts in the receiver stats match the sender's `FramesSent` (no doubling).

### 10. Sending under load (TRF-5)

- Run two subjects at 60 Hz (or one subject at 120 Hz if the sender allows) on a healthy local link.
- Expect: no `backpressure` or `queue building` warnings (those messages were removed), and no
  dropped frames in the sender stats while the link is idle.

### 11. Stop and start cycles

- Start and stop PIE five times with audio enabled on both sides, in both token modes.
- Expect: no crash, no `Failed to destroy audio track` warnings, and each run connects again.

### 12. Control channel: events and values over WebRTC (CTL-6, ADR 0011)

Control rides a reliable, ordered data channel labelled `__o3d.ctl`. The automation tests
(`Open3DBroadcast.Transport.WebRTC.Control.*`) cover it against a fake FFI; this case checks it
through a real LiveKit server and answers what the fake cannot: that two senders in one room stay
apart (ADR 0011 Verification, WebRTC), and whether the real data channel keeps control in order
(ADR 0011 open question 4).

- Receiver side: enable control, either with **Accept Control** under Project Settings ›
  Plugins › Open3DBroadcast Control or with `Set Control Receive Enabled (true)` on BeginPlay.
  Add a `UO3DRemoteControlComponent` (Remote Control) to an actor in the receiving level and bind
  `OnControlEvent` and `OnControlValueChanged` to a Print String of the name or key, the value and
  the meta's `SourceId` and `SourceName`.
- Sender side: use case 4's two senders (room `wp-s7`, different subject names). On one, call
  `Fire Control Event` with `vfx.test` and an Int payload that you increase on every press (bind
  it to a key); on the other, call `Set Control Value` with `env.fog_density` and a Double that a
  timeline drives every tick.
- Expect:
  - every `vfx.test` press prints exactly once, with increasing payloads in the order fired;
  - `env.fog_density` updates smoothly and ends at the sender's last value;
  - the two senders print different `SourceId`s, and each key or event appears only under its
    own sender;
  - the LiveLink subjects animate as before, no LiveLink subject named `__o3d.ctl` appears, and
    the receiver's frame counts match the senders' `FramesSent` (control is not counted);
  - stopping and restarting one sender (PIE stop or a details-panel edit) shows no
    `OnControlValueCleared` and no repeated `OnControlValueChanged` for an unchanged key.
- Late joiner: start the receiver after the senders have set values. Expect the current values
  within one snapshot interval (`ControlSnapshotIntervalSeconds`, 1 s by default) and no events
  fired before the receiver joined.
- Old receiver (optional): a receiver add-on built before CTL-6 hands control to its mocap path,
  which rejects it. Expect its mocap unaffected and, with CTL-2 or later in the main plugin, at
  most one malformed-frame warning per stream every 10 s while control flows (ADR 0011 item 10).
- Record whether any event arrived out of order or twice. Either would mean the reliable data
  channel is not ordered per sender, which ADR 0011 open question 4 assumes it is.

## Not covered here

- Refresh fallback on the sender (a full reconnect) is not implemented; see the WP-S7 PR notes.
- Token endpoint authentication and secret storage are WP-S9.
