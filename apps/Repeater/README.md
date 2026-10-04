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

## Docker

`docker/Dockerfile.repeater` builds only the Repeater, against the pinned submodules (check them
out first), and ships NNG's `nngcat` next to it for testing:

```
docker build -f docker/Dockerfile.repeater -t open3dstream-repeater .
docker run -p 7000:7000 -p 7001:7001 -e REPEATER_OPTIONS="--stats-seconds 60" open3dstream-repeater
```

`LISTEN_ADDR`, `BROADCAST_ADDR` and `REPEATER_OPTIONS` set the arguments; `docker stop` stops it
cleanly. `compose/docker-compose.yml` and `cloud-init/cloud-init-repeater.yaml` run the published
image (`ghcr.io/lifelike-and-believable/open3dstream-repeater`), which the
`Build and publish Repeater image` workflow builds; that workflow is disabled until the image is
wanted.

The relay is `relay.h` and `relay.cpp`; the core tests run it with real sockets
(`test/repeater_tests.cpp`).
