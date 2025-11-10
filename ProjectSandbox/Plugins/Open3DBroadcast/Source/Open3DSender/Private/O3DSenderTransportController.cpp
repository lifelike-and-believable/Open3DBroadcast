#include "O3DSenderTransportController.h"

#include "O3DSenderInterface.h"
#include "O3DSenderLogs.h"
#include "O3DSenderRegistry.h"

FO3DSenderTransportController::FO3DSenderTransportController() = default;

bool FO3DSenderTransportController::Start(const FO3DTransportConfig& InConfig)
{
    Stop();

    ActiveConfig = InConfig;
    if (ActiveConfig.Transport.IsEmpty())
    {
        UE_LOG(LogO3DSenderComponent, Warning, TEXT("No transport specified; skipping auto transport setup."));
        return false;
    }

    const FName SelectedTransportName(*ActiveConfig.Transport);
    ActiveSender = O3DTransport::CreateSender(SelectedTransportName);
    if (!ActiveSender.IsValid())
    {
        UE_LOG(LogO3DSenderComponent, Warning, TEXT("No sender registered for transport '%s'."), *ActiveConfig.Transport);
        return false;
    }

    if (!ActiveSender->Initialize(ActiveConfig))
    {
        UE_LOG(LogO3DSenderComponent, Warning, TEXT("Failed to initialize sender transport '%s'."), *ActiveConfig.Transport);
        ActiveSender.Reset();
        return false;
    }

    if (!ActiveSender->Start())
    {
        UE_LOG(LogO3DSenderComponent, Warning, TEXT("Failed to start sender transport '%s'."), *ActiveConfig.Transport);
        ActiveSender->Stop();
        ActiveSender.Reset();
        return false;
    }

    AudioSink.Reset();
    if (ActiveConfig.Audio.bEnableAudio)
    {
        if (ActiveSender->SupportsAudio())
        {
            AudioSink = ActiveSender->CreateAudioSink(ActiveConfig.Audio);
            if (!AudioSink.IsValid())
            {
                UE_LOG(LogO3DSenderComponent, Warning, TEXT("Transport '%s' reported audio support but failed to provide a sink."), *ActiveConfig.Transport);
            }
        }
        else
        {
            UE_LOG(LogO3DSenderComponent, Warning, TEXT("Transport '%s' does not support audio; ignoring audio configuration."), *ActiveConfig.Transport);
        }
    }

    return true;
}

void FO3DSenderTransportController::Stop()
{
    if (AudioSink.IsValid())
    {
        AudioSink->OnCaptureStopped();
        AudioSink.Reset();
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