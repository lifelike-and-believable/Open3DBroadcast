# WebRTC Audio ↔ LiveLink Subject Association

This document defines the conventions for associating microphone/game audio with LiveLink subjects over WebRTC in both P2P and LiveKit backends.

## Goals

- Maintain a stable subject↔audio mapping independent of SFU rewrites
- Support multiple subjects and a global game mix concurrently
- Keep animation data on the Data path (DataChannel or LiveKit data)

## Track labeling

- Game mix:
  - Label: `o3ds:mix`
- Per-subject microphone:
  - Label: `o3ds:subject/<SubjectName>`

Use these labels as:
- P2P (libdatachannel): MediaStream ID (msid) when adding the audio track
- LiveKit: Track.Name when publishing the audio track

Track ID can be any unique string; it is not part of the routing convention.

## Announce message

On connection (and whenever tracks change), the sender broadcasts an announce JSON over the reliable data path:

```json
{
  "type": "o3ds.audio.announce",
  "tracks": [
    { "stream": "o3ds:mix", "track": "o3ds:audio/mix/0a1b…", "subject": "", "source": "mix", "sr": 48000, "ch": 2, "br": 128 },
    { "stream": "o3ds:subject/Actor_1", "track": "o3ds:audio/mic/ab12…", "subject": "Actor_1", "source": "mic", "sr": 48000, "ch": 1, "br": 64 }
  ]
}
```

Fields:
- stream: the label used for routing (`o3ds:mix` or `o3ds:subject/<Name>`)
- subject: the LiveLink subject name (empty for mix)
- source: one of `mic` or `mix`
- sr/ch/br: nominal sample rate, channels, and target bitrate (kbps)

## Receiver mapping

- Preferred (LiveKit): map by Track.Name
- Preferred (P2P): map by MediaStream ID (msid)
- Fallback: use the `o3ds.audio.announce` table to resolve subject association

Components:
- UO3DSRemoteAudioComponent(SubjectName): plays only frames whose subject matches (or global `o3ds:mix` if SubjectName is empty)
- Global mix component: create a component expecting only `o3ds:mix`

## Edge cases

- If a subject’s mic track drops and is republished, the sender must re-emit `o3ds.audio.announce`.
- If msid/Track.Name are rewritten by an SFU (e.g., some servers), the announce remains authoritative.

## Example UE snippet (receive binding, pseudo-code)

```cpp
Connector->OnRemoteAudio().BindLambda(
  [Weak = TWeakObjectPtr<UO3DSRemoteAudioComponent>(this)]
  (const FString& Subject, const FString& Stream, const float* Data, int32 Frames, int32 Ch, int32 SR)
{
  if (auto* Self = Weak.Get())
  {
    const bool IsMix = Stream.StartsWith(TEXT("o3ds:mix"));
    const bool Match = (Self->SubjectName.IsNone() && IsMix) ||
                       (Self->SubjectName.ToString().Equals(Subject, ESearchCase::IgnoreCase));
    if (Match && Self->Wave) { Self->Wave->PushPcm(Data, Frames, Ch); }
  }
});
```