# UE Audio Components (Sender/Receiver)

This document outlines the UE-facing components for capturing and playing audio associated with LiveLink subjects.

## Sender: UO3DSBroadcastAudioCaptureComponent

Purpose: Capture game mix and/or microphone audio and send via the active WebRTC backend.

Key properties:
- Source: GameSubmix | Microphone | Game+Mic
- SubmixToTap: USoundSubmix (null = Master)
- MicrophoneDeviceHint: string (optional)
- SampleRate: default 48 kHz
- NumChannels: 1 or 2
- BitrateKbps: target Opus bitrate (e.g., 64 kbps for mono voice)
- Gains: GameGain, MicGain
- SubjectName: LiveLink subject to associate (empty for global game mix)

Flow:
1) Attach ISubmixBufferListener to the chosen Submix (or Master) for game audio
2) Initialize AudioCapture for mic (optional)
3) Resample to 48 kHz as needed (Audio::FResampler)
4) Push 10–20 ms frames (float interleaved) to `Connector->PushPcm(StreamLabel, ...)`
5) Build `StreamLabel` as:
   - `o3ds:mix` (game mix)
   - `o3ds:subject/<SubjectName>` (mic)

## Receiver: UO3DSRemoteAudioComponent

Purpose: Play remote audio for a specific subject (or the global mix).

Key properties:
- SubjectName: LiveLink subject to filter; empty => consumes `o3ds:mix`
- OutputSubmix: optional routing
- Volume, Mute

Flow:
1) Bind to `Connector->OnRemoteAudio()` delegate
2) On frame: if Subject matches (or mix), write to a ring buffer in USoundWaveProcedural
3) AudioComponent plays the procedural wave; optional OutputSubmix for routing

## Example usage

```cpp
// Broadcast: add one component for game mix, and one per actor mic if desired
auto* MixCapture = Actor->AddComponentByClass(UO3DSBroadcastAudioCaptureComponent::StaticClass(), false, FTransform::Identity, true);
MixCapture->Config.Source = EO3DSAudioCaptureSource::GameSubmix;
MixCapture->SubjectName = NAME_None; // global
MixCapture->Config.BitrateKbps = 128;

// Mic per subject
auto* MicCapture = Actor->AddComponentByClass(UO3DSBroadcastAudioCaptureComponent::StaticClass(), false, FTransform::Identity, true);
MicCapture->Config.Source = EO3DSAudioCaptureSource::Microphone;
MicCapture->SubjectName = FName(TEXT("Actor_1"));
MicCapture->Config.BitrateKbps = 64;

// Receiver: attach to an actor driven by LiveLink subject "Actor_1"
auto* Remote = Actor->AddComponentByClass(UO3DSRemoteAudioComponent::StaticClass(), false, FTransform::Identity, true);
Remote->SubjectName = FName(TEXT("Actor_1"));
```

## Notes

- Keep sender work off the Game Thread; buffer handoffs between Audio Render Thread and a worker feeding WebRTC
- If both game and mic are enabled, mix on a worker with per-source gains
- Consider a lightweight registry so components find the active connector created by the transport/source