// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "IBroadcastTransport.h"

#include "IWebRTCConnector.h"
#include "WebRTCConnectorFactory.h"

// IBroadcastTransport implementation backed by the shared IWebRTCConnector.
// This provides a WebRTC DataChannel transport for broadcasting serialized O3DS buffers.
class FO3DSWebRtcTransport : public IBroadcastTransport
{
public:
    FO3DSWebRtcTransport() = default;
    virtual ~FO3DSWebRtcTransport() override { Stop(); }

    // IBroadcastTransport
    virtual bool Start(const FString& InUrl, const FString& InProtocol, const FString& InKey) override;
    virtual void Stop() override;
    virtual bool Send(const uint8* Data, int32 Size, double /*TimestampSeconds*/) override;
    virtual bool IsConnected() const override;
    virtual void Tick(float DeltaTime) override;
    virtual const FCounters& GetCounters() const override { return Counters; }

    // Optional: apply audio and verbosity settings before Start()
    void ApplyPreStartConfig(const FO3DSWebRtcConfig& InConfig);

private:
    static bool ParseBoolParam(const FString& Url, const TCHAR* Key, bool& OutVal);
    static bool ParseIntParam(const FString& Url, const TCHAR* Key, int32& OutVal);
    static bool ParseFloatParam(const FString& Url, const TCHAR* Key, float& OutVal);
    static bool ParseStringParam(const FString& Url, const TCHAR* Key, FString& OutVal);
    static EO3DSWebRtcRole RoleFromUrlOrProtocol(const FString& Url, const FString& Protocol);

private:
    TSharedPtr<IWebRTCConnector> Connector;
    FO3DSWebRtcConfig PreConfig; // settings provided by component prior to Start
    bool bHasPreConfig = false;

    FCounters Counters;
};
