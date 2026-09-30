// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "O3DSenderInterface.h"
#include "../Shared/LoopbackChannel.h"
#include "O3DLifetimeGate.h"
#include "HAL/CriticalSection.h"

#include <atomic>

class FO3DLoopbackSender : public IOpen3DSender
{
public:
    virtual ~FO3DLoopbackSender() override;

    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual bool Send(const O3DS::SubjectList& List) override;
    virtual bool SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override;
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

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

    /** WP-S5: audio sinks hold this gate (never the sender); Stop() closes it before returning. */
    TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> AudioGate = MakeShared<FO3DLifetimeGate, ESPMode::ThreadSafe>();

    bool SendBytes(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec);
};
