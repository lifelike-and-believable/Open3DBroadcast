#pragma once

#include "CoreMinimal.h"
#include "IBroadcastTransport.h"

class FSocket;
class ISocketSubsystem;
class FInternetAddr;

// TCP server transport that accepts one client and sends O3DS-framed payloads compatible with LiveLink TCP source
class FO3DSTcpServerTransport : public IBroadcastTransport
{
public:
    FO3DSTcpServerTransport();
    virtual ~FO3DSTcpServerTransport() override;

    virtual bool Start(const FString& InUrl, const FString& InProtocol, const FString& InKey) override;
    virtual void Stop() override;
    virtual bool Send(const uint8* Data, int32 Size, double TimestampSeconds) override;
    virtual bool IsConnected() const override { return bClientConnected; }
    virtual const FCounters& GetCounters() const override { return Counters; }

    virtual void Tick(float DeltaTime) override;

private:
    bool ParseTcpUrl(const FString& InUrl, FString& OutHost, int32& OutPort) const;
    bool Listen();
    void TickAccept();

    // header framing
    bool SendFramed(const uint8* Data, int32 Size);

    ISocketSubsystem* Subsys = nullptr;
    FSocket* ListenSocket = nullptr;
    FSocket* ClientSocket = nullptr;
    FString Url;
    FString Host;
    int32 Port = 0;

    TAtomic<bool> bClientConnected{false};
    FCounters Counters;

    // timing for accept polling
    double LastAcceptPoll = 0.0;
};
