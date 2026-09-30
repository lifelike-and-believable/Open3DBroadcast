// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "O3DTransportOptionSchema.h"
#include "Templates/Function.h"

class UO3DSenderComponent;
struct FO3DTransportConfig;

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

    /**
     * The options this transport reads from UO3DSenderComponent::TransportOptions, as data. The
     * Open3DBroadcastEditor module builds the Details panel rows from it (ADR 0010 §4). Every
     * Secret entry's key must also be in SecretOptionKeys. Same layout in every build
     * configuration: this struct has no WITH_EDITOR members (SND-34).
     */
    FO3DTransportOptionSchema OptionSchema;
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

    /**
     * Copies the option schema of a registered transport under the registry lock. Returns false,
     * with an empty output, when the transport has no customization.
     */
    OPEN3DSENDER_API bool GetTransportOptionSchema(FName TransportName, FO3DTransportOptionSchema& OutSchema);
}
