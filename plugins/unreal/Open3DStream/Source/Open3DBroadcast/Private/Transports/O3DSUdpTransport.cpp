#include "Transports/O3DSUdpTransport.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/UnrealMemory.h"
#include "Open3DBroadcast.h"
#include "o3ds/udp_fragment.h"

#include <vector>

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

    // If it fits in one datagram, send as-is
    if (Size <= MaxDatagramBytes)
    {
        return SendSingle(Data, Size);
    }

    // Fragment using O3DS helper (header is 16 bytes)
    const int32 HeaderBytes = 16;
    const int32 FragPayload = FMath::Clamp(MtuBytes - HeaderBytes, 256, MaxDatagramBytes);

    if (FragPayload <= 0)
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("UDP: Invalid fragmentation payload size (MtuBytes=%d)"), MtuBytes);
        Counters.FramesDropped++;
        return false;
    }

    UdpFragmenter Frag(reinterpret_cast<const char*>(Data), static_cast<size_t>(Size), static_cast<size_t>(FragPayload));
    const uint32 MessageId = static_cast<uint32>(MessageCounter.Increment());

    for (uint32 Seq = 0; Seq < static_cast<uint32>(Frag.mFrames); ++Seq)
    {
        std::vector<char> Out;
        Frag.makeFragment(MessageId, Seq, Out);

        int32 Sent = 0;
        if (!Socket->SendTo(reinterpret_cast<const uint8*>(Out.data()), static_cast<int32>(Out.size()), Sent, *RemoteAddr)
            || Sent != static_cast<int32>(Out.size()))
        {
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("UDP: Fragment send failed (seq=%u, sent=%d, size=%d)"), Seq, Sent, static_cast<int32>(Out.size()));
            Counters.FramesDropped++;
            return false;
        }
    }

    Counters.BytesSent += static_cast<uint64>(Size);
    Counters.FramesSent++;
    return true;
}
