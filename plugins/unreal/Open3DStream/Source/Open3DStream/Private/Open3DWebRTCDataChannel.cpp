// Copyright (c) Open3DStream Contributors

#include "Open3DWebRTCDataChannel.h"
#include "WebRTCConnector.h"

class FO3DSWebRTCDataChannel::FImpl
{
public:
    FImpl()
    {
        Connector = MakeUnique<FWebRTCConnector>();
    }

    bool Start(const FString& Url)
    {
        // Default role client unless URL has role=server
        FString Host; uint16 Port = 0; FString Room; TMap<FString,FString> Params;
        // Use FWebRTCConnector helper to parse internally; if not available, fall back to simple heuristic
        const bool bServer = Url.Contains(TEXT("role=server"));
        const bool bOk = Connector->Start(Url, bServer);
        if (!bOk)
        {
            return false;
        }
        Connector->SetDataReceivedCallback([this](const uint8* Data, int32 Size)
        {
            // Enqueue for game-thread delivery
            TArray<uint8> Copy; Copy.Append(Data, Size);
            Received.Enqueue(MoveTemp(Copy));
        });
        return true;
    }

    void Stop()
    {
        if (Connector)
        {
            Connector->Stop();
        }
    }

    bool Send(const uint8* Data, int32 Size)
    {
        return Connector ? Connector->Send(Data, Size) : false;
    }

    bool IsConnected() const
    {
        return Connector ? Connector->IsConnected() : false;
    }

    bool IsOpen() const
    {
        return Connector ? Connector->IsDataChannelOpen() : false;
    }

    void Tick()
    {
        if (Connector)
        {
            Connector->Tick();
        }
        // Deliver queued messages on game thread
        TArray<uint8> Msg;
        int SafeGuard = 32;
        while (SafeGuard-- > 0 && Received.Dequeue(Msg))
        {
            if (OnMessage)
            {
                OnMessage(Msg.GetData(), Msg.Num());
            }
        }
    }

    TUniquePtr<FWebRTCConnector> Connector;
    TQueue<TArray<uint8>, EQueueMode::Mpsc> Received;
    TFunction<void(const uint8*, int32)> OnMessage;
};

FO3DSWebRTCDataChannel::FO3DSWebRTCDataChannel() : Impl(MakeUnique<FImpl>()) {}
FO3DSWebRTCDataChannel::~FO3DSWebRTCDataChannel() { Stop(); }

bool FO3DSWebRTCDataChannel::Start(const FString& Url) { return Impl->Start(Url); }
void FO3DSWebRTCDataChannel::Stop() { if (Impl) Impl->Stop(); }
bool FO3DSWebRTCDataChannel::Send(const uint8* Data, int32 Size) { return Impl->Send(Data, Size); }
bool FO3DSWebRTCDataChannel::IsConnected() const { return Impl->IsConnected(); }
bool FO3DSWebRTCDataChannel::IsOpen() const { return Impl->IsOpen(); }
void FO3DSWebRTCDataChannel::SetOnMessage(TFunction<void(const uint8*, int32)> InOnMessage) { Impl->OnMessage = MoveTemp(InOnMessage); }
void FO3DSWebRTCDataChannel::Tick() { Impl->Tick(); }
