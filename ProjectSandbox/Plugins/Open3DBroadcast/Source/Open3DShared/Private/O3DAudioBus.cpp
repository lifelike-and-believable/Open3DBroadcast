// Copyright (c) Open3DStream Contributors

#include "O3DAudioBus.h"

#include "O3DRuntimeContext.h"

FO3DOnAudioPcm16& FO3DAudioBus::FInstance::OnPcm16()
{
    check(IsInGameThread());
    return Delegate;
}

void FO3DAudioBus::FInstance::PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes)
{
    check(IsInGameThread());
    if (!Delegate.IsBound())
    {
        return;
    }

    const TConstArrayView<uint8> Bytes = (NumBytes > 0 && Data) ? TConstArrayView<uint8>(Data, NumBytes) : TConstArrayView<uint8>();
    Delegate.Broadcast(Meta, Bytes);
}

FO3DOnAudioPcm16& FO3DAudioBus::OnPcm16()
{
    return FO3DRuntimeContext::Default()->GetAudioBus().OnPcm16();
}

void FO3DAudioBus::PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes)
{
    FO3DRuntimeContext::Default()->GetAudioBus().PublishPcm16(Meta, Data, NumBytes);
}
