// Copyright (c) Open3DStream Contributors

#include "Open3DWebRTCDataChannel.h"
#include "IWebRTCConnector.h"
#include "O3DSBroadcastTransportAdapter.h" // For EO3DSWebRtcBackend enum
#include "Open3DStreamSourceSettings.h"    // For EO3DSWebRtcBackendReceiver enum

// Helper to convert broadcast backend enum to receiver backend enum
static EO3DSWebRtcBackendReceiver ConvertBackendEnum(EO3DSWebRtcBackend BroadcastBackend)
{
    switch (BroadcastBackend)
    {
    case EO3DSWebRtcBackend::LibDataChannel:
        return EO3DSWebRtcBackendReceiver::LibDataChannel;
    case EO3DSWebRtcBackend::LiveKit:
        return EO3DSWebRtcBackendReceiver::LiveKit;
    default:
        return EO3DSWebRtcBackendReceiver::LibDataChannel;
    }
}

class FO3DSWebRTCDataChannel::FImpl
{
public:
    FImpl() = default;

    bool Start(const FString& Url, EO3DSWebRtcBackend Backend)
    {
        // Convert backend enum and create connector via factory
        EO3DSWebRtcBackendReceiver ReceiverBackend = ConvertBackendEnum(Backend);
        Connector = CreateWebRTCConnector(ReceiverBackend);
        if (!Connector)
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to create WebRTC connector for backend %d"), (int)Backend);
            return false;
        }

        // Default role client unless URL has role=server
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
            Connector.Reset();
        }
    }

    bool Send(const uint8* Data, int32 Size)
    {
        return Connector ? Connector->SendDataLossy(Data, Size) : false;
    }

    bool IsConnected() const
    {
        return Connector ? Connector->IsConnected() : false;
    }

    bool IsOpen() const
    {
        // For now, assume connected == open for the interface
        return Connector ? Connector->IsConnected() : false;
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

    TSharedPtr<IWebRTCConnector> Connector;
    TQueue<TArray<uint8>, EQueueMode::Mpsc> Received;
    TFunction<void(const uint8*, int32)> OnMessage;
};

FO3DSWebRTCDataChannel::FO3DSWebRTCDataChannel() : Impl(MakeUnique<FImpl>()) {}
FO3DSWebRTCDataChannel::~FO3DSWebRTCDataChannel() { Stop(); }

bool FO3DSWebRTCDataChannel::Start(const FString& Url, EO3DSWebRtcBackend Backend) { return Impl->Start(Url, Backend); }
void FO3DSWebRTCDataChannel::Stop() { if (Impl) Impl->Stop(); }
bool FO3DSWebRTCDataChannel::Send(const uint8* Data, int32 Size) { return Impl->Send(Data, Size); }
bool FO3DSWebRTCDataChannel::IsConnected() const { return Impl->IsConnected(); }
bool FO3DSWebRTCDataChannel::IsOpen() const { return Impl->IsOpen(); }
void FO3DSWebRTCDataChannel::SetOnMessage(TFunction<void(const uint8*, int32)> InOnMessage) { Impl->OnMessage = MoveTemp(InOnMessage); }
void FO3DSWebRTCDataChannel::Tick() { Impl->Tick(); }
