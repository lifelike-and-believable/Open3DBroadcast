// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "O3DSUnifiedMessage.h"

// Simple global bus to route decoded audio frames (PCM16) from network demux
// to interested in-world components for playback.

DECLARE_MULTICAST_DELEGATE_TwoParams(FO3DSOnAudioPcm16, const O3DS::FAudioFrameMeta& /*Meta*/, const TArray<uint8>& /*PCM16Bytes*/);

class OPEN3DSTREAM_API FO3DSAudioBus
{
public:
    static FO3DSOnAudioPcm16& OnPcm16()
    {
        static FO3DSOnAudioPcm16 Delegate;
        return Delegate;
    }

    static void PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes)
    {
        TArray<uint8> Copy;
        if (NumBytes > 0 && Data)
        {
            Copy.Append(Data, NumBytes);
        }
        OnPcm16().Broadcast(Meta, Copy);
    }
};
