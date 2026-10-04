# Repeater

Relays Open3DStream traffic over NNG: senders push to one address, and every receiver subscribed
to the other gets each message, byte for byte and in order per sender. Use it when senders and
receivers cannot reach each other directly, or when one sender feeds many receivers.

```
Repeater listen-addr broadcast-addr [--max-message-mb N] [--send-buffer N] [--stats-seconds N]
Repeater tcp://0.0.0.0:7000 tcp://0.0.0.0:7001
```

- **Senders** (UE NNG transport): mode `push`, role `client`, connected to `listen-addr`.
- **Receivers** (UE NNG transport): mode `sub`, role `client`, connected to `broadcast-addr`.
- `--max-message-mb` (default 64; 0 = unlimited): the largest message a sender may push. A larger
  one closes that sender's connection (the NNG TCP transport's behaviour), and the sender
  reconnects.
- `--send-buffer` (default 256): messages queued per subscriber. A subscriber that falls further
  behind misses messages (NNG pub never blocks the relay).
- `--stats-seconds` (default 10; 0 = none): a line with the messages and bytes relayed, by kind
  (mocap, audio, control, other), the errors, and the connected senders and subscribers. The relay
  never drops a message it does not understand; it only counts it as other.
- `SIGINT` or `SIGTERM` stops it cleanly and prints the totals.

A receiver that subscribes after a sender started gets the sender's next full frame: with the
default (legacy) encoding that is the next frame; with delta or residual encoding it is the next
periodic full sync (`FullSyncIntervalSeconds`). The sender's peer-joined full sync (ADR 0005 (vi))
does not reach receivers behind the Repeater.

The relay is `relay.h` and `relay.cpp`; the core tests run it with real sockets
(`test/repeater_tests.cpp`).
