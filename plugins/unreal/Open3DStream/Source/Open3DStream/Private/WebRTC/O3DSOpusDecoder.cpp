// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebRTC/O3DSOpusDecoder.h"
#include "O3DSAudioBus.h"
#include "O3DSUnifiedMessage.h"
#include "O3DSStreamLogs.h"
#include "Async/Async.h"
#include "HAL/PlatformTime.h"

// Include Opus headers (assumes thirdparty/opus or similar is in include path)
#include "opus.h"

// CVars for audio decoder logging
static TAutoConsoleVariable<int32> CVarO3DSReceiverAudioLog(
    TEXT("o3ds.Receiver.Audio.Log"),
    0,
    TEXT("Enable verbose logging for Opus audio decoder (0/1)."),
    ECVF_Default);

FO3DSOpusDecoder::FO3DSOpusDecoder()
{
}

FO3DSOpusDecoder::~FO3DSOpusDecoder()
{
    Shutdown();
}

bool FO3DSOpusDecoder::Initialize(int32 InSampleRate, int32 InNumChannels)
{
    if (Decoder)
    {
        UE_LOG(LogTemp, Warning, TEXT("O3DS Opus Decoder: already initialized"));
        return false;
    }

    // For MVP, only support 48 kHz
    if (InSampleRate != 48000)
    {
        UE_LOG(LogO3DSReceiverAudio, Error, TEXT("unsupported sample rate %d (only 48000 supported)"), InSampleRate);
        return false;
    }

    SampleRate = InSampleRate;
    NumChannels = FMath::Clamp(InNumChannels, 1, 2);
    FrameSizeSamples = (SampleRate * 20) / 1000; // 20 ms

    int32 Error = 0;
    Decoder = opus_decoder_create(SampleRate, NumChannels, &Error);
    if (Error != OPUS_OK || !Decoder)
    {
        UE_LOG(LogO3DSReceiverAudio, Error, TEXT("opus_decoder_create failed (error=%d)"), Error);
        return false;
    }

    // Allocate decode buffer (max frame size + headroom)
    DecodedBuffer.SetNumUninitialized(FrameSizeSamples * NumChannels * 2);

    if (CVarO3DSReceiverAudioLog->GetInt() != 0)
    {
        UE_LOG(LogO3DSReceiverAudio, Log, TEXT("initialized (rate=%d, channels=%d, frame=%d samples)"),
            SampleRate, NumChannels, FrameSizeSamples);
    }

    return true;
}

void FO3DSOpusDecoder::Shutdown()
{
    if (Decoder)
    {
        opus_decoder_destroy(Decoder);
        Decoder = nullptr;

        if (CVarO3DSReceiverAudioLog->GetInt() != 0)
        {
            UE_LOG(LogO3DSReceiverAudio, Log, TEXT("shutdown"));
        }
    }
}

void FO3DSOpusDecoder::DecodeRtpPacket(const TArray<uint8>& RtpBytes)
{
    if (!Decoder)
        return;

    // Parse RTP header
    int32 PayloadLen = 0;
    uint32 Timestamp = 0;
    uint8 PayloadType = 0;
    const uint8* Payload = ParseRtpHeader(RtpBytes.GetData(), RtpBytes.Num(), PayloadLen, Timestamp, PayloadType);

    if (!Payload)
    {
        // Throttled error
        const double Now = FPlatformTime::Seconds();
        if (Now - LastErrorLogTime > ErrorLogThrottleSec)
        {
            UE_LOG(LogO3DSReceiverAudio, Warning, TEXT("invalid RTP packet (size=%d)"), RtpBytes.Num());
            LastErrorLogTime = Now;
        }
        return;
    }

    // Verify PT=111 (Opus)
    if (PayloadType != 111)
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - LastErrorLogTime > ErrorLogThrottleSec)
        {
            UE_LOG(LogO3DSReceiverAudio, Warning, TEXT("unexpected payload type %d (expected 111)"), PayloadType);
            LastErrorLogTime = Now;
        }
        return;
    }

    // Decode Opus payload
    const int32 SamplesDecoded = opus_decode(
        Decoder,
        Payload,
        PayloadLen,
        DecodedBuffer.GetData(),
        FrameSizeSamples,
        0 // no FEC for MVP
    );

    if (SamplesDecoded < 0)
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - LastErrorLogTime > ErrorLogThrottleSec)
        {
            UE_LOG(LogO3DSReceiverAudio, Warning, TEXT("opus_decode failed (error=%d)"), SamplesDecoded);
            LastErrorLogTime = Now;
        }
        return;
    }

    if (CVarO3DSReceiverAudioLog->GetInt() != 0)
    {
        UE_LOG(LogO3DSReceiverAudio, Verbose, TEXT("decoded %d samples (payload=%d bytes, ts=%u)"),
            SamplesDecoded, PayloadLen, Timestamp);
    }

    // Publish audio to UE
    PublishAudio(DecodedBuffer.GetData(), SamplesDecoded * NumChannels, Timestamp);
}

