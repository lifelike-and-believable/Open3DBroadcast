// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"

#include "O3DReceiverInterface.h"
#include "Shared/NngHelpers.h"

class FO3DNngReceiver : public IOpen3DReceiver
{
public:
    virtual ~FO3DNngReceiver() override = default;

    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual int32 Poll() override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override { return false; }

    void HandlePipeAdded();
    void HandlePipeRemoved();

private:
    struct FNngSocketWrapper;

    bool OpenSocket();
    void CloseSocket();
    void HandleReceiveError(int ErrorCode);
    bool EnsureDialSocket();

    O3DNNG::FNngReceiverOptions Options;
    FO3DTransportStats Stats;
    FO3DTransportConfig ActiveConfig;

    TWeakPtr<ISerializedFrameConsumer> Consumer;

    FNngSocketWrapper* Socket = nullptr;

    TAtomic<bool> bInitialized{ false };
    TAtomic<bool> bRunning{ false };
    TAtomic<bool> bConnected{ false };

    FThreadSafeCounter PipeCount;

    double LastDialAttempt = 0.0;
    int32 BackoffAttempt = 0;
    double LastErrorLogTimestamp = 0.0;
};
