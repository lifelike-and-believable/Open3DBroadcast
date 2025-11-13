#pragma once

#include "CoreMinimal.h"
#include "O3DUnifiedMessage.h"

struct OpusEncoder;
struct OpusDecoder;

/**
 * Lightweight wrapper around libOpus encoder state used by transports.
 */
class OPEN3DSHARED_API FO3DAudioOpusEncoder
{
public:
    struct FSettings
    {
        int32 SampleRate = 48000;
        int32 NumChannels = 1;
        int32 BitrateKbps = 64;
        int32 FrameSizeMs = 20;
        int32 Complexity = 5;
        bool bUseVariableBitrate = true;
    };

    FO3DAudioOpusEncoder();
    ~FO3DAudioOpusEncoder();

    /** Initialize encoder with the supplied settings. Returns false and fills OutError on failure. */
    bool Initialize(const FSettings& InSettings, FString& OutError);

    /** Reset and release the underlying encoder. Safe to call multiple times. */
    void Reset();

    /** Encode interleaved float PCM into Opus payload. Returns false on failure. */
    bool Encode(const float* InterleavedPCM, int32 NumFrames, TArray<uint8>& OutPayload, int32& OutFramesEncoded);

    /** Whether the encoder is ready for Encode calls. */
    bool IsInitialized() const { return Encoder != nullptr; }

    const FSettings& GetSettings() const { return Settings; }

private:
    FSettings Settings;
    OpusEncoder* Encoder = nullptr;
    int32 MaxPacketBytes = 0;
};

/**
 * Lightweight wrapper around libOpus decoder state used by transports.
 */
class OPEN3DSHARED_API FO3DAudioOpusDecoder
{
public:
    struct FSettings
    {
        int32 SampleRate = 48000;
        int32 NumChannels = 1;
        int32 FrameSizeMs = 60;
        bool bEnableFec = false;
    };

    FO3DAudioOpusDecoder();
    ~FO3DAudioOpusDecoder();

    bool Initialize(const FSettings& InSettings, FString& OutError);
    void Reset();

    /** Decode Opus payload into PCM16. Returns false on failure. */
    bool Decode(const uint8* EncodedData, int32 NumBytes, TArray<int16>& OutPcm16, int32& OutFramesDecoded);

    bool IsInitialized() const { return Decoder != nullptr; }

    const FSettings& GetSettings() const { return Settings; }

private:
    FSettings Settings;
    OpusDecoder* Decoder = nullptr;
    int32 MaxFrameSizeSamples = 0;
};