const uint8* FO3DSOpusDecoder::ParseRtpHeader(const uint8* RtpBytes, int32 RtpLen, int32& OutPayloadLen, uint32& OutTimestamp, uint8& OutPayloadType)
{
    // RTP header is 12 bytes minimum
    if (RtpLen < 12)
        return nullptr;

    // Byte 0: V(2) P(1) X(1) CC(4)
    const uint8 V = (RtpBytes[0] >> 6) & 0x03;
    const uint8 P = (RtpBytes[0] >> 5) & 0x01;
    const uint8 X = (RtpBytes[0] >> 4) & 0x01;
    const uint8 CC = RtpBytes[0] & 0x0F;

    if (V != 2)
        return nullptr; // invalid version

    // Byte 1: M(1) PT(7)
    OutPayloadType = RtpBytes[1] & 0x7F;

    // Bytes 4-7: timestamp (big-endian)
    OutTimestamp = ((uint32)RtpBytes[4] << 24) | ((uint32)RtpBytes[5] << 16) | ((uint32)RtpBytes[6] << 8) | (uint32)RtpBytes[7];

    // Header size: 12 + (CC * 4) + extension if X=1
    int32 HeaderSize = 12 + (CC * 4);

    if (X)
    {
        // Extension: 2 bytes (defined by profile) + 2 bytes (length in 32-bit words)
        if (RtpLen < HeaderSize + 4)
            return nullptr;
        const uint16 ExtLen = ((uint16)RtpBytes[HeaderSize + 2] << 8) | (uint16)RtpBytes[HeaderSize + 3];
        HeaderSize += 4 + (ExtLen * 4);
    }

    if (RtpLen < HeaderSize)
        return nullptr;

    // Payload is after header (padding handled by P flag if needed; for MVP, ignore)
    OutPayloadLen = RtpLen - HeaderSize;
    return RtpBytes + HeaderSize;
}

void FO3DSOpusDecoder::PublishAudio(const int16* Samples, int32 NumSamples, uint32 RtpTimestamp)
{
    // Marshal to game thread for Audio Bus publish
    // Copy samples to avoid lifetime issues across threads
    TArray<int16> SamplesCopy;
    SamplesCopy.Append(Samples, NumSamples);
    
    const int32 LocalChannels = NumChannels;
    const int32 LocalRate = SampleRate;

    AsyncTask(ENamedThreads::GameThread, [SamplesCopy, LocalChannels, LocalRate, RtpTimestamp]()
    {
        // Build metadata for Audio Bus
        O3DS::FAudioFrameMeta Meta;
        // Use a stream label that passes the RemoteAudio Mix-mode filter (starts with "o3ds:mix")
        Meta.StreamLabel = TEXT("o3ds:mix:webrtc");
        Meta.SubjectName = TEXT("WebRTC");
        Meta.NumChannels = LocalChannels;
        Meta.SampleRate = LocalRate;
        Meta.TimestampSec = (double)RtpTimestamp / (double)LocalRate; // approximate

        // Publish to Audio Bus (PCM16 bytes)
        const uint8* BytePtr = reinterpret_cast<const uint8*>(SamplesCopy.GetData());
        const int32 NumBytes = SamplesCopy.Num() * sizeof(int16);

        FO3DSAudioBus::PublishPcm16(Meta, BytePtr, NumBytes);

        if (CVarO3DSReceiverAudioLog->GetInt() != 0)
        {
            UE_LOG(LogO3DSReceiverAudio, Verbose, TEXT("published %d samples (%d bytes) to Audio Bus"),
                SamplesCopy.Num(), NumBytes);
        }
    });
}
