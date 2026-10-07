# Open3DTransportNNG

The **NNG** transport sends Open3D frames, audio and control over TCP with NNG
(nanomsg-next-gen). Pick it in the transport list of the Open3D sender component or of the
**Open3DStream Receiver** LiveLink source. Win64 only.

## Modes

Set the same pairing on both ends:

| Sender mode | Receiver mode | Shape | Delivery |
|---|---|---|---|
| `pub` (**Publisher**, default) | `sub` (**Subscriber**, default) | one sender, many receivers | Unreliable: a slow subscriber can miss messages |
| `pair` (**Pair**) | `pair` (**Pair**) | one sender, one receiver | ReliableOrdered |
| `push` (**Push**) | `pull` (**Pull**) | many senders, one receiver | ReliableOrdered |

A subscriber receives every message. Subscription topics are not supported; a topic left in a
config is ignored with the warning "NNG subscription topics are not supported; ignoring '...'
and receiving every message."

## Options

| Key | Shown as | Default | Notes |
|---|---|---|---|
| `host` | **Host** | `127.0.0.1` | Address to listen on or dial. A listener needs `0.0.0.0` (or one interface's address) to accept other machines, and then logs a warning at Start. |
| `port` | **Port** | 6000 for pub/sub, 7000 for pair, 8000 for push/pull | TCP port, 1 to 65535. |
| `nng.mode` | **Mode** | sender `pub`, receiver `sub` | Sender: `pub`, `pair`, `push`. Receiver: `sub`, `pair`, `pull`. |
| `nng.role` | **Role** | **Default for the mode** | `server` (**Listen (server)**) or `client` (**Dial (client)**). Shown only for the modes that can do both; see [Roles](#roles). |
| `nng.qmax` | **Queue Capacity (MiB)** | 4 MiB | Sender only. Stored in bytes; the panel takes 1 to 512 MiB. The sender clamps the value to 64 KiB to 512 MiB; 0 means the default. |

You set options, not a URI. The transport builds `nng+<mode>://<host>:<port>` (with `?role=`
when the role is not the mode's default) for its logs and stream id, and opens a plain
`tcp://<host>:<port>` socket.

### Roles

One end listens and the other dials. Supported roles, the default first:

| Side | Mode | Roles |
|---|---|---|
| Sender | `pub` | `server` only |
| Sender | `pair` | `server`, `client` |
| Sender | `push` | `client`, `server` |
| Receiver | `sub` | `client` only |
| Receiver | `pair` | `client`, `server` |
| Receiver | `pull` | `server`, `client` |

With default roles, one end of every pairing listens and the other dials. A role the mode does
not support falls back to the default.

## Example: through the Repeater

The Repeater (`apps/Repeater` in the Open3DBroadcast repository) relays what senders push on one
port to every subscriber on another. Run it as `Repeater tcp://0.0.0.0:7000 tcp://0.0.0.0:7001`,
then set:

| End | Mode | Role | Host | Port |
|---|---|---|---|---|
| Sender | `push` | default (`client`) | the Repeater's address | 7000 |
| Receiver | `sub` | default (`client`) | the Repeater's address | 7001 |

`apps/Repeater/README.md` describes its options and its Docker image.

## Behaviour

- **Sending never blocks the caller.** Frames, audio and control go into one send queue; a worker
  thread owns the socket and sends with `NNG_FLAG_NONBLOCK`. There is no send timeout.
- **Queue limits.** The queue refuses a new frame once the waiting frames reach `nng.qmax` bytes,
  and never discards a frame it accepted. Audio has a budget of the same size of its own; control
  has a cap of 1,024 envelopes.
- **No peer, or NNG's buffer full.** NNG's send buffer holds 1,024 messages. When no peer is ready
  or that buffer is full, the worker drops the oldest queued frame and counts it in
  `DroppedFrames`. Frames are not re-queued, so a receiver that reconnects gets current data.
- **Reconnecting.** NNG redials a dialing socket in the background. The sender and receiver reopen
  only a socket that failed to listen or dial, after 0.1 s, doubling up to 5 s.
- **Largest message.** The receiver accepts messages up to 50 MiB.
- **Security.** The connection is plain TCP, without encryption or authentication. Anyone who
  can reach the port can connect.

## Troubleshooting

### "NNG sender queue full" and nothing arrives

1. Check that the modes pair up (Publisher with Subscriber, Pair with Pair, Push with Pull) and
   that the ports match. Through the Repeater, the sender uses the listen port (7000 above) and
   receivers the broadcast port (7001).
2. Check that the host is reachable and that a firewall lets the port through.
3. Watch the Output Log for the connection lines:
   - Sender: "NNG sender connection established (pipe count=N)" and "NNG sender connection lost
     (pipe count=N)".
   - Receiver: "NNG receiver pipe added (count=N)" and "NNG receiver pipe removed (count=N)".
   - A dialing sender that cannot connect yet logs "NNG sender could not dial ... yet; retrying
     with backoff (check host/port)".

### Frames dropped on a slow link

Raise the sender's **Queue Capacity (MiB)** (`nng.qmax`).
