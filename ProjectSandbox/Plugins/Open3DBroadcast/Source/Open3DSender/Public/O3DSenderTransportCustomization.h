// Copyright (c) Open3DStream Contributors

#pragma once

// Deprecated, kept for one release (ADR 0007 item 9, WP-A1). A transport now sets
// FO3DTransportDescriptor::ConfigureSender and SenderOptions and registers the descriptor once
// with FO3DTransportRegistry ("Transport/O3DTransportRegistry.h"). These functions forward to that
// registry, so a transport built against the previous release keeps working. Removed in the next
// minor release together with an O3D_TRANSPORT_API_VERSION bump.

#include "CoreMinimal.h"
#include "O3DTransportOptionSchema.h"
#include "Templates/Function.h"
#include "Transport/O3DTransportRegistry.h"

class UO3DSenderComponent;
struct FO3DTransportConfig;

/** Deprecated: the sender part of FO3DTransportDescriptor (ConfigureSender, SenderOptions). */
struct FO3DSenderTransportCustomization
{
    /** Allow transports to translate component settings into FO3DTransportConfig prior to initialization. */
    TFunction<void(const UO3DSenderComponent*, FO3DTransportConfig&)> ConfigureTransport;

    /** Option keys whose values are credentials (ADR 0004). See FO3DTransportRoleOptions::SecretOptionKeys. */
    TArray<FString> SecretOptionKeys;

    /** Optional environment variable per secret key. See FO3DTransportRoleOptions::SecretEnvVars. */
    TMap<FString, FString> SecretEnvVars;

    /** The declared options. See FO3DTransportRoleOptions::OptionSchema. */
    FO3DTransportOptionSchema OptionSchema;
};

namespace O3DSender
{
    /**
     * Deprecated. Sets the sender configure function and options of the legacy descriptor for
     * TransportName, replacing earlier ones. Ignored, with a Warning, for a name registered
     * through FO3DTransportRegistry::Register.
     */
    OPEN3DSENDER_API void RegisterTransportCustomization(FName TransportName, FO3DSenderTransportCustomization&& Customization);

    /** Deprecated. Clears what RegisterTransportCustomization set. */
    OPEN3DSENDER_API void UnregisterTransportCustomization(FName TransportName);

    /**
     * Deprecated: use FO3DTransportRegistry::Get().Find(TransportName), which returns a shared
     * pointer that stays valid (RCV-27). Returns a copy of the sender part of the registered
     * descriptor, or null when it has none. The pointer stays valid until the transport is
     * registered again or unregistered.
     */
    OPEN3DSENDER_API const FO3DSenderTransportCustomization* FindTransportCustomization(FName TransportName);

    /** Deprecated. FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Sender). */
    OPEN3DSENDER_API void GetRegisteredTransportNames(TArray<FName>& OutNames);

    /** Deprecated. FO3DTransportRegistry::Get().GetSecretDeclaration(TransportName, EO3DTransportRole::Sender, ...). */
    OPEN3DSENDER_API bool GetTransportSecretDeclaration(FName TransportName, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars);

    /** Deprecated. FO3DTransportRegistry::Get().GetOptionSchema(TransportName, EO3DTransportRole::Sender, ...). */
    OPEN3DSENDER_API bool GetTransportOptionSchema(FName TransportName, FO3DTransportOptionSchema& OutSchema);
}
