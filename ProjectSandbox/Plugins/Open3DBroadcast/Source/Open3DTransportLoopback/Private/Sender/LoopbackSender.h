// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DConnectionState.h"
#include "../Shared/LoopbackChannel.h"
#include "O3DLifetimeGate.h"
#include "HAL/CriticalSection.h"

#include <atomic>

class FO3DLoopbackSender : public IOpen3DSender
{
public:
    virtual ~FO3DLoopbackSender() override;

    virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
    virtual FO3DTransportResult Start() override;
    virtual void Stop() override;
    virtual bool Send(const O3DS::SubjectList& List) override;
    virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DLoopback::GetCapabilities(FO3DTransportConfig()); }
    virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
    virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;
    virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len) override;

private:
    FString ChannelKey;
    int32 QueueCapacity = 64;
    int32 AudioQueueCapacity = 32;
    TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> Channel;
    bool bInitialized = false;
    /** Set by Start(), cleared by Stop(). Sends outside a session are rejected (ADR 0007 contract; WP-T2 conformance). */
    std::atomic<bool> bRunning{false};
    FO3DTransportAudioConfig ActiveAudioConfig;
    FGuid AudioSourceGuid;
    /** SendSerialized runs on any thread; counters change only under StatsMutex (WP-T2 conformance Stats.MonotonicUnderLoad). */
    FO3DTransportStats Stats;
    mutable FCriticalSection StatsMutex;
    /** ADR 0007 item 3: Connected from Start to Stop (the channel needs no peer). */
    FO3DConnectionStateTracker ConnectionState;

    /** WP-S5: audio sinks hold this gate (never the sender); Stop() closes it before returning. */
    TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> AudioGate = MakeShared<FO3DLifetimeGate, ESPMode::ThreadSafe>();

    /** Running and backpressure checks shared by Send and SendSerialized; Queued when the channel has room. */
    EO3DSendResult CheckCanSend();
    void EnqueueFrame(TArray<uint8>&& Bytes, const FString& SubjectName, double CaptureTimestampSec);
};
