// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DSenderTransportController.h"

#include "Transport/O3DSenderInterface.h"
#include "O3DBlueprintTransportTypes.h"
#include "O3DSenderLogs.h"
#include "O3DSenderPipeline.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportRegistry.h"

FO3DSenderTransportController::FO3DSenderTransportController() = default;

FO3DSenderTransportController::~FO3DSenderTransportController()
{
    // Only the subscription: the delegate must not call a destroyed controller. Releasing the
    // sender is left to the members' destructors, as before.
    Unsubscribe();
}

bool FO3DSenderTransportController::Start(const FO3DTransportConfig& InConfig)
{
    Stop();

    ActiveConfig = InConfig;
    if (ActiveConfig.Transport.IsNone())
    {
        UE_LOG(LogO3DSenderComponent, Warning, TEXT("No transport specified; skipping auto transport setup."));
        LastResult = FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("No transport selected."));
        return false;
    }

    // The schema's Validate functions refuse the options before anything is created (WP-A1 PR 5c).
    const FO3DTransportResult OptionsResult = O3DTransportOptions::ValidateOptions(ActiveConfig.GetOptions());
    if (!OptionsResult.IsOk())
    {
        UE_LOG(LogO3DSenderComponent, Warning, TEXT("Sender transport '%s' not started: %s"), *ActiveConfig.Transport.ToString(), *LexToString(OptionsResult));
        LastResult = OptionsResult;
        return false;
    }

    const FName SelectedTransportName(ActiveConfig.Transport);
    ActiveSender = FO3DTransportRegistry::Get().CreateSender(SelectedTransportName);
    if (!ActiveSender.IsValid())
    {
        UE_LOG(LogO3DSenderComponent, Warning, TEXT("No sender registered for transport '%s'."), *ActiveConfig.Transport.ToString());
        LastResult = FO3DTransportResult::Error(EO3DTransportError::Unsupported, FString::Printf(TEXT("No sender is registered for transport '%s'."), *ActiveConfig.Transport.ToString()));
        return false;
    }

    // From here on the controller owns a registry-tracked instance: release it if the transport
    // unregisters (ADR 0007 item 5).
    ActiveTransportName = SelectedTransportName;
    UnregisteringHandle = FO3DTransportRegistry::Get().OnTransportUnregistering().AddRaw(this, &FO3DSenderTransportController::HandleTransportUnregistering);

    // The callback must be set before Initialize and Start (IOpen3DSender). It posts into a mailbox
    // the owner drains on the game thread, never into the owner itself.
    if (StateMailbox.IsValid())
    {
        ActiveSender->SetStateChangedCallback(FO3DConnectionStateMailbox::MakeCallback(StateMailbox.ToSharedRef()));
    }

    LastResult = ActiveSender->Initialize(ActiveConfig);
    if (!LastResult.IsOk())
    {
        UE_LOG(LogO3DSenderComponent, Warning, TEXT("Failed to initialize sender transport '%s': %s"), *ActiveConfig.Transport.ToString(), *LexToString(LastResult));
        Unsubscribe();
        ActiveSender.Reset();
        return false;
    }

    LastResult = ActiveSender->Start();
    if (!LastResult.IsOk())
    {
        UE_LOG(LogO3DSenderComponent, Warning, TEXT("Failed to start sender transport '%s': %s"), *ActiveConfig.Transport.ToString(), *LexToString(LastResult));
        Stop();
        return false;
    }

    AudioSink.Reset();
    if (ActiveConfig.Audio.bEnableAudio)
    {
        if (ActiveSender->GetCapabilities().bAudioSend)
        {
            AudioSink = ActiveSender->CreateAudioSink(ActiveConfig.Audio);
            if (!AudioSink.IsValid())
            {
                UE_LOG(LogO3DSenderComponent, Warning, TEXT("Transport '%s' reported audio support but failed to provide a sink."), *ActiveConfig.Transport.ToString());
            }
        }
        else
        {
            UE_LOG(LogO3DSenderComponent, Warning, TEXT("Transport '%s' does not support audio; ignoring audio configuration."), *ActiveConfig.Transport.ToString());
        }
    }

    // The pipeline's worker sends the pose frames from now on (WP-A2c).
    if (Pipeline.IsValid())
    {
        Pipeline->AttachSender(ActiveSender);
    }

    return true;
}

void FO3DSenderTransportController::Stop()
{
    Unsubscribe();

    if (AudioSink.IsValid())
    {
        AudioSink->OnCaptureStopped();
        AudioSink.Reset();
    }

    // Before the sender stops: waits for a frame the pipeline's worker is sending, after which the
    // worker never calls this sender again (WP-A2c).
    if (Pipeline.IsValid())
    {
        Pipeline->DetachSender();
    }

    if (ActiveSender.IsValid())
    {
        ActiveSender->Stop();
        ActiveSender.Reset();
    }
}

bool FO3DSenderTransportController::IsActive() const
{
    return ActiveSender.IsValid();
}

void FO3DSenderTransportController::HandleTransportUnregistering(FName TransportName)
{
    if (TransportName != ActiveTransportName || !ActiveSender.IsValid())
    {
        return;
    }

    UE_LOG(LogO3DSenderComponent, Warning, TEXT("Sender transport '%s' is being unregistered (its module is shutting down); stopping and releasing the sender."), *TransportName.ToString());

    // The owner first drops what it holds (the audio capture's sink, the control publisher); it
    // may call Stop() itself. Copied, because the handler may replace itself.
    if (OnTransportUnregisteringHandler)
    {
        const TFunction<void()> Handler = OnTransportUnregisteringHandler;
        Handler();
    }

    Stop();
}

void FO3DSenderTransportController::Unsubscribe()
{
    if (UnregisteringHandle.IsValid())
    {
        // Removing during the registry's broadcast is safe: UE multicast delegates defer the
        // compaction of their invocation list until the broadcast ends.
        FO3DTransportRegistry::Get().OnTransportUnregistering().Remove(UnregisteringHandle);
        UnregisteringHandle.Reset();
    }
    ActiveTransportName = NAME_None;
}
