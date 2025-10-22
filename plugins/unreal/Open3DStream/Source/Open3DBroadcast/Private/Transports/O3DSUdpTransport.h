#pragma once

#include "CoreMinimal.h"
#include "IBroadcastTransport.h"
#include "HAL/ThreadSafeCounter.h"

class FSocket;
class ISocketSubsystem;

class FO3DSUdpTransport : public IBroadcastTransport
{
public:
    FO3DSUdpTransport();
    virtual ~FO3DSUdpTransport() override;

    virtual bool Start(const FString& InUrl, const FString& InProtocol, const FString& InKey) override;
    virtual void Stop() override;
    virtual bool Send(const uint8* Data, int32 Size, double TimestampSeconds) override;
    virtual bool IsConnected() const override { return bReady; }
    virtual const FCounters& GetCounters() const override { return Counters; }

private:
    bool ParseUdpUrl(const FString& InUrl, FString& OutHost, int32& OutPort) const;
    bool InitSocket();

    // Single datagram send; rely on IP fragmentation if needed
    bool SendSingle(const uint8* Data, int32 Size);

    FString Url;
    FString Host;
    int32 Port = 0;

    FSocket* Socket = nullptr;
    ISocketSubsystem* Subsys = nullptr;
    TSharedPtr<class FInternetAddr> RemoteAddr;

    bool bReady = false;

    FCounters Counters;

    // Datagram sizing (IPv4 max safe payload ~65507 bytes)
    int32 MaxDatagramBytes = 64000;

    // Optional MTU hint for future fragmentation work (not used yet)
    int32 MtuBytes = 1200;

    // Message id generator (thread-safe) - unused after removing custom fragmentation
    FThreadSafeCounter MessageCounter;
};
