// Copyright (c) Open3DStream Contributors

#include "O3DSOpusCodec.h"

#if O3DS_WITH_OPUS
    #include <opus/opus.h>
#endif

namespace O3DS
{
    // ======================= Encoder =======================
    FOpusEncoder::FOpusEncoder() = default;
    FOpusEncoder::~FOpusEncoder() { Reset(); }

    bool FOpusEncoder::Init(int32 InSampleRate, int32 InNumChannels, int32 BitrateKbps, bool bUseDtx)
    {
#if O3DS_WITH_OPUS
        Reset();
        int Err = 0;
        OpusEncoder* E = opus_encoder_create(InSampleRate, InNumChannels, OPUS_APPLICATION_AUDIO, &Err);
        if (!E || Err != OPUS_OK)
        {
            return false;
        }
        // Configure bitrate and DTX
        opus_encoder_ctl(E, OPUS_SET_BITRATE(BitrateKbps * 1000));
        opus_encoder_ctl(E, OPUS_SET_VBR(1));
        opus_encoder_ctl(E, OPUS_SET_DTX(bUseDtx ? 1 : 0));

        Enc = (void*)E;
        SampleRate = InSampleRate;
        Channels = InNumChannels;
        Bitrate = BitrateKbps * 1000;
        bDtx = bUseDtx;
        return true;
#else
        (void)InSampleRate; (void)InNumChannels; (void)BitrateKbps; (void)bUseDtx; return false;
#endif
    }

    void FOpusEncoder::Reset()
    {
#if O3DS_WITH_OPUS
        if (Enc)
        {
            opus_encoder_destroy((OpusEncoder*)Enc);
            Enc = nullptr;
        }
#endif
        SampleRate = 0; Channels = 0; Bitrate = 0; bDtx = true;
    }

    bool FOpusEncoder::Encode(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 InSampleRate,
                               TArray<uint8>& OutPacket)
    {
#if O3DS_WITH_OPUS
        if (!Enc || NumChannels != Channels || InSampleRate != SampleRate || !Interleaved || NumFrames <= 0)
        {
            return false;
        }
        // Opus packet worst-case size is ~1275 bytes per frame, but allocate conservatively for 60ms stereo
        constexpr int32 MaxPacketBytes = 4096;
        OutPacket.SetNumUninitialized(MaxPacketBytes);
        int Bytes = opus_encode_float((OpusEncoder*)Enc, Interleaved, NumFrames, OutPacket.GetData(), MaxPacketBytes);
        if (Bytes < 0)
        {
            OutPacket.Reset();
            return false;
        }
        OutPacket.SetNum(Bytes, /*bAllowShrinking*/true);
        return true;
#else
        (void)Interleaved; (void)NumFrames; (void)NumChannels; (void)InSampleRate; (void)OutPacket; return false;
#endif
    }

    // ======================= Decoder =======================
    FOpusDecoder::FOpusDecoder() = default;
    FOpusDecoder::~FOpusDecoder() { Reset(); }

    bool FOpusDecoder::Init(int32 InSampleRate, int32 InNumChannels)
    {
#if O3DS_WITH_OPUS
        Reset();
        int Err = 0;
        OpusDecoder* D = opus_decoder_create(InSampleRate, InNumChannels, &Err);
        if (!D || Err != OPUS_OK)
        {
            return false;
        }
        Dec = (void*)D;
        SampleRate = InSampleRate;
        Channels = InNumChannels;
        return true;
#else
        (void)InSampleRate; (void)InNumChannels; return false;
#endif
    }

    void FOpusDecoder::Reset()
    {
#if O3DS_WITH_OPUS
        if (Dec)
        {
            opus_decoder_destroy((OpusDecoder*)Dec);
            Dec = nullptr;
        }
#endif
        SampleRate = 0; Channels = 0;
    }

    bool FOpusDecoder::DecodeToPcm16(const uint8* PacketData, int32 PacketBytes,
                                      TArray<uint8>& OutPcm16, int32& OutNumFrames)
    {
#if O3DS_WITH_OPUS
        if (!Dec || !PacketData || PacketBytes <= 0)
        {
            return false;
        }
        // Opus allows up to 120 ms frames. Allocate a safe buffer: 120ms @ 48k = 5760 frames.
        const int32 MaxFrames = FMath::Max(120 * SampleRate / 1000, 960); // at least 20ms
        const int32 MaxSamples = MaxFrames * Channels;
        OutPcm16.SetNumUninitialized(MaxSamples * sizeof(int16));
        int16* Pcm = reinterpret_cast<int16*>(OutPcm16.GetData());

        const int DecodedFrames = opus_decode((OpusDecoder*)Dec, PacketData, PacketBytes, Pcm, MaxFrames, 0);
        if (DecodedFrames < 0)
        {
            OutPcm16.Reset();
            OutNumFrames = 0;
            return false;
        }
        const int32 BytesOut = DecodedFrames * Channels * (int32)sizeof(int16);
        OutPcm16.SetNum(BytesOut, /*bAllowShrinking*/true);
        OutNumFrames = DecodedFrames;
        return true;
#else
        (void)PacketData; (void)PacketBytes; (void)OutPcm16; (void)OutNumFrames; return false;
#endif
    }
}
