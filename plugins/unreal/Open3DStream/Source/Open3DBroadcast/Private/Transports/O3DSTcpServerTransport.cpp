#include "Transports/O3DSTcpServerTransport.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Open3DBroadcast.h"

namespace { static const uint8 kO3DSMagic[] = { 0x00, 0xFF, 0x03, 0xFE, 'O','3','D','S','-','S','T','A','R','T' }; }

FO3DSTcpServerTransport::FO3DSTcpServerTransport()
{
}

FO3DSTcpServerTransport::~FO3DSTcpServerTransport()
{
    Stop();
}

bool FO3DSTcpServerTransport::ParseTcpUrl(const FString& InUrl, FString& OutHost, int32& OutPort) const
{
    // Expect format tcp://host:port
    FString Work = InUrl;
    if (Work.StartsWith(TEXT("tcp://")))
    {
    Work.RightChopInline(6, EAllowShrinking::No);
    }
    int32 ColonIdx;
    if (!Work.FindChar(':', ColonIdx))
    {
        return false;
    }
    OutHost = Work.Left(ColonIdx);
    const FString PortStr = Work.Mid(ColonIdx + 1);
    OutPort = FCString::Atoi(*PortStr);
    return !OutHost.IsEmpty() && OutPort > 0;
}

bool FO3DSTcpServerTransport::Start(const FString& InUrl, const FString& InProtocol, const FString& InKey)
{
    Url = InUrl;
    Subsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    return Listen();
}

bool FO3DSTcpServerTransport::Listen()
{
    if (!Subsys) { return false; }

    if (!ParseTcpUrl(Url, Host, Port))
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("TCPServer: Invalid URL %s"), *Url);
        return false;
    }

    // Bind listen socket on Host:Port
    bool bValid = false;
    TSharedRef<FInternetAddr> BindAddr = Subsys->CreateInternetAddr();
    BindAddr->SetIp(*Host, bValid);
    BindAddr->SetPort(Port);
    if (!bValid)
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("TCPServer: Invalid host %s"), *Host);
        return false;
    }

    ListenSocket = Subsys->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_LISTEN"), BindAddr->GetProtocolType());
    if (!ListenSocket)
    {
        return false;
    }

    ListenSocket->SetReuseAddr(true);
    ListenSocket->SetNonBlocking(true);

    if (!ListenSocket->Bind(*BindAddr))
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("TCPServer: Bind failed %s:%d"), *Host, Port);
        Subsys->DestroySocket(ListenSocket); ListenSocket = nullptr; return false;
    }
    if (!ListenSocket->Listen(8))
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("TCPServer: Listen failed %s:%d"), *Host, Port);
        Subsys->DestroySocket(ListenSocket); ListenSocket = nullptr; return false;
    }

    UE_LOG(LogO3DSBroadcast, Log, TEXT("TCPServer: Listening on %s:%d"), *Host, Port);
    return true;
}

void FO3DSTcpServerTransport::Stop()
{
    bClientConnected = false;
    if (ClientSocket && Subsys)
    {
        Subsys->DestroySocket(ClientSocket); ClientSocket = nullptr;
    }
    if (ListenSocket && Subsys)
    {
        Subsys->DestroySocket(ListenSocket); ListenSocket = nullptr;
    }
}

void FO3DSTcpServerTransport::TickAccept()
{
    if (!ListenSocket)
    {
        return;
    }

    // If we have a client, verify it's still valid by probing send zero bytes (or rely on send failure)
    if (ClientSocket)
    {
        return; // keep existing; send will clear on failure
    }

    const double Now = FPlatformTime::Seconds();
    if (Now - LastAcceptPoll < 0.005) { return; }
    LastAcceptPoll = Now;

    TSharedRef<FInternetAddr> Peer = Subsys->CreateInternetAddr();
    FSocket* Accepted = ListenSocket->Accept(*Peer, TEXT("O3DS_TCP_ACCEPT"));
    if (Accepted)
    {
        ClientSocket = Accepted;
        ClientSocket->SetNonBlocking(true);
        bClientConnected = true;
        UE_LOG(LogO3DSBroadcast, Log, TEXT("TCPServer: Client connected from %s"), *Peer->ToString(true));
    }
}

bool FO3DSTcpServerTransport::SendFramed(const uint8* Data, int32 Size)
{
    if (!ClientSocket) { return false; }

    // frame: magic(14) + size(4 LE) + payload
    uint8 Header[18];
    FMemory::Memcpy(Header, kO3DSMagic, 14);
    const uint32 Len = (uint32)Size;
    Header[14] = (uint8)(Len & 0xFF);
    Header[15] = (uint8)((Len >> 8) & 0xFF);
    Header[16] = (uint8)((Len >> 16) & 0xFF);
    Header[17] = (uint8)((Len >> 24) & 0xFF);

    int32 Sent = 0;
    if (!ClientSocket->Send(Header, sizeof(Header), Sent) || Sent != sizeof(Header))
    {
        bClientConnected = false; return false;
    }
    int32 PayloadSent = 0;
    if (!ClientSocket->Send(Data, Size, PayloadSent) || PayloadSent != Size)
    {
        bClientConnected = false; return false;
    }
    return true;
}

bool FO3DSTcpServerTransport::Send(const uint8* Data, int32 Size, double /*TimestampSeconds*/)
{
    if (!ClientSocket)
    {
        TickAccept();
        return false;
    }

    if (!SendFramed(Data, Size))
    {
        // drop client and continue listening
        if (ClientSocket && Subsys) { Subsys->DestroySocket(ClientSocket); ClientSocket = nullptr; }
        bClientConnected = false;
        return false;
    }

    Counters.BytesSent += (uint64)Size;
    Counters.FramesSent++;
    return true;
}

void FO3DSTcpServerTransport::Tick(float /*DeltaTime*/)
{
    TickAccept();
}
