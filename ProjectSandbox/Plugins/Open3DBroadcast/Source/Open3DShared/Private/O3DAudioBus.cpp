// Copyright (c) Open3DStream Contributors

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

    TArray<uint8> Copy;
    if (NumBytes > 0 && Data)
    {
        Copy.Append(Data, NumBytes);
    }

    OnPcm16().Broadcast(Meta, Copy);
}
