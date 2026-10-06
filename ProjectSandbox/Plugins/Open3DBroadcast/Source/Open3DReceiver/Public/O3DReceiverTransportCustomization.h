// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

// Receiver transport helpers: secret options (ADR 0004) and transport switching (SND-35). A
// transport itself sets FO3DTransportDescriptor::ConfigureReceiver and ReceiverOptions and
// registers the descriptor once with FO3DTransportRegistry ("Transport/O3DTransportRegistry.h").

#include "CoreMinimal.h"
#include "O3DTransportOptionSchema.h"
#include "Transport/O3DTransportRegistry.h"

struct FO3DReceiverSourceConfig;
struct FO3DTransportConfig;

namespace O3DReceiver
{
    /** True when Key is a declared secret option key of TransportName. */
    OPEN3DRECEIVER_API bool IsSecretOptionKey(FName TransportName, const FString& Key);

    /** The credential profile Settings selects ("<transport>.credentialProfile"), "default" when unset. */
    OPEN3DRECEIVER_API FString GetCredentialProfile(const FO3DReceiverSourceConfig& Settings);

    /**
     * Removes every declared secret key of Settings' transport from Settings.TransportOptions.
     * Returns the removed keys (never values). Used before ExportText and SaveConfig.
     */
    OPEN3DRECEIVER_API TArray<FString> StripSecretOptions(FO3DReceiverSourceConfig& Settings);

    /**
     * Migration (ADR 0004 item 4): moves declared secret keys found in Settings.TransportOptions
     * into the session store (keeping a session value that already exists), removes them from
     * Settings and logs one Warning naming SourceDescription and the keys, never the values.
     * Returns the number of keys moved.
     */
    OPEN3DRECEIVER_API int32 MigrateLegacySecretOptions(FO3DReceiverSourceConfig& Settings, const FString& SourceDescription);

    /**
     * Exports Settings as a LiveLink connection string with every declared secret key removed and
     * without InactiveTransportOptions (the source uses only the selected transport's options).
     */
    OPEN3DRECEIVER_API FString ExportConnectionString(const FO3DReceiverSourceConfig& Settings);

    /**
     * Why a source with Settings could not start, or empty when it could (RCV-17): the transport
     * has no receiver registered, or its options (with the project defaults applied) fail the
     * transport's validation. The LiveLink Create Source panel shows it and disables Create.
     */
    OPEN3DRECEIVER_API FText ValidateNewSource(const FO3DReceiverSourceConfig& Settings);

    /** Resolves Settings' declared secrets from FO3DSecretStore into OutSecrets (key -> value). */
    OPEN3DRECEIVER_API void ResolveSecrets(const FO3DReceiverSourceConfig& Settings, TMap<FString, FString>& OutSecrets);

    /**
     * Selects NewTransport and switches the options with it (SND-35,
     * WP-A1 PR 5a): the outgoing transport's TransportOptions go to InactiveTransportOptions,
     * without its declared secret keys, and the incoming transport's come back. Returns false and
     * changes nothing when the transport is already selected. The caller records the change for
     * undo (the owning object's Modify) first. Game thread.
     */
    OPEN3DRECEIVER_API bool SwitchTransport(FO3DReceiverSourceConfig& Settings, FName NewTransport);
}
