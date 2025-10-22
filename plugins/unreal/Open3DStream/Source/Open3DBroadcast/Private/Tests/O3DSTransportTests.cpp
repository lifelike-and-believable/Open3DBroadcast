// Copyright (c) Open3DStream Contributors

#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

#include "Transports/O3DSTcpTransport.h"
#include "Transports/O3DSUdpTransport.h"
#include "Transports/O3DSTcpServerTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

static bool WaitUntil(double Seconds, TFunctionRef<bool()> Predicate)
{
    const double Start = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - Start) < Seconds)
    {
        if (Predicate()) { return true; }
        FPlatformProcess::Sleep(0.01f);
    }
    return false;
}

static bool CreateTcpListener(FString& OutHost, int32& OutPort, FSocket*& OutListen, TSharedPtr<FInternetAddr>& OutAddr)
{
    ISocketSubsystem* Subsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Subsys) { return false; }

    OutHost = TEXT("127.0.0.1");

    FSocket* Listen = Subsys->CreateSocket(NAME_Stream, TEXT("O3DS_Test_TCPListen"), Subsys->GetLocalBindAddr(*GLog)->GetProtocolType());
    if (!Listen) { return false; }

    Listen->SetReuseAddr(true);
    Listen->SetNonBlocking(true);

    bool bValid = false;
    TSharedRef<FInternetAddr> Addr = Subsys->CreateInternetAddr();
    Addr->SetIp(*OutHost, bValid);
    Addr->SetPort(0); // ephemeral
    if (!bValid) { Subsys->DestroySocket(Listen); return false; }

    if (!Listen->Bind(*Addr))
    {
        Subsys->DestroySocket(Listen);
        return false;
    }

    TSharedRef<FInternetAddr> Bound = Subsys->CreateInternetAddr();
    Listen->GetAddress(*Bound);
    OutPort = Bound->GetPort();

    if (!Listen->Listen(8))
    {
        Subsys->DestroySocket(Listen);
        return false;
    }

    OutListen = Listen;
    OutAddr = Bound;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSTcpTransport_ConnectAndSend,
    "Open3DStream.M3.Transport.TCP.ConnectAndSend",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSTcpTransport_ConnectAndSend::RunTest(const FString& Parameters)
{
    FString Host; int32 Port = 0; FSocket* Listen = nullptr; TSharedPtr<FInternetAddr> BoundAddr;
    if (!CreateTcpListener(Host, Port, Listen, BoundAddr))
    {
        AddError(TEXT("Failed to create TCP listen socket"));
        return false;
    }

    // Start client transport
    FO3DSTcpTransport Tx;
    const FString Url = FString::Printf(TEXT("tcp://%s:%d"), *Host, Port);
    bool started = Tx.Start(Url, TEXT("TCP"), TEXT(""));
    TestTrue(TEXT("TCP transport started"), started);

    // Accept the connection (poll for up to 3s)
    FSocket* Accepted = nullptr;
    TSharedRef<FInternetAddr> PeerAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
    const bool bAccepted = WaitUntil(3.0, [&]()
    {
        Accepted = Listen->Accept(*PeerAddr, TEXT("O3DS_Test_TCPAccept"));
        return Accepted != nullptr;
    });

    TestTrue(TEXT("TCP accepted connection"), bAccepted && Accepted != nullptr);

    // Send a small payload
    const char* Msg = "hello_o3ds";
    const int32 Len = 10;
    bool sentOk = Tx.Send(reinterpret_cast<const uint8*>(Msg), Len, FPlatformTime::Seconds());
    TestTrue(TEXT("TCP Send returned true"), sentOk);

    // Read from accepted socket
    int32 Total = 0; int32 Attempts = 0; Accepted->SetNonBlocking(true);
    while (Attempts++ < 200 && Total < Len)
    {
        uint8 Buf[64]; int32 Read = 0;
        if (Accepted->Recv(Buf, sizeof(Buf), Read))
        {
            Total += Read;
        }
        FPlatformProcess::Sleep(0.01f);
    }

    TestTrue(TEXT("Received all bytes"), Total >= Len);

    // Cleanup
    ISocketSubsystem* Subsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (Accepted) { Subsys->DestroySocket(Accepted); }
    if (Listen) { Subsys->DestroySocket(Listen); }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSTcpServerTransport_FrameHeader,
    "Open3DStream.M3.Transport.TCPServer.FrameHeader",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSTcpServerTransport_FrameHeader::RunTest(const FString& Parameters)
{
    // Bind server on ephemeral port
    ISocketSubsystem* Subsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Subsys) { AddError(TEXT("No socket subsystem")); return false; }

    FSocket* Tmp = Subsys->CreateSocket(NAME_Stream, TEXT("TmpBind"), Subsys->GetLocalBindAddr(*GLog)->GetProtocolType());
    bool bValid=false; TSharedRef<FInternetAddr> Addr = Subsys->CreateInternetAddr(); Addr->SetIp(TEXT("127.0.0.1"), bValid); Addr->SetPort(0);
    TestTrue(TEXT("Valid localhost"), bValid);
    TestTrue(TEXT("Bind tmp"), Tmp->Bind(*Addr));
    TSharedRef<FInternetAddr> Local = Subsys->CreateInternetAddr(); Tmp->GetAddress(*Local);
    const int32 Port = Local->GetPort(); Subsys->DestroySocket(Tmp);

    FO3DSTcpServerTransport Server;
    const FString Url = FString::Printf(TEXT("tcp://127.0.0.1:%d"), Port);
    TestTrue(TEXT("Server start"), Server.Start(Url, TEXT("TCPServer"), TEXT("")));

    // Connect a raw client
    TSharedRef<FInternetAddr> ConnectAddr = Subsys->CreateInternetAddr(); ConnectAddr->SetIp(TEXT("127.0.0.1"), bValid); ConnectAddr->SetPort(Port);
    FSocket* Client = Subsys->CreateSocket(NAME_Stream, TEXT("Client"), ConnectAddr->GetProtocolType());
    TestTrue(TEXT("Client connect"), Client->Connect(*ConnectAddr));

    // Give accept loop a moment
    WaitUntil(0.2, [&]() { return Server.IsConnected(); });

    // Send some payload through server (should frame header)
    uint8 Payload[5] = {1,2,3,4,5};
    TestTrue(TEXT("Server send"), Server.Send(Payload, 5, FPlatformTime::Seconds()));

    // Expect 18-byte header + 5 payload
    Client->SetNonBlocking(true);
    uint8 Buf[64]; int32 Read=0; int32 Total=0; int Attempts=0;
    while (Attempts++ < 200 && Total < 23)
    {
        if (Client->Recv(Buf + Total, 23 - Total, Read))
        {
            Total += Read;
        }
        FPlatformProcess::Sleep(0.01f);
    }

    TestTrue(TEXT("Got framed bytes"), Total == 23);
    // magic check
    const uint8 ExpectMagic[14] = {0x00,0xFF,0x03,0xFE,'O','3','D','S','-','S','T','A','R','T'};
    bool MagicOk = (FMemory::Memcmp(Buf, ExpectMagic, 14) == 0);
    TestTrue(TEXT("Magic OK"), MagicOk);
    // size LE 5
    TestTrue(TEXT("Size low"), Buf[14] == 5);
    TestTrue(TEXT("Size high"), Buf[15] == 0 && Buf[16] == 0 && Buf[17] == 0);
    // payload
    bool PayloadOk = (Buf[18]==1 && Buf[19]==2 && Buf[20]==3 && Buf[21]==4 && Buf[22]==5);
    TestTrue(TEXT("Payload OK"), PayloadOk);

    Subsys->DestroySocket(Client);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSUdpTransport_SendFragments,
    "Open3DStream.M3.Transport.UDP.SendFragments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSUdpTransport_SendFragments::RunTest(const FString& Parameters)
{
    ISocketSubsystem* Subsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Subsys)
    {
        AddError(TEXT("No socket subsystem"));
        return false;
    }

    // Create UDP receive socket bound to localhost:0
    FSocket* Rx = Subsys->CreateSocket(NAME_DGram, TEXT("O3DS_Test_UDPRecv"), Subsys->GetLocalBindAddr(*GLog)->GetProtocolType());
    if (!Rx)
    {
        AddError(TEXT("Failed to create UDP recv socket"));
        return false;
    }

    bool bValid = false;
    TSharedRef<FInternetAddr> BindAddr = Subsys->CreateInternetAddr();
    BindAddr->SetIp(TEXT("127.0.0.1"), bValid);
    BindAddr->SetPort(0);
    if (!bValid || !Rx->Bind(*BindAddr))
    {
        Subsys->DestroySocket(Rx);
        AddError(TEXT("Failed to bind UDP recv socket"));
        return false;
    }

    TSharedRef<FInternetAddr> Local = Subsys->CreateInternetAddr();
    Rx->GetAddress(*Local);
    const int32 Port = Local->GetPort();
    Rx->SetNonBlocking(true);

    // Start UDP transport targeting our bound port
    FO3DSUdpTransport Udp;
    const FString UrlUdp = FString::Printf(TEXT("udp://127.0.0.1:%d"), Port);
    bool started = Udp.Start(UrlUdp, TEXT("UDP"), TEXT(""));
    TestTrue(TEXT("UDP transport started"), started);

    // Send a payload large enough to force fragmentation (default MTU ~1200)
    TArray<uint8> Payload; Payload.SetNumUninitialized(4096);
    for (int32 i = 0; i < Payload.Num(); ++i) { Payload[i] = (uint8)(i & 0xFF); }

    const bool sent = Udp.Send(Payload.GetData(), Payload.Num(), FPlatformTime::Seconds());
    TestTrue(TEXT("UDP send returned true"), sent);

    // Receive few datagrams for up to 2s
    int32 Packets = 0; int32 Bytes = 0; int32 Attempts = 0;
    while (Attempts++ < 200)
    {
        uint8 Buf2[1500]; int32 Read2 = 0; TSharedRef<FInternetAddr> From = Subsys->CreateInternetAddr();
        if (Rx->RecvFrom(Buf2, sizeof(Buf2), Read2, *From))
        {
            if (Read2 > 0) { ++Packets; Bytes += Read2; }
            if (Packets >= 2) { break; }
        }
        FPlatformProcess::Sleep(0.01f);
    }

    TestTrue(TEXT("UDP received >=2 packets"), Packets >= 2);
    TestTrue(TEXT("UDP received some bytes"), Bytes > 0);

    Subsys->DestroySocket(Rx);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
