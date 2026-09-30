// Copyright (c) Open3DStream Contributors

#pragma once

// The customization struct and its register, find and list functions are deprecated and kept for
// one release (ADR 0007 item 9, WP-A1). A transport now sets FO3DTransportDescriptor::
// ConfigureReceiver and ReceiverOptions and registers the descriptor once with
// FO3DTransportRegistry ("Transport/O3DTransportRegistry.h"). They forward to that registry, so a
// transport built against the previous release keeps working, and are removed in the next minor
// release together with an O3D_TRANSPORT_API_VERSION bump.
//
// The receiver secret helpers below them (IsSecretOptionKey onwards) are not deprecated.

#include "CoreMinimal.h"
#include "O3DTransportOptionSchema.h"
#include "Templates/Function.h"
#include "Transport/O3DTransportRegistry.h"

struct FO3DReceiverSourceConfig;
struct FO3DTransportConfig;

/** Deprecated: the receiver part of FO3DTransportDescriptor (ConfigureReceiver, ReceiverOptions). */
struct FO3DReceiverTransportCustomization
{
    TFunction<void(const FO3DReceiverSourceConfig&, FO3DTransportConfig&)> ConfigureTransport;

    /** Option keys whose values are credentials (ADR 0004). See FO3DTransportRoleOptions::SecretOptionKeys. */
    TArray<FString> SecretOptionKeys;

    /** Optional environment variable per secret key. See FO3DTransportRoleOptions::SecretEnvVars. */
    TMap<FString, FString> SecretEnvVars;

    /** The declared options. See FO3DTransportRoleOptions::OptionSchema. */
    FO3DTransportOptionSchema OptionSchema;
};

namespace O3DReceiver
{
    /**
     * Deprecated. Sets the receiver configure function and options of the legacy descriptor for
     * TransportName, replacing earlier ones. Ignored, with a Warning, for a name registered
     * through FO3DTransportRegistry::Register.
     */
    OPEN3DRECEIVER_API void RegisterTransportCustomization(FName TransportName, FO3DReceiverTransportCustomization&& Customization);

    /** Deprecated. Clears what RegisterTransportCustomization set. */
    OPEN3DRECEIVER_API void UnregisterTransportCustomization(FName TransportName);

    /**
     * Deprecated: use FO3DTransportRegistry::Get().Find(TransportName), which returns a shared
     * pointer that stays valid (RCV-27). Returns a copy of the receiver part of the registered
     * descriptor, or null when it has none. The pointer stays valid until the transport is
     * registered again or unregistered.
     */
    OPEN3DRECEIVER_API const FO3DReceiverTransportCustomization* FindTransportCustomization(FName TransportName);

    /** Deprecated. FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Receiver). */
    OPEN3DRECEIVER_API void GetRegisteredTransportNames(TArray<FName>& OutNames);

    /** Deprecated. FO3DTransportRegistry::Get().GetSecretDeclaration(TransportName, EO3DTransportRole::Receiver, ...). */
    OPEN3DRECEIVER_API bool GetTransportSecretDeclaration(FName TransportName, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars);

    /** Deprecated. FO3DTransportRegistry::Get().GetOptionSchema(TransportName, EO3DTransportRole::Receiver, ...). */
    OPEN3DRECEIVER_API bool GetTransportOptionSchema(FName TransportName, FO3DTransportOptionSchema& OutSchema);

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
