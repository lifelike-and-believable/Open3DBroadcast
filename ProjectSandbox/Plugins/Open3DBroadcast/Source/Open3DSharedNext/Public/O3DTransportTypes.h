#pragma once

#include "CoreMinimal.h"
#include "Open3DSharedNext.h"

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
        return FString::Printf(TEXT("[Transport=%s Role=%s Backend=%s Uri=%s StreamId=%s Advanced={%s} Token=%s]"),
            *Transport,
            *Role,
            *Backend,
            *Uri,
            *StreamId,
            *ParamsSummary,
            bPersistToken ? TEXT("<persist>") : (Token.IsEmpty() ? TEXT("<empty>") : TEXT("<provided>")));
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
