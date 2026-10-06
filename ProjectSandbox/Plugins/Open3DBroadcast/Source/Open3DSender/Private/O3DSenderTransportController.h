// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Delegates/IDelegateInstance.h"
#include "Templates/Function.h"
#include "Transport/O3DTransportTypes.h"

class IOpen3DSender;
class IO3DSenderAudioSink;
class FO3DConnectionStateMailbox;
class FO3DSenderPipeline;

/**
 * Owns the active sender transport instance for a capture component, encapsulating lifecycle
 * management (Create/Start/Stop) and surfacing associated audio sinks on demand.
 *
 * While a sender is active the controller subscribes to
 * FO3DTransportRegistry::OnTransportUnregistering (ADR 0007 item 5, WP-A1 PR 2). When its
 * transport unregisters (the transport module shuts down) it runs the owner's handler, which
 * tears down everything that references the sender or its sinks, and then stops and releases the
 * sender itself, so the registry finds no live instance. Game thread.
 *
 * The started sender is handed to the owner's pose pipeline (ADR 0008 implementation outline
 * item 4, WP-A2c), whose worker calls SendSerialized. Stop detaches it from the pipeline first,
 * which waits for a frame the worker is sending, and only then stops and releases it, so no
 * SendSerialized is in flight when the transport stops.
 */
class FO3DSenderTransportController
{
public:
    FO3DSenderTransportController();
    ~FO3DSenderTransportController();

    FO3DSenderTransportController(const FO3DSenderTransportController&) = delete;
    FO3DSenderTransportController& operator=(const FO3DSenderTransportController&) = delete;

    /** Creates, initializes and starts the sender. On failure GetLastResult() says why. */
    bool Start(const FO3DTransportConfig& InConfig);
    void Stop();

    /** Result of the last Start (Ok after a successful one; ADR 0007 item 3). */
    const FO3DTransportResult& GetLastResult() const { return LastResult; }

    bool IsActive() const;
    const FO3DTransportConfig& GetConfig() const { return ActiveConfig; }
    TSharedPtr<IOpen3DSender> GetSender() const { return ActiveSender; }
    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> GetAudioSink() const { return AudioSink; }

    /**
     * Called when the active transport unregisters, before the controller stops and releases the
     * sender. The owner drops its own references to the sender and the audio sink there (it may
     * call Stop). Game thread.
     */
    void SetOnTransportUnregistering(TFunction<void()> InHandler) { OnTransportUnregisteringHandler = MoveTemp(InHandler); }

    /** The pose pipeline a started sender is attached to (null: none). Game thread, while stopped. */
    void SetPipeline(const TSharedPtr<FO3DSenderPipeline>& InPipeline) { Pipeline = InPipeline; }

    /**
     * Where a created sender posts its connection-state changes (WP-U2): Start sets the sender's
     * state callback to post into it before Initialize. Null: none. Game thread, while stopped.
     */
    void SetStateMailbox(const TSharedPtr<FO3DConnectionStateMailbox, ESPMode::ThreadSafe>& InMailbox) { StateMailbox = InMailbox; }

private:
    void HandleTransportUnregistering(FName TransportName);
    void Unsubscribe();

    FO3DTransportConfig ActiveConfig;
    TSharedPtr<IOpen3DSender> ActiveSender;
    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink;
    /** Name of the active transport, as registered. None while inactive. */
    FName ActiveTransportName;
    FDelegateHandle UnregisteringHandle;
    TFunction<void()> OnTransportUnregisteringHandler;
    FO3DTransportResult LastResult;
    /** Receives ActiveSender after a successful Start; detached in Stop before the sender stops. */
    TSharedPtr<FO3DSenderPipeline> Pipeline;
    TSharedPtr<FO3DConnectionStateMailbox, ESPMode::ThreadSafe> StateMailbox;
};
