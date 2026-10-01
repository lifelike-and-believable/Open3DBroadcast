// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Delegates/IDelegateInstance.h"
#include "Templates/Function.h"
#include "Transport/O3DTransportTypes.h"

class IOpen3DSender;
class IO3DSenderAudioSink;

/**
 * Owns the active sender transport instance for a capture component, encapsulating lifecycle
 * management (Create/Start/Stop) and surfacing associated audio sinks on demand.
 *
 * While a sender is active the controller subscribes to
 * FO3DTransportRegistry::OnTransportUnregistering (ADR 0007 item 5, WP-A1 PR 2). When its
 * transport unregisters (the transport module shuts down) it runs the owner's handler, which
 * tears down everything that references the sender or its sinks, and then stops and releases the
 * sender itself, so the registry finds no live instance. Game thread.
 */
class FO3DSenderTransportController
{
public:
    FO3DSenderTransportController();
    ~FO3DSenderTransportController();

    FO3DSenderTransportController(const FO3DSenderTransportController&) = delete;
    FO3DSenderTransportController& operator=(const FO3DSenderTransportController&) = delete;

    bool Start(const FO3DTransportConfig& InConfig);
    void Stop();

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
};
