# Open3DTransportMoQ

MoQ (Media over QUIC) transport implementation for Open3DBroadcast.

## Overview

Open3DTransportMoQ provides real-time motion capture and audio streaming over QUIC/WebTransport using the MoQ (Media over QUIC) protocol. It leverages the `moq-ffi` Rust library for reliable, low-latency transport.

### Key Features

- **Dual-Track Architecture**: Separate tracks for mocap and audio data
  - Mocap: `mocap/<session>/<track>`
  - Audio: `audio/<session>/<track>`
- **Opus/PCM16 Audio**: Configurable audio encoding via O3DAudio framework
- **WebTransport/QUIC**: Modern transport with built-in congestion control
- **Automatic Reconnection**: Exponential backoff for resilient connectivity

## Quick Start

### Sender Configuration

```cpp
FO3DTransportConfig Config;
Config.Uri = TEXT("https://relay.example.com:443");
Config.StreamId = TEXT("session1/character1");

// Optional advanced parameters
Config.AdvancedParams.Add(TEXT("delivery_mode"), TEXT("stream"));  // or "datagram"
Config.AdvancedParams.Add(TEXT("queue_bytes"), TEXT("8388608"));   // 8MB queue

TSharedPtr<IOpen3DSender> Sender = /* create via factory */;
Sender->Initialize(Config);
Sender->Start();

// For audio
TSharedPtr<IO3DSenderAudioSink> AudioSink = Sender->CreateAudioSink(AudioConfig);
AudioSink->SubmitPcm(...);
```

### Receiver Configuration

```cpp
FO3DTransportConfig Config;
Config.Uri = TEXT("https://relay.example.com:443");
Config.StreamId = TEXT("session1/character1");

TSharedPtr<IOpen3DReceiver> Receiver = /* create via factory */;
Receiver->SetConsumer(FrameConsumer);
Receiver->Initialize(Config);
Receiver->Start();

// For audio
Receiver->SetAudioSink(AudioSink, AudioConfig);

// Poll for data in game loop
Receiver->Poll();
```

## Configuration Options

| Key | Alt Keys | Description | Default |
|-----|----------|-------------|---------|
| `relay_url` | `moq.relay` | MoQ relay server URL | (from Uri) |
| `track_namespace` | `moq.namespace` | Track namespace | `mocap/<session>` |
| `track_name` | `moq.track` | Track name | (from StreamId) |
| `moq.session` | - | Session identifier | (from StreamId) |
| `delivery_mode` | `moq.delivery` | `stream` or `datagram` | `stream` |
| `queue_bytes` | `moq.queue_bytes`, `moq.qbytes` | Send queue size | 8MB |
| `connect_timeout` | `moq.connect_timeout` | Seconds before an unfinished connect attempt is abandoned and retried on a new client (clamped to 1-120) | 15 |

## Track Naming Convention

Tracks are automatically named based on configuration:

```
<type>/<session>/<track>

Examples:
  mocap/session1/character1   - Motion capture data
  audio/session1/character1   - Audio data
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
- **Worker Thread**: Actual network publishing (sender)
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

All moq-ffi calls go through `FMoQFfiApi` (`Private/Shared/MoQFfiApi.h`), a per-instance function table (ADR 0006, option F2). Production code uses `FMoQFfiApi::GetProduction()`. `FMoQSessionWrapper`, `FO3DMoQSender` and `FO3DMoQReceiver` each have a constructor that takes a table, and the sender and receiver also take a clock and a jitter seed, so tests run without a relay or sleeps (see `Private/Tests/MoQFakeFfi.h`).

## See Also

- [MoQ Transport Implementation Plan](../../MOQ_TRANSPORT_IMPLEMENTATION_PLAN.md)
- [moq-ffi README](ThirdParty/moq-ffi/README.md)
- [O3DAudio Framework](../Open3DShared/Public/O3DAudioFrameCodec.h)
