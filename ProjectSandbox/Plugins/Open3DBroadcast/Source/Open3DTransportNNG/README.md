# Open3D Transport NNG Module

High-performance transport for Open3D mocap data using NNG (nanomsg-next-gen).

## Protocols Supported

- **Pub/Sub**: Publisher/Subscriber (one-to-many broadcast)
- **Push/Pull**: Pipeline (one-to-one with built-in load balancing)
- **Pair**: Bidirectional (one-to-one)

## Configuration

### URI Format

```
nng://<host>:<port>?mode=<mode>&role=<role>
```

Where:
- `host`: IP address or hostname
- `port`: TCP port number
- `mode`: `pub`, `sub`, `push`, `pull`, `pair` (default: `pub`)
- `role`: `server` (listen) or `client` (dial). Supported roles, with the default first:

| Side | Mode | Roles |
|---|---|---|
| Sender | `pub` | `server` only |
| Sender | `pair` | `server`, `client` |
| Sender | `push` | `client`, `server` |
| Receiver | `sub` | `client` only |
| Receiver | `pair` | `client`, `server` |
| Receiver | `pull` | `server`, `client` |

With default roles, one side of every pair listens and the other dials. A role a mode does
not support falls back to the default. With no `host` option, the host comes from the URI,
then its `?host=` query, then the stream id; the last resort is `0.0.0.0` for a listening
socket and `127.0.0.1` for a dialing one.

### Example Configurations

**Localhost Pub/Sub (default):**
```
nng://127.0.0.1:5555
```

**Cloud Push/Pull via Repeater:**
- Sender (Push): `tcp://<repeater-host>:7000`
- Repeater Listen: `tcp://0.0.0.0:7000` (receives Push from sender)
- Repeater Broadcast: `tcp://0.0.0.0:7001` (publishes to receivers)
- Receiver (Subscribe): `tcp://<repeater-host>:7001`

## Troubleshooting

### No Connection / Queue Full Warnings

If you see "NNG sender queue full" warnings and no animation on the receiver:

1. **Verify ports match**: Check that sender/receiver ports match the repeater configuration
   - Sender should connect to repeater's listen port (e.g., 7000)
   - Receivers should connect to repeater's broadcast port (e.g., 7001)

2. **Check network connectivity**: Ping the remote host and verify firewall rules allow the ports

3. **Enable verbose logging**: Monitor the logs for connection establishment messages
   - Look for "NNG sender pipe added" = connection successful
   - Look for "NNG sender pipe removed" = connection lost

### High Latency / Cloud Connections

The sender never blocks on network I/O. Frames, audio and control go into one send queue, and a
worker thread owns the socket and sends with `NNG_FLAG_NONBLOCK`:
- The queue refuses a new frame once the waiting frames reach `nng.qmax` bytes; queued frames are
  never discarded. Audio has a budget of the same size of its own, and control a cap of 1,024
  envelopes, so neither is refused because frames are waiting.
- When no peer is ready or NNG's send buffer (1024 messages) is full, the oldest queued frame
  is dropped and counted in `DroppedFrames`. Frames are never re-queued, so a receiver that
  reconnects gets current data, not a stale backlog.
- There is no send timeout, because a non-blocking send never waits.
- A dialing socket is reconnected by NNG in the background. The sender and receiver only reopen
  a socket that failed to listen or dial, with a backoff of 0.1 s doubling to 5 s.

If you see frame drops with high latency:
- Increase the sender's "Queue Capacity (MiB)" (option `nng.qmax`, in bytes; default 4 MiB)

## Performance Notes

- Single subject mocap: ~4 KB per frame at 30 FPS = ~120 KB/sec
- Audio (PCM16, 48kHz stereo): ~4 KB per frame at 50 FPS = ~200 KB/sec
- Combined with Opus compression: ~240 bytes per audio frame

Even slow cloud connections (10+ Mbps) can easily handle this bandwidth.
