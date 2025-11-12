#pragma once

#include "CoreMinimal.h"

#include "O3DTransportTypes.h"

class IOpen3DSender;
class IO3DSenderAudioSink;

/**
 * Owns the active sender transport instance for a capture component, encapsulating lifecycle
 * management (Create/Start/Stop) and surfacing associated audio sinks on demand.
 */
class FO3DSenderTransportController
{
public:
    FO3DSenderTransportController();

    bool Start(const FO3DTransportConfig& InConfig);
    void Stop();

    bool IsActive() const;
    const FO3DTransportConfig& GetConfig() const { return ActiveConfig; }
    TSharedPtr<IOpen3DSender> GetSender() const { return ActiveSender; }
    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> GetAudioSink() const { return AudioSink; }

private:
    FO3DTransportConfig ActiveConfig;
    TSharedPtr<IOpen3DSender> ActiveSender;
    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink;
};
