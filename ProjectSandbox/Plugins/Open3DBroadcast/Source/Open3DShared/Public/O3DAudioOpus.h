// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DUnifiedMessage.h"

struct OpusEncoder;
struct OpusDecoder;

/**
 * Lightweight wrapper around libOpus encoder state used by transports.
 *
 * Opus only accepts whole Opus frames: Encode() must be given exactly GetFrameSizeSamples()
 * frames per call. O3DAudio::FFrameEncoder accumulates arbitrary capture buffers into packets
 * of that size (SHR-2). Not copyable: it owns the libOpus state. Not thread-safe: one thread
 * uses an instance.
 */
class OPEN3DSHARED_API FO3DAudioOpusEncoder
{
public:
    struct FSettings
    {
        /** 8000, 12000, 16000, 24000 or 48000 Hz. */
        int32 SampleRate = 48000;
        /** 1 or 2. */
        int32 NumChannels = 1;
        int32 BitrateKbps = 64;
        /** Opus frame duration: 5, 10, 20, 40 or 60 ms (SHR-31). */
        int32 FrameSizeMs = 20;
        int32 Complexity = 5;
        bool bUseVariableBitrate = true;
    };

    /** Largest packet Encode() can produce; the size libOpus recommends for one packet (SHR-31). */
    static constexpr int32 MaxPacketBytes = 4000;

    FO3DAudioOpusEncoder();
    ~FO3DAudioOpusEncoder();

    FO3DAudioOpusEncoder(const FO3DAudioOpusEncoder&) = delete;
    FO3DAudioOpusEncoder& operator=(const FO3DAudioOpusEncoder&) = delete;

    /**
     * Initialize encoder with the supplied settings. Returns false and fills OutError when the
     * settings are not legal for Opus, when libOpus rejects them, or when a ctl call fails.
     */
    bool Initialize(const FSettings& InSettings, FString& OutError);

    /** Reset and release the underlying encoder. Safe to call multiple times. */
    void Reset();

    /**
     * Encode exactly GetFrameSizeSamples() frames of interleaved float PCM into one packet.
     * OutPayload is resized to the packet size without shrinking its allocation, so a reused
     * scratch array does not reallocate (SHR-18). Returns false on failure.
     */
    bool Encode(const float* InterleavedPCM, int32 NumFrames, TArray<uint8>& OutPayload, int32& OutFramesEncoded);

    /** Whether the encoder is ready for Encode calls. */
    bool IsInitialized() const { return Encoder != nullptr; }

    const FSettings& GetSettings() const { return Settings; }

    /** Frames per channel in one packet: SampleRate * FrameSizeMs / 1000. 0 before Initialize. */
    int32 GetFrameSizeSamples() const { return FrameSizeSamples; }

    /** Encoder lookahead (algorithmic delay) in frames per channel, from OPUS_GET_LOOKAHEAD. */
    int32 GetLookaheadSamples() const { return LookaheadSamples; }

    /** True when SampleRate is one Opus can encode or decode at. */
    static bool IsSupportedSampleRate(int32 SampleRate);

    /** True when FrameSizeMs is a legal Opus frame duration that an int32 can express. */
    static bool IsSupportedFrameSizeMs(int32 FrameSizeMs);

private:
    FSettings Settings;
    OpusEncoder* Encoder = nullptr;
    int32 FrameSizeSamples = 0;
    int32 LookaheadSamples = 0;
};

/**
 * Lightweight wrapper around libOpus decoder state used by transports. Not copyable: it owns
 * the libOpus state. Not thread-safe: one thread uses an instance.
 */
class OPEN3DSHARED_API FO3DAudioOpusDecoder
{
public:
    struct FSettings
    {
        int32 SampleRate = 48000;
        int32 NumChannels = 1;
        /**
         * Largest packet duration the decoder accepts, in ms. Opus packets carry up to 120 ms,
         * so that is the default (SHR-31). Clamped to [10, 120].
         */
        int32 FrameSizeMs = 120;
        bool bEnableFec = false;
    };

    FO3DAudioOpusDecoder();
    ~FO3DAudioOpusDecoder();

    FO3DAudioOpusDecoder(const FO3DAudioOpusDecoder&) = delete;
    FO3DAudioOpusDecoder& operator=(const FO3DAudioOpusDecoder&) = delete;

    bool Initialize(const FSettings& InSettings, FString& OutError);
    void Reset();

    /**
     * Decode Opus payload into PCM16. OutPcm16 is resized without shrinking its allocation, so a
     * reused scratch array does not reallocate per packet (SHR-18). Returns false on failure.
     */
    bool Decode(const uint8* EncodedData, int32 NumBytes, TArray<int16>& OutPcm16, int32& OutFramesDecoded);

    /**
     * Packet-loss concealment: synthesise NumFrames frames per channel for a lost packet
     * (SHR-31). NumFrames must be a multiple of 2.5 ms at the decoder's rate. Returns false on
     * failure.
     */
    bool DecodeLost(int32 NumFrames, TArray<int16>& OutPcm16, int32& OutFramesDecoded);

    bool IsInitialized() const { return Decoder != nullptr; }

    const FSettings& GetSettings() const { return Settings; }

    /** Largest number of frames per channel one Decode call can return. */
    int32 GetMaxFrameSizeSamples() const { return MaxFrameSizeSamples; }

private:
    FSettings Settings;
    OpusDecoder* Decoder = nullptr;
    int32 MaxFrameSizeSamples = 0;
};
