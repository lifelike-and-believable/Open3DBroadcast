// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

class UO3DReceiverSettingsObject;
struct FO3DReceiverSourceConfig;
struct FO3DTransportConfig;
class SO3DTransportConfigPanelBase;

struct FO3DReceiverTransportCustomization
{
    TFunction<void(const FO3DReceiverSourceConfig&, FO3DTransportConfig&)> ConfigureTransport;

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
    TFunction<TSharedPtr<SO3DTransportConfigPanelBase>(UO3DReceiverSettingsObject*, FSimpleDelegate /*OnSubmit*/)> BuildTransportWidget;
#endif // WITH_EDITOR
};

namespace O3DReceiver
{
    OPEN3DRECEIVER_API void RegisterTransportCustomization(FName TransportName, FO3DReceiverTransportCustomization&& Customization);
    OPEN3DRECEIVER_API void UnregisterTransportCustomization(FName TransportName);
    OPEN3DRECEIVER_API const FO3DReceiverTransportCustomization* FindTransportCustomization(FName TransportName);
    OPEN3DRECEIVER_API void GetRegisteredTransportNames(TArray<FName>& OutNames);

    /**
     * Copies the secret declaration (SecretOptionKeys, SecretEnvVars) of a registered transport
     * under the registry lock. Returns false, with empty outputs, when the transport has no
     * customization.
     */
    OPEN3DRECEIVER_API bool GetTransportSecretDeclaration(FName TransportName, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars);

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

    /** Exports Settings as a LiveLink connection string with every declared secret key removed. */
    OPEN3DRECEIVER_API FString ExportConnectionString(const FO3DReceiverSourceConfig& Settings);

    /** Resolves Settings' declared secrets from FO3DSecretStore into OutSecrets (key -> value). */
    OPEN3DRECEIVER_API void ResolveSecrets(const FO3DReceiverSourceConfig& Settings, TMap<FString, FString>& OutSecrets);
}
