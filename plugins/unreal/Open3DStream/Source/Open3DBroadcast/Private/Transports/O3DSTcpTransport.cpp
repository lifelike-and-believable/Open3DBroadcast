#include "Transports/O3DSTcpTransport.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Open3DBroadcast.h"

FO3DSTcpTransport::FO3DSTcpTransport()
{
}

FO3DSTcpTransport::~FO3DSTcpTransport()
{
    Stop();
}

bool FO3DSTcpTransport::ParseTcpUrl(const FString& InUrl, FString& OutHost, int32& OutPort) const
{
    // Expect format tcp://host:port
    FString Work = InUrl;
    if (Work.StartsWith(TEXT("tcp://")))
    {
        Work.RightChopInline(6, false);
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

bool FO3DSTcpTransport::Start(const FString& InUrl, const FString& InProtocol, const FString& InKey)
{
    Url = InUrl;
    Subsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    return Connect();
}

bool FO3DSTcpTransport::Connect()
{
    if (!Subsys)
    {
        return false;
    }

    if (!ParseTcpUrl(Url, Host, Port))
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("TCP: Invalid URL %s"), *Url);
        return false;
    }

    TSharedRef<FInternetAddr> Addr = Subsys->CreateInternetAddr();
    bool bValid = false;
    Addr->SetIp(*Host, bValid);
    Addr->SetPort(Port);
    if (!bValid)
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("TCP: Invalid host %s"), *Host);
        return false;
    }

    if (Socket)
    {
        Subsys->DestroySocket(Socket);
        Socket = nullptr;
    }

    Socket = Subsys->CreateSocket(NAME_Stream, TEXT("O3DS_TCP"), Addr->GetProtocolType());
    if (!Socket)
    {
        return false;
    }

    Socket->SetNonBlocking(true);

    if (!Socket->Connect(*Addr))
    {
        // Non-blocking Connect returns immediately; we'll retry later
        bConnected = false;
        return false;
    }

    bConnected = true;
    BackoffAttempt = 0;
    UE_LOG(LogO3DSBroadcast, Log, TEXT("TCP: Connected to %s:%d"), *Host, Port);
    return true;
}

void FO3DSTcpTransport::Stop()
{
    bConnected = false;
    if (Socket && Subsys)
    {
        Subsys->DestroySocket(Socket);
        Socket = nullptr;
    }
}

bool FO3DSTcpTransport::Send(const uint8* Data, int32 Size, double TimestampSeconds)
{
    if (!bConnected.Load() || !Socket)
    {
        // opportunistic retry handled in Tick()
        return false;
    }

    int32 Sent = 0;
    if (!Socket->Send(Data, Size, Sent) || Sent != Size)
    {
        bConnected = false;
        return false;
    }

    Counters.BytesSent += (uint64)Size;
    Counters.FramesSent++;
    return true;
}

void FO3DSTcpTransport::Tick(float /*DeltaTime*/)
{
    if (bConnected.Load()) { return; }

    const double Now = FPlatformTime::Seconds();
    const double Delay = FMath::Min(5.0, FMath::Pow(2.0, (double)FMath::Clamp(BackoffAttempt, 0, 5)) * 0.1);
    if (Now - LastConnectAttemptTime > Delay)
    {
        LastConnectAttemptTime = Now;
        if (Connect())
        {
            Counters.Reconnects++;
        }
        else
        {
            ++BackoffAttempt;
        }
    }
}
