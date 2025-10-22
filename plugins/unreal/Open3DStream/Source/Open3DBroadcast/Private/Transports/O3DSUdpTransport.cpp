#include "Transports/O3DSUdpTransport.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/UnrealMemory.h"
#include "Open3DBroadcast.h"

FO3DSUdpTransport::FO3DSUdpTransport() {}
FO3DSUdpTransport::~FO3DSUdpTransport() { Stop(); }

bool FO3DSUdpTransport::ParseUdpUrl(const FString& InUrl, FString& OutHost, int32& OutPort) const
{
    // Expect format udp://host:port
    FString Work = InUrl;
    if (Work.StartsWith(TEXT("udp://")))
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

bool FO3DSUdpTransport::Start(const FString& InUrl, const FString& /*InProtocol*/, const FString& /*InKey*/)
{
    Url = InUrl;
    Subsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    return InitSocket();
}

bool FO3DSUdpTransport::InitSocket()
{
    if (!Subsys)
    {
        return false;
    }

    if (!ParseUdpUrl(Url, Host, Port))
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("UDP: Invalid URL %s"), *Url);
        return false;
    }

    // Resolve destination. Support common shorthands:
    //  - "localhost" => 127.0.0.1
    //  - "*" => 255.255.255.255 (broadcast)
    FString ResolvedHost = Host;
    if (Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
    {
        ResolvedHost = TEXT("127.0.0.1");
    }
    else if (Host == TEXT("*"))
    {
        ResolvedHost = TEXT("255.255.255.255");
    }

    bool bIsValid = false;
    TSharedRef<FInternetAddr> Addr = Subsys->CreateInternetAddr();
    Addr->SetIp(*ResolvedHost, bIsValid);
    Addr->SetPort(Port);
    if (!bIsValid)
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("UDP: Invalid host %s"), *Host);
        return false;
    }

    RemoteAddr = Addr;

    Socket = Subsys->CreateSocket(NAME_DGram, TEXT("O3DS_UDP"), RemoteAddr->GetProtocolType());
    if (!Socket)
    {
        return false;
    }

    // Robust defaults for a sender
    Socket->SetNonBlocking(true);
    Socket->SetReuseAddr(true);

    // Allow broadcast if host is broadcast address
    if (ResolvedHost == TEXT("255.255.255.255"))
    {
        Socket->SetBroadcast(true);
    }

    int32 NewSize = 0;
    Socket->SetSendBufferSize(2 * 1024 * 1024, NewSize);

    bReady = true;
    UE_LOG(LogO3DSBroadcast, Log, TEXT("UDP: Ready to %s:%d"), *ResolvedHost, Port);
    return true;
}

void FO3DSUdpTransport::Stop()
{
    bReady = false;
    RemoteAddr.Reset();
    if (Socket && Subsys)
    {
        Subsys->DestroySocket(Socket);
        Socket = nullptr;
    }
}

bool FO3DSUdpTransport::SendSingle(const uint8* Data, int32 Size)
{
    if (!RemoteAddr.IsValid())
    {
        return false;
    }
    int32 Sent = 0;
    if (!Socket->SendTo(Data, Size, Sent, *RemoteAddr))
    {
        UE_LOG(LogO3DSBroadcast, Verbose, TEXT("UDP: SendTo failed (Size=%d, Sent=%d)"), Size, Sent);
        return false;
    }
    if (Sent != Size)
    {
        UE_LOG(LogO3DSBroadcast, Verbose, TEXT("UDP: Partial send (Size=%d, Sent=%d)"), Size, Sent);
        return false;
    }
    Counters.BytesSent += (uint64)Sent;
    Counters.FramesSent++;
    return true;
}

bool FO3DSUdpTransport::Send(const uint8* Data, int32 Size, double /*TimestampSeconds*/)
{
    if (!bReady || !Socket)
    {
        return false;
    }

    // Receiver expects one full message per datagram. If message is larger than MaxDatagramBytes, drop.
    if (Size > MaxDatagramBytes)
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("UDP: Payload too large for single datagram (%d > %d) — dropping"), Size, MaxDatagramBytes);
        Counters.FramesDropped++;
        return false;
    }

    return SendSingle(Data, Size);
}
