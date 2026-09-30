// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

class UO3DSenderComponent;
struct FO3DTransportConfig;
class SWidget;

struct FO3DSenderTransportCustomization
{
    /** Allow transports to translate component settings into FO3DTransportConfig prior to initialization. */
    TFunction<void(const UO3DSenderComponent*, FO3DTransportConfig&)> ConfigureTransport;

    /**
     * Option keys whose values are credentials (ADR 0004). Never persisted, never logged.
     * SetTransportOption routes them to FO3DSecretStore; BuildTransportConfig resolves them into
     * FO3DTransportConfig::Secrets. Keys not listed here are treated as non-secret for persistence.
     */
    TArray<FString> SecretOptionKeys;

    /**
     * Optional environment variable per secret key, e.g. {"<transport>.token", "O3DB_<TRANSPORT>_TOKEN"}.
     * A non-default credential profile first tries "<NAME>__<PROFILE>".
     */
    TMap<FString, FString> SecretEnvVars;

#if WITH_EDITOR
    /**
     * Build an editor widget that edits transport-specific options. The provided delegate should be
     * executed whenever the widget mutates component state so previews can refresh.
     */
    TFunction<TSharedPtr<SWidget>(UO3DSenderComponent*, FSimpleDelegate /*OnConfigChanged*/)> BuildTransportWidget;
#endif // WITH_EDITOR
};

namespace O3DSender
{
    OPEN3DSENDER_API void RegisterTransportCustomization(FName TransportName, FO3DSenderTransportCustomization&& Customization);
    OPEN3DSENDER_API void UnregisterTransportCustomization(FName TransportName);
    OPEN3DSENDER_API const FO3DSenderTransportCustomization* FindTransportCustomization(FName TransportName);
    OPEN3DSENDER_API void GetRegisteredTransportNames(TArray<FName>& OutNames);

    /**
     * Copies the secret declaration (SecretOptionKeys, SecretEnvVars) of a registered transport
     * under the registry lock. Returns false, with empty outputs, when the transport has no
     * customization.
     */
    OPEN3DSENDER_API bool GetTransportSecretDeclaration(FName TransportName, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars);
}
