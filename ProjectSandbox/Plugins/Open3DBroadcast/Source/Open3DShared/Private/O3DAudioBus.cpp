// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DAudioBus.h"

namespace
{
    FO3DOnAudioPcm16 GO3DAudioBusDelegate;
}

FO3DOnAudioPcm16& FO3DAudioBus::OnPcm16()
{
    check(IsInGameThread());
    return GO3DAudioBusDelegate;
}

void FO3DAudioBus::PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes)
{
    check(IsInGameThread());
    if (!GO3DAudioBusDelegate.IsBound())
    {
        return;
    }

    const TConstArrayView<uint8> Bytes = (NumBytes > 0 && Data) ? TConstArrayView<uint8>(Data, NumBytes) : TConstArrayView<uint8>();
    GO3DAudioBusDelegate.Broadcast(Meta, Bytes);
}
