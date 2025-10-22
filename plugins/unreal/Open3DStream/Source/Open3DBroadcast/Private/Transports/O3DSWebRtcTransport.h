// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "IBroadcastTransport.h"
#include "Open3DWebRTCDataChannel.h"

// WebRTC DataChannel transport (optional/beta). Uses libdatachannel via Open3DStream wrapper.
class FO3DSWebRtcTransport : public IBroadcastTransport
{
public:
    FO3DSWebRtcTransport() = default;
    virtual ~FO3DSWebRtcTransport() override { Stop(); }

    virtual bool Start(const FString& InUrl, const FString& InProtocol, const FString& InKey) override
    {
        Url = InUrl; Key = InKey; Protocol = InProtocol;
        Channel = MakeUnique<FO3DSWebRTCDataChannel>();
        // Auto-detect role if not present; treat legacy "WebRTC (Deprecated)" as-is
        const bool bIsServer = Url.Contains(TEXT("role=server")) || Protocol.Contains(TEXT("WebRTCServer"));
        if (!Channel->Start(Url))
        {
            return false;
        }
        return true;
    }

    virtual void Stop() override
    {
        if (Channel)
        {
            Channel->Stop();
            Channel.Reset();
        }
    }

    virtual bool Send(const uint8* Data, int32 Size, double /*TimestampSeconds*/) override
    {
        if (!Channel || !Channel->IsOpen())
        {
            Counters.FramesDropped.Store(Counters.FramesDropped.Load() + 1);
            return false;
        }
        const bool bOk = Channel->Send(Data, Size);
        if (bOk)
        {
            Counters.FramesSent.Store(Counters.FramesSent.Load() + 1);
            Counters.BytesSent.Store(Counters.BytesSent.Load() + (uint64)Size);
        }
        else
        {
            Counters.FramesDropped.Store(Counters.FramesDropped.Load() + 1);
        }
        return bOk;
    }

    virtual bool IsConnected() const override
    {
        return Channel && Channel->IsConnected() && Channel->IsOpen();
    }

    virtual void Tick(float DeltaTime) override
    {
        if (Channel)
        {
            Channel->Tick();
        }
    }

    virtual const FCounters& GetCounters() const override { return Counters; }

private:
    FString Url, Key, Protocol;
    TUniquePtr<FO3DSWebRTCDataChannel> Channel;
    FCounters Counters;
};
