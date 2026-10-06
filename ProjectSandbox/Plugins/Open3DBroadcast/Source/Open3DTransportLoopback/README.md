# Open3DTransportLoopback

The **Loopback** transport passes frames, audio and control from a sender to a receiver in the
same process, without a network. Use it to test a sender and a LiveLink receiver in one editor
or game. Pick it in the transport list of the Open3D sender component or of the
**Open3DStream Receiver** LiveLink source.

## How it works

Each channel is one in-process queue, keyed by its name. Senders put items on it; the receiver's
`Poll` (game thread) takes them off and hands them on. A sender and a receiver connect when they
use the same **Channel Name**. Names are trimmed and compared without regard to case.

- Delivery is ReliableOrdered: nothing is lost or reordered.
- When the frame queue is full, the sender refuses the new frame (`DroppedBackpressure`), keeps
  the queued ones, and warns at most once every 2 seconds ("Loopback queue full"). A full queue
  usually means no receiver is polling the channel.
- The receiver takes frames off the channel only between Start and Stop. Frames sent while it is
  stopped wait in the channel, up to its capacity.
- Audio and control have limits of their own, so neither waits behind frames. Control keeps the
  shared queue's cap of 1,024 envelopes.
- A channel exists while a sender or receiver holds it.

## Options

Options without a **Shown as** entry are not in the settings panel. Set them with
**Set Transport Option** on the sender component, or in the options map of
**Create Open3DStream LiveLink Source**.

| Key | Side | Shown as | Default | Notes |
|---|---|---|---|---|
| `channel` | both | **Channel Name** | `default` | Empty uses `default`. |
| `loopback.maxqueue` | sender | **Queue Capacity** | 64 | Frames the channel holds. The panel takes 1 to 4096. A value below 1 means the default. |
| `loopback.maxaudioqueue` | sender | | 32 | Audio items the channel holds. A value below 1 means the default. |

The older keys `maxqueue` and `maxaudioqueue` are still read when the new ones are absent.

The sender sets the channel's limits, whichever end starts first. A receiver never changes them;
a channel that a receiver creates before any sender starts uses the defaults until a sender sets
its own.

## Debugging

The console variable `o3ds.Loopback.Audio.Debug` logs the audio path: 0 off, 1 basic, 2 verbose.
