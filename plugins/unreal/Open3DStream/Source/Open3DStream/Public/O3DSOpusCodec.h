// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

/**
 * Thin RAII wrappers around libopus encoder/decoder.
 *
 * Usage:
 *  - Initialize with desired sample rate (8k..48k) and channels (1 or 2)
 *  - Encode: interleaved float PCM [-1,1] -> Opus packet bytes
 *  - Decode: Opus packet bytes -> PCM16 interleaved bytes
 *
 * Guarded by O3DS_WITH_OPUS. When not available, methods return false.
 */
namespace O3DS
{
    class OPEN3DSTREAM_API FOpusEncoder
    {
    public:
        FOpusEncoder();
        ~FOpusEncoder();

        bool Init(int32 SampleRate, int32 NumChannels, int32 BitrateKbps = 64, bool bUseDtx = true);
        void Reset();

        // Encode interleaved float PCM to a single Opus packet.
        // NumFrames is per-channel samples (e.g., 480 for 10 ms @ 48 kHz)
        bool Encode(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate,
                    TArray<uint8>& OutPacket);

        int32 GetSampleRate() const { return SampleRate; }
        int32 GetNumChannels() const { return Channels; }

    private:
        void* Enc = nullptr; // OpusEncoder*
        int32 SampleRate = 0;
        int32 Channels = 0;
        int32 Bitrate = 0; // bps
        bool bDtx = true;
    };

    class OPEN3DSTREAM_API FOpusDecoder
    {
    public:
        FOpusDecoder();
        ~FOpusDecoder();

        bool Init(int32 SampleRate, int32 NumChannels);
        void Reset();

        // Decode Opus packet to interleaved PCM16 bytes.
        // Returns true and fills OutPcm16 on success.
        bool DecodeToPcm16(const uint8* PacketData, int32 PacketBytes,
                           TArray<uint8>& OutPcm16, int32& OutNumFrames);

        int32 GetSampleRate() const { return SampleRate; }
        int32 GetNumChannels() const { return Channels; }

    private:
        void* Dec = nullptr; // OpusDecoder*
        int32 SampleRate = 0;
        int32 Channels = 0;
    };
}
