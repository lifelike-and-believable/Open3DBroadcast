# WebRTC Connector Interface

This document describes the backend-agnostic connector interface used by the WebRTC transport. It enables swapping between P2P (libdatachannel) and LiveKit SFU without changing UE components.

## Interface

```cpp
class IWebRTCConnector
{
public:
  virtual ~IWebRTCConnector() = default;

  // Lifecycle
  virtual bool Start(const FString& Url, bool bIsServer) = 0;
  virtual void Stop() = 0;
  virtual bool IsConnected() const = 0;
  virtual void Tick() {}

  // Data
  virtual bool SendDataReliable(const uint8* Data, int32 Size) = 0;
  virtual bool SendDataLossy(const uint8* Data, int32 Size) = 0;

  // Audio
  struct FAudioSendConfig
  {
    bool bEnable = false;
    int32 SampleRate = 48000;
    int32 NumChannels = 1;
    int32 BitrateKbps = 64;
    bool bUseDTX = true;

    FString StreamLabel; // e.g., o3ds:subject/Actor_1 or o3ds:mix
    FString TrackLabel;  // unique id (optional)
    FString SubjectName; // convenience mirror for announce/UI
    FString SourceType;  // "mic" | "mix"
  };
  virtual bool EnableAudioSend(const FAudioSendConfig&) { return false; }
  virtual bool PushPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) { return false; }

  // Remote audio (subject-aware)
  DECLARE_DELEGATE_SixParams(FOnRemoteAudio, const FString&, const FString&, const float*, int32, int32, int32);
  virtual FOnRemoteAudio& OnRemoteAudio() = 0;
};
```

Factory:
```cpp
std::shared_ptr<IWebRTCConnector> CreateWebRTCConnector(EO3DSWebRtcBackend Backend, const FLO3DSLiveKitConfig* LiveKitCfgIfAny);
```

## Backend mapping

- LibDataChannelConnector
  - Start/Stop: mirrors current FWebRTCConnector behavior
  - Data: SendDataReliable/Lossy map to the single DataChannel (unreliable/ordered toggles may be added later)
  - Audio: requires libdatachannel built with USE_MEDIA=ON, USE_OPUS=ON; add audio source/track and PushPcm in 10–20 ms chunks at 48 kHz

- LiveKitConnector
  - Start/Stop: join LiveKit room (ServerUrl, Token), enter `Cfg.Room`
  - Data:
    - SendDataReliable → LiveKit reliable data
    - SendDataLossy → LiveKit lossy data
    - Topic: `o3ds.anim`, `o3ds.ctrl`, `o3ds.audio.announce` (application-layer topic header recommended)
  - Audio: publish track(s) with Track.Name = `StreamLabel`; push PCM into the publication

## Message header (recommended)

Add a tiny header to every message (both backends):

```json
{
  "topic": "o3ds.anim",
  "v": 1,
  "seq": 12345,
  "ts": 1730000000.123,     // seconds
  "subject": "Actor_1",     // optional
  "stream": "o3ds:subject/Actor_1" // optional
}
```

For lossy messages, drop any frame with an older `seq`/`ts` than the latest applied.

## Backpressure policy

- Lossy: queue size <= 2 frames (or <= 50 ms). Drop oldest on overflow.
- Reliable: low-rate control only. If queue backlog > small threshold, coalesce or defer non-critical messages.

## Size limits

- Keep messages <= 15 KB to avoid SCTP fragmentation and HOL blocking on reliable paths. Chunk rare large control messages with reassembly and timeout.