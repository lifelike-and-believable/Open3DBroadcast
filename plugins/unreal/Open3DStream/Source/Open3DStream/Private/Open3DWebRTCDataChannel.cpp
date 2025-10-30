// Copyright (c) Open3DStream Contributors

#include "Open3DWebRTCDataChannel.h"
#include "IWebRTCConnector.h"
#include "Open3DStreamSourceSettings.h" // For EO3DSWebRtcBackendReceiver enum
// For TQueue and EQueueMode
#include "Containers/Queue.h"

class FO3DSWebRTCDataChannel::FImpl
{
public:
 FImpl() = default;

 // Create connector without starting (allows audio configuration before PeerConnection creation)
 bool PrepareConnector(EO3DSWebRtcBackendReceiver Backend)
 {
 if (Connector)
 {
 return true; // Already prepared
 }
 
 // Create connector via factory using provided backend
 Connector = CreateWebRTCConnector(Backend);
 if (!Connector)
 {
 UE_LOG(LogTemp, Error, TEXT("Failed to create WebRTC connector for backend %d"), (int)Backend);
 return false;
 }
 return true;
 }

 bool Start(const FString& Url, EO3DSWebRtcBackendReceiver Backend)
 {
 // Ensure connector is created (supports both old single-phase and new two-phase init)
 if (!PrepareConnector(Backend))
 {
 return false;
 }

 // Default role client unless URL has role=server
 const bool bServer = Url.Contains(TEXT("role=server"));
 const bool bOk = Connector->Start(Url, bServer);
 UE_LOG(LogTemp, Log, TEXT("O3DS WebRTCDataChannel: Start url=%s backend=%d role=%s ok=%d"),
 *Url, (int)Backend, bServer?TEXT("server"):TEXT("client"), bOk?1:0);
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
 UE_LOG(LogTemp, Log, TEXT("O3DS WebRTCDataChannel: Stop"));
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
 while (Received.Dequeue(Msg))
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

bool FO3DSWebRTCDataChannel::PrepareConnector(EO3DSWebRtcBackendReceiver Backend) { return Impl->PrepareConnector(Backend); }
bool FO3DSWebRTCDataChannel::Start(const FString& Url, EO3DSWebRtcBackendReceiver Backend) { return Impl->Start(Url, Backend); }
void FO3DSWebRTCDataChannel::Stop() { if (Impl) Impl->Stop(); }
bool FO3DSWebRTCDataChannel::Send(const uint8* Data, int32 Size) { return Impl->Send(Data, Size); }
bool FO3DSWebRTCDataChannel::IsConnected() const { return Impl->IsConnected(); }
bool FO3DSWebRTCDataChannel::IsOpen() const { return Impl->IsOpen(); }
void FO3DSWebRTCDataChannel::SetOnMessage(TFunction<void(const uint8*, int32)> InOnMessage) { Impl->OnMessage = MoveTemp(InOnMessage); }
void FO3DSWebRTCDataChannel::Tick() { Impl->Tick(); }
TSharedPtr<IWebRTCConnector> FO3DSWebRTCDataChannel::GetConnector() const { return Impl ? Impl->Connector : nullptr; }
