#pragma once

#include "CoreMinimal.h"

/** Lightweight audio configuration shared between the sender component and transports. */ 
struct FO3DTransportAudioConfig
{
    /** Toggle audio capture/sending. */
    bool bEnableAudio = false;

    /** Preferred audio codec identifier (e.g. "PCM16", "Opus"). Empty uses transport default. */
    FString Codec;

    /** Target PCM sample rate the transport expects from the capture path. */
    int32 SampleRate = 48000;

    /** Target channel count (1 = mono, 2 = stereo). */
    int32 NumChannels = 1;

    /** Desired encoded bitrate in kbps (transport may clamp). */
    int32 BitrateKbps = 64;

    /** Optional transport-specific capture mode hint (e.g. "mix", "input"). */
    FString Mode;

    /** Optional friendly label used when publishing audio streams. */
    FString StreamLabel;

    /** Optional input device identifier when capturing microphone input. */
    FString InputDevice;

    /** Additional transport specific overrides. */
    TMap<FString, FString> AdvancedParams;

    FString ToDebugString() const
    {
        return FString::Printf(TEXT("[Enabled=%d Codec=%s SR=%d Ch=%d Bitrate=%d Mode=%s Device=%s Label=%s Adv=%d]"),
            bEnableAudio ? 1 : 0,
            Codec.IsEmpty() ? TEXT("<default>") : *Codec,
            SampleRate,
            NumChannels,
            BitrateKbps,
            *Mode,
            *InputDevice,
            *StreamLabel,
            AdvancedParams.Num());
    }
};

/**
 * Canonical configuration parameters used by transport implementations. Values are intentionally
 * high-level; transports may choose to interpret/augment them as needed.
 */
struct FO3DTransportConfig
{
    /** Canonical transport identifier (e.g. "sockets", "webrtc", "loopback"). */
    FString Transport;

    /** Role specific to the transport (e.g. "sender"/"receiver", "pub"/"sub"). */
    FString Role;

    /** Backend hint (e.g. "livekit", "libdc"). Optional. */
    FString Backend;

    /** Canonical URI representation for the endpoint. */
    FString Uri;

    /** Optional secondary identifier (room name, stream id, channel name, etc.). */
    FString StreamId;

    /** Authentication token or credential material, if required. */
    FString Token;

    /**
     * Whether Token should persist to disk. Default false so callers explicitly opt-in to storing
     * credentials in assets/config files.
     */
    bool bPersistToken = false;

    /** Transport-specific advanced key/value overrides. Keys are case-insensitive. */
    TMap<FString, FString> AdvancedParams;

    /** Optional audio configuration shared with transports that support audio. */
    FO3DTransportAudioConfig Audio;

    /** Build a concise debug string summarising the configuration (secrets redacted). */
    FString ToDebugString() const
    {
        FString ParamsSummary;
        if (AdvancedParams.Num() > 0)
        {
            TArray<FString> Pairs;
            Pairs.Reserve(AdvancedParams.Num());
            for (const TPair<FString, FString>& Pair : AdvancedParams)
            {
                Pairs.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *Pair.Value));
            }
            ParamsSummary = FString::Join(Pairs, TEXT(","));
        }
        FString AudioSummary;
        if (Audio.bEnableAudio)
        {
            AudioSummary = Audio.ToDebugString();
        }
        else
        {
            AudioSummary = TEXT("[Enabled=0]");
        }

        return FString::Printf(TEXT("[Transport=%s Role=%s Backend=%s Uri=%s StreamId=%s Advanced={%s} Token=%s Audio=%s]"),
            *Transport,
            *Role,
            *Backend,
            *Uri,
            *StreamId,
            *ParamsSummary,
            bPersistToken ? TEXT("<persist>") : (Token.IsEmpty() ? TEXT("<empty>") : TEXT("<provided>")),
            *AudioSummary);
    }
};

/** Lightweight stats aggregated by transports for diagnostics. */
struct FO3DTransportStats
{
    int64 FramesSent = 0;
    int64 FramesReceived = 0;
    int64 BytesSent = 0;
    int64 BytesReceived = 0;
    int64 DroppedFrames = 0;
    double AverageLatencyMs = 0.0;
    double MaxLatencyMs = 0.0;

    void Reset()
    {
        FramesSent = 0;
        FramesReceived = 0;
        BytesSent = 0;
        BytesReceived = 0;
        DroppedFrames = 0;
        AverageLatencyMs = 0.0;
        MaxLatencyMs = 0.0;
    }
};
