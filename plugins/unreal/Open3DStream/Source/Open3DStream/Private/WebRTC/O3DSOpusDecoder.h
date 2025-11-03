// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

// Forward declare Opus types (we'll include opus.h in the .cpp)
struct OpusDecoder;

/**
 * Minimal Opus decoder for WebRTC audio (RTP → PCM16).
 * 
 * Expects RTP payloads with PT=111 (Opus), decodes to PCM16 @ 48 kHz,
 * and publishes frames to UE audio (Audio Bus or component).
 * 
 * For MVP:
 * - No jitter buffer (decode per-packet).
 * - PLC via opus_decode for simple concealment.
 * - Non-48k input is dropped and logged (resampler can be follow-up).
 * 
 * All decoding occurs off the game thread; audio output is marshaled to the game thread.
 */
class FO3DSOpusDecoder : public TSharedFromThis<FO3DSOpusDecoder>
{
public:
    FO3DSOpusDecoder();
    ~FO3DSOpusDecoder();

    /**
     * Initialize the decoder with the expected format.
     * @param SampleRate - expected sample rate (48000 for MVP)
     * @param NumChannels - 1 or 2
     * @return true if decoder created successfully
     */
    bool Initialize(int32 SampleRate, int32 NumChannels);

    /**
     * Decode an RTP packet (12-byte header + Opus payload) to PCM16.
     * @param RtpBytes - full RTP packet bytes
     */
    void DecodeRtpPacket(const TArray<uint8>& RtpBytes);

    /**
     * Shutdown the decoder and release Opus resources.
     */
    void Shutdown();

    /**
     * @return true if decoder is initialized and ready
     */
    bool IsInitialized() const { return Decoder != nullptr; }

private:
    // Opus decoder instance
    OpusDecoder* Decoder = nullptr;

    // Decoder config
    int32 SampleRate = 48000;
    int32 NumChannels = 1;
    int32 FrameSizeSamples = 960; // 20 ms @ 48 kHz

    // Decode buffer (PCM16)
    TArray<int16> DecodedBuffer;

    // Parse RTP header (12 bytes) and extract payload
    // Returns payload pointer and size, or nullptr on error
    const uint8* ParseRtpHeader(const uint8* RtpBytes, int32 RtpLen, int32& OutPayloadLen, uint32& OutTimestamp, uint8& OutPayloadType);

    // Publish decoded PCM16 to UE audio (Audio Bus or component)
    void PublishAudio(const int16* Samples, int32 NumSamples, uint32 RtpTimestamp);

    // Throttled error logging
    double LastErrorLogTime = 0.0;
    static constexpr double ErrorLogThrottleSec = 2.0;
};
