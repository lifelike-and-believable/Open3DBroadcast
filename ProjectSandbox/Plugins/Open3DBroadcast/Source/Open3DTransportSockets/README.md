# Open3DTransportSockets

Two transports on Unreal's socket subsystem: **TCP** and **UDP**. Pick either in the transport
list of the Open3D sender component or of the **Open3DStream Receiver** LiveLink source. Frames,
audio and control share one socket.

| | TCP | UDP |
|---|---|---|
| Default port | 17700 | 17800 |
| Who listens | the sender | the receiver |
| Receivers | one at a time | any number (with broadcast) |
| Delivery | ReliableOrdered | Unreliable |
| Encryption, authentication | none | none |

## TCP

The sender listens and serves one receiver at a time. Further receivers wait in the listen
backlog until the connected one goes away. While no receiver is connected, queued frames are
dropped.

### Options

Options without a **Shown as** entry are not in the settings panel. Set them with
**Set Transport Option** on the sender component, or in the options map of
**Create Open3DStream LiveLink Source**.

| Key | Side | Shown as | Default | Notes |
|---|---|---|---|---|
| `bind` | sender | **Bind Address** | `0.0.0.0` | Local address to listen on. An IP literal or `0.0.0.0`. |
| `host` | receiver | **Remote Host** | `127.0.0.1` | The sender's address. |
| `port` | both | **Port** | 17700 | 1 to 65535. |
| `tcp.timeout` | receiver | **Connection Timeout (seconds)** | 5 | Reconnect when no data arrived for this long. The panel takes 1 to 60. |
| `tcp.connecttimeout` | receiver | | 5 | Seconds to wait for a connect before retrying. |
| `tcp.backoff` | receiver | | 500 | First reconnect delay in ms, doubled after each failure (10 to 60000). |
| `tcp.maxbackoff` | receiver | | 5000 | Longest reconnect delay in ms (10 to 600000). |
| `tcp.maxframe` | receiver | | 4194304 | Largest payload accepted, in bytes (1024 to 50 MiB). |
| `tcp.maxqueue` | sender | | 4194304 | Bytes of queued frames, and separately of queued audio. A frame that does not fit is refused. At least 65536. |
| `tcp.maxqueueage` | sender | | 1000 | Frames and audio that waited longer than this many ms are dropped before sending. 0 turns it off. |
| `tcp.stalltimeout` | sender | | 2000 | Drop the receiver when a frame makes no progress for this many ms. At least 100. |
| `tcp.keepalive` | sender | | 1000 | Send a keepalive after this many idle ms, so the receiver does not time out. 0 turns it off. |

### Framing

Each message on the stream has an 18-byte header, then the payload:

| Bytes | Content |
|---|---|
| 0-13 | magic `00 FF 03 FE` followed by `O3DS-START` |
| 14-17 | payload length, unsigned 32-bit, little-endian |

A payload is an Open3D frame, or a unified envelope (magic `O3DU`) for audio and control. The
keepalive is an audio envelope with an empty payload. The core's `src/o3ds/tcp_stream_parser.h`
holds the layout and the parser.

## UDP

The sender sends datagrams to one address; the receiver binds a local port. With
**Enable UDP Broadcast** on the sender and **Accept Broadcast Packets** on the receiver, one
sender reaches every receiver on the subnet. Multicast is not supported.

### Options

| Key | Side | Shown as | Default | Notes |
|---|---|---|---|---|
| `host` | sender | **Destination Host** | `127.0.0.1` | Address the datagrams go to. `*` sends to `255.255.255.255` and turns broadcast on. Another broadcast address needs **Enable UDP Broadcast**. A host name is resolved on the worker thread. |
| `host` | receiver | **Bind Address** | `0.0.0.0` | Local address to listen on. An IP literal or `0.0.0.0`. |
| `port` | both | **Port** | 17800 | 1 to 65535. |
| `udp.broadcast` | sender | **Enable UDP Broadcast** | false | Allow sending to a broadcast address. |
| `udp.broadcast` | receiver | **Accept Broadcast Packets** | false | Receive datagrams sent to a broadcast address. |
| `udp.maxdatagram` | both | **Max Datagram Bytes** | 64000 | 512 to 65507. Sender: a message up to this size goes out as one datagram; a larger one is split into fragments. Receiver: a datagram larger than this plus the 24-byte fragment header is dropped, so set it at least as large as the sender's. |
| `udp.mtu` | sender | **MTU** | 1200 | Size of each fragment, header included, when a message is split. 256 up to `udp.maxdatagram`. |
| `udp.maxframe` | receiver | | 4194304 | Largest reassembled message, in bytes (65507 to 50 MiB). Not in the panel. |

With the defaults, a message is split only above 64000 bytes.

### Fragments

A fragment is a datagram with a 24-byte little-endian header, then its part of the message:

| Bytes | Content |
|---|---|
| 0-3 | magic `O3DF` |
| 4 | version, 2 |
| 5 | flags, 0 |
| 6-7 | reserved, 0 |
| 8-11 | message id |
| 12-15 | fragment index |
| 16-19 | total size of the reassembled message |
| 20-23 | size of this fragment's payload |

Datagrams that are not fragments carry one Open3D frame or one unified envelope (`O3DU`) each.
Control is never fragmented. The core's `src/o3ds/udp_fragment.h` holds the layout.

### Queue

UDP prefers fresh frames over complete ones. When the worker falls behind, the oldest waiting
frames are dropped (more than 4 frames or 16 MiB waiting); callers are refused only at twice that.
Audio has a 1 MiB budget of its own.
