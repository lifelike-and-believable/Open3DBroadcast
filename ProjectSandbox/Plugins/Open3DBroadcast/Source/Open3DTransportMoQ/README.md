# Open3DTransportMoQ

The **MoQ** transport sends Open3D frames, audio and control through a MoQ (Media over QUIC)
relay, using the `moq-ffi` library. The relay must speak draft-ietf-moq-transport-07.

To use it in the editor, pick **MoQ** in the transport list of the Open3D sender component or of
the **Open3DStream Receiver** LiveLink source and fill in the options below. The plugin's
`USER_GUIDE.md` describes the sender component and the LiveLink source.

- Separate tracks for mocap, audio and control (see [Track naming](#track-naming)).
- Audio as PCM16 or Opus, with the codec read from each audio frame.
- Reconnects with capped exponential backoff (see [Error Handling](#error-handling)).

## Configuration Options

Options without a **Shown as** entry are not in the settings panel. Set them with
**Set Transport Option** on the sender component, or in the options map of
**Create Open3DStream LiveLink Source**. Each key has the alternate names in the second column;
the first one set wins.

| Key | Also read as | Side | Shown as | Default | Notes |
|---|---|---|---|---|---|
| `relay_url` | `moq.relay` | both | **Relay URL** | none | Required, for example `https://relay.example.com:443`. |
| `track_namespace` | `moq.namespace` | both | **Track Namespace (optional)** | `mocap/<session>` | A namespace that starts with `mocap/` or `audio/` gets the matching prefix for each track. Any other namespace is used as it is for mocap; audio and control get `audio/` and `control/` in front of it. |
| `track_name` | `moq.track` | both | **Track Name (optional)** | the stream id's last part; `primary` when there is none | The sender has no stream id, so with no options both ends use `mocap/default` and `primary`. |
| `moq.session` | | both | | the stream id's first part; `default` when there is none | Used only when no namespace is set. |
| `delivery_mode` | `moq.delivery` | sender | **Delivery Mode** | `stream` | `stream` delivers every frame in order; `datagram` drops late frames. |
| `queue_bytes` | `moq.queue_bytes`, `moq.qbytes` | sender | **Queue Capacity (MiB)** | 8 MiB | Stored in bytes. Clamped to 256 KiB to 256 MiB. |
| `connect_timeout` | `moq.connect_timeout` | both | | 15 | Seconds before an unfinished connect attempt is abandoned and retried on a new client. Clamped to 1 to 120. |

The receiver has no delivery or queue options: the publisher picks the delivery.

## Track naming

Each track is `<type>/<session>/<track>`:

```
mocap/session1/character1     frames
audio/session1/character1     audio
control/session1/character1   control events and values
```

## Using the transport from C++

Code that drives the transport directly, without the sender component or the LiveLink source,
creates it from the transport registry and passes the options in `AdvancedParams`.
`Config.Uri` is used as the relay URL when `relay_url` is not set.

```cpp
FO3DTransportConfig Config;
Config.Uri = TEXT("https://relay.example.com:443");
Config.StreamId = TEXT("session1/character1");
Config.AdvancedParams.Add(TEXT("delivery_mode"), TEXT("stream"));  // or "datagram"
Config.AdvancedParams.Add(TEXT("queue_bytes"), TEXT("8388608"));   // 8 MiB

TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender = FO3DTransportRegistry::Get().CreateSender(TEXT("MoQ"));
Sender->Initialize(Config);
Sender->Start();

// Audio
TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink = Sender->CreateAudioSink(AudioConfig);
AudioSink->SubmitPcm(...);
```

```cpp
TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Receiver = FO3DTransportRegistry::Get().CreateReceiver(TEXT("MoQ"));
Receiver->SetConsumer(FrameConsumer);
Receiver->Initialize(Config);
Receiver->SetAudioSink(ReceiverAudioSink, AudioConfig);  // an IO3DReceiverAudioSink
Receiver->Start();

// Every game-thread tick
Receiver->Poll();
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    MoQ Transport Layer                       │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────────────┐              ┌──────────────────┐     │
│  │   MoQSender      │              │   MoQReceiver    │     │
│  │  ┌────────────┐  │              │  ┌────────────┐  │     │
│  │  │ Mocap Pub  │  │─────────────▶│  │ Mocap Sub  │  │     │
│  │  └────────────┘  │              │  └────────────┘  │     │
│  │  ┌────────────┐  │              │  ┌────────────┐  │     │
│  │  │ Audio Pub  │  │─────────────▶│  │ Audio Sub  │  │     │
│  │  └────────────┘  │              │  └────────────┘  │     │
│  └──────────────────┘              └──────────────────┘     │
│           │                                 │               │
│           ▼                                 ▼               │
│  ┌──────────────────────────────────────────────────────┐   │
│  │               MoQSessionWrapper                       │   │
│  │         (Thread-safe connection management)           │   │
│  └──────────────────────────────────────────────────────┘   │
│                            │                                 │
│                            ▼                                 │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                    moq-ffi (Rust)                     │   │
│  │            WebTransport / QUIC Transport              │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## Threading Model

- **Game Thread**: Initialize(), Start(), Stop(), Tick(), Poll()
- **Audio Thread**: SubmitPcm() (via audio sink)
- **Worker Thread**: Actual network publishing (sender). Frames, audio and control share one bounded send queue (`FO3DSendQueue`): a full frame budget (`queue_bytes`) refuses the newest frame, audio (1 MiB) and control (1,024 envelopes) have budgets of their own, and items whose track is not ready are dropped by the worker so a reconnect never replays a stale backlog
- **Receive path**: data callbacks are queued for `Poll()`, which hands them to the shared receive demux (consumer, audio sink, control sink); the receiver holds its consumer until `Stop()`
- **Connect**: `moq_connect` blocks, so it runs on a background task, never on the game thread
- **moq-ffi Callbacks**: Connection and data callbacks are queued and delivered on the game thread by a core ticker (`FMoQAsyncDispatcher`). The module stops the dispatcher before it unloads moq-ffi; callbacks that arrive later are dropped.

## Audio Support

Audio follows the same pattern as NNG transport:
1. PCM audio captured via `IO3DSenderAudioSink::SubmitPcm()`
2. Encoded to PCM16 or Opus using `O3DAudio::FFrameEncoder`
3. Published to dedicated audio track
4. Received and decoded using `O3DAudio::FFrameDecoder`, with the codec read from each frame's header (the receiver's own codec setting is not used for decoding)
5. Delivered via `IO3DReceiverAudioSink::SubmitPcm16()`

## Error Handling

The transport implements automatic reconnection with capped exponential backoff:
- Initial delay: 0.5 seconds
- Maximum delay: 10 seconds
- Backoff factor: 2x per failure
- Jitter: each delay is shortened by up to 25%, derived from a per-instance seed so that many instances do not retry in step

Every connect attempt uses a new moq-ffi client and ends in `CONNECTED` or `FAILED`, including error paths where moq-ffi returns an error without calling back. An attempt that has not finished after `connect_timeout` seconds is abandoned: its late callbacks are ignored and its client is closed when the blocking call returns. After a reconnect, namespaces are announced again, once per connection.

The receiver retries a failed subscribe (for example, when the publisher has not announced yet) with the same backoff instead of on every `Poll()`.

Connection state changes are broadcast via delegates on the game thread.

## Testing Seam

All moq-ffi calls go through `FMoQFfiApi` (`Public/MoQFfiApi.h`), a per-instance function table (ADR 0006, option F2). Production code uses `FMoQFfiApi::GetProduction()`. `FMoQSessionWrapper`, `FO3DMoQSender` and `FO3DMoQReceiver` each have a constructor that takes a table, and the sender and receiver also take a clock and a jitter seed, so tests run without a relay or sleeps. Tests live in the `Open3DBroadcastTests` module and reach the transport only through `Public/Testing/MoQTesting.h` (`CreateSenderForTest`, `CreateReceiverForTest`, the `FMoQTestSession` façade); the fake table is `Open3DBroadcastTests/Private/Transport/MoQ/MoQFakeFfi.h`.

## See Also

- Developer planning and review notes: `docs/dev/Open3DTransportMoQ/` in the Open3DBroadcast repository (historical; not part of the plugin package)
- [moq-ffi README](ThirdParty/moq-ffi/README.md)
- [O3DAudio Framework](../Open3DShared/Public/O3DAudioFrameCodec.h)
