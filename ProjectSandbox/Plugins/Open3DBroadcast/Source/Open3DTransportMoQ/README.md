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
- **moq-ffi Callbacks**: Data received callbacks (routed to queues)

## Audio Support

Audio follows the same pattern as NNG transport:
1. PCM audio captured via `IO3DSenderAudioSink::SubmitPcm()`
2. Encoded to PCM16 or Opus using `O3DAudio::FFrameEncoder`
3. Published to dedicated audio track
4. Received and decoded using `O3DAudio::FFrameDecoder`
5. Delivered via `IO3DReceiverAudioSink::SubmitPcm16()`

## Error Handling

The transport implements automatic reconnection with exponential backoff:
- Initial delay: 0.5 seconds
- Maximum delay: 10 seconds
- Backoff factor: 2x per failure

Connection state changes are broadcast via delegates.

## See Also

- [MoQ Transport Implementation Plan](../../MOQ_TRANSPORT_IMPLEMENTATION_PLAN.md)
- [moq-ffi README](ThirdParty/moq-ffi/README.md)
- [O3DAudio Framework](../Open3DShared/Public/O3DAudioFrameCodec.h)
