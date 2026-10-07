# 0015: WebRTC reconnect

- **Status:** Accepted (maintainer sign-off 2026-10-07: "take your recommendations")
- **Date:** 2026-10-07
- **Findings:** TRF-6, TRF-26 (`docs/review/2026-09-plugin-review/transports-webrtc-moq.md`); related TRF-7, TRF-33
- **Supersedes:** the "Reconnect" bullet of [ADR 0007](0007-transport-abstraction-and-registry.md)'s PR 4f addendum, which kept `FO3DReconnectPolicy` out of WebRTC.

Paths below are relative to the add-on's `Source/Open3DTransportWebRTC/Private/`. "FFI" is
`lifelike-and-believable/livekit-ffi` v0.2.7 (da50d08; `livekit_ffi/src` is unchanged up to 50e6663),
whose `backend_livekit.rs` is cited as `F:line`; "SDK" is the `livekit` crate 0.7.24 it pins.

## Context

**Receiver.** `Poll()` (game thread) requests a reconnect when no data arrived for
`webrtc.reconnect_timeout` seconds (default 2, `Receiver/WebRTCReceiver.cpp:565-571`), whatever the
link's state: while LiveKit is still joining, while the SDK reconnects on its own (`LkConnReconnecting`
does not touch the timer, `:202-205`), and in a healthy room that has no publisher. `LkConnDisconnected`
and `LkConnFailed` reconnect on the next `Poll`. The reconnect (`:918-954`) destroys and recreates the
client on the game thread, with `lk_disconnect` blocking (`F:904-921`). There is no backoff; the only
delay is a fixed 5 s after a synchronous connect error.

**Sender.** On `LkConnDisconnected` or `LkConnFailed` it only records the state; `bConnectIssued` is
never cleared, so it stays dead until Stop and Start (`Sender/WebRTCSender.cpp:625-666`, `:785-794`).

**The FFI calls the 2026-10-06 plan wanted to use do not do what their names say:**
- `lk_set_reconnect_backoff` ignores its arguments and returns ok (`F:522-538`). The SDK has no
  reconnect settings to forward to: 10 attempts at a fixed 5 s are constants (SDK
  `rtc_engine/mod.rs:52-53`).
- `lk_client_is_ready` returns 1 from `LkConnConnected` until `lk_disconnect` (`F:923-931`), including
  while the SDK reconnects and after it has given up, and it takes the client mutex, which a send holds
  for a whole SDK reconnect.

**What the SDK does itself:** it recovers from a network drop (resume, then a full restart, up to
about 45 to 50 s), reporting `LkConnReconnecting` then `LkConnConnected`, and republishes the sender's
audio tracks. When it gives up, the FFI fires `LkConnDisconnected` (twice, F:828-847) but keeps its
`room`, so a connect on the same client returns error 104 until `lk_disconnect`. `LkConnFailed` comes
only from a failed initial connect.

## Decision

1. **Health signal:** the `LkState` the add-on already receives. **Backoff:** the shared
   `FO3DReconnectPolicy` (`Open3DShared/Public/Transport/O3DTransportWorker.h`), on both sides,
   game-thread owned: initial 1 s, max 30 s, ×2, jitter 0.2, and **no attempt limit** (as NNG). Our
   loop starts only after LiveKit has given up (`LkConnDisconnected`) or a connect failed
   (`LkConnFailed`, or a synchronous connect error), so it never runs against LiveKit's own reconnect.
   `LkConnConnected` resets it. The double `LkConnDisconnected` counts as one failure.
2. **No-data watchdog:** off by default (`webrtc.reconnect_timeout` = 0). When a user sets it, it
   counts only while the state is `LkConnConnected` and goes through the same policy.
3. **Reconnect on the same handle:** call `lk_disconnect` first when the FFI still holds a room (after
   `LkConnDisconnected`), otherwise connect directly. The receiver keeps recreating its client as today.
4. **Sender audio tracks after our own reconnect:** the cached track handles point at pipelines that
   `lk_disconnect` removed. They are moved, under `AudioTracksMutex`, to a retired list (a late publish
   on one just fails), destroyed at `Stop` after the gate closes, and new tracks are created on the new
   room. An SDK-internal reconnect needs none of this.
5. **Not used:** `lk_set_reconnect_backoff` and `lk_client_is_ready`. Both go to
   `docs/livekit_ffi_feature_request.md` with what they would need to do.
6. **State shown:** `Reconnecting` while the policy is retrying; the sender no longer goes to `Failed`
   on a dropped room.

## Consequences

- An idle room (no publisher) is never torn down. A link that dies silently without LiveKit noticing
  is only detected if the user turns the watchdog on. Whether the SDK's signal ping catches that case
  is unverified (that code is in `livekit-api`).
- A manual token that has expired keeps failing under the backoff (at most one attempt per 30 s); auto
  token fetch gets a new token on the next connect.
- `lk_disconnect` on the game thread remains for Stop and for the reconnect after LiveKit gave up
  (fast then: the room is already closed). TRF-7 (moving it off the game thread) stays separate.

## Verification

Tests on the add-on's fake LiveKit, which must model: error 104 while a room is held, `lk_disconnect`
clearing it, the double `LkConnDisconnected`, and a clock seam (no sleeps):
- receiver: no reconnect while `Reconnecting` or idle with the watchdog off; reconnect with growing
  delays after `LkConnDisconnected`; the watchdog only in `Connected`;
- sender: reconnects after `LkConnDisconnected` (disconnect first) and after `LkConnFailed`, with
  backoff; audio after a reconnect goes to new tracks; Stop destroys retired ones.
- Not verified without a live LiveKit server: the timings against a real network drop (desk check).

## References

TRF-6, TRF-7, TRF-26, TRF-33; ADR 0007; `docs/livekit_ffi_feature_request.md` sections 6 and 12.
