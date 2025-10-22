#pragma once

#include "CoreMinimal.h"
#include "IBroadcastTransport.h"

class FSocket;
class ISocketSubsystem;

class FO3DSTcpTransport : public IBroadcastTransport
{
public:
    FO3DSTcpTransport();
    virtual ~FO3DSTcpTransport() override;

    virtual bool Start(const FString& InUrl, const FString& InProtocol, const FString& InKey) override;
    virtual void Stop() override;
    virtual bool Send(const uint8* Data, int32 Size, double TimestampSeconds) override;
    virtual bool IsConnected() const override { return bConnected; }
    virtual const FCounters& GetCounters() const override { return Counters; }

    virtual void Tick(float DeltaTime) override;

private:
    bool ParseTcpUrl(const FString& InUrl, FString& OutHost, int32& OutPort) const;
    bool Connect();

    TAtomic<bool> bConnected{false};
    FString Url;
    FString Host;
    int32 Port = 0;

    FSocket* Socket = nullptr;
    ISocketSubsystem* Subsys = nullptr;

    FCounters Counters;

    double LastConnectAttemptTime = 0.0;
    int32 BackoffAttempt = 0;
};
