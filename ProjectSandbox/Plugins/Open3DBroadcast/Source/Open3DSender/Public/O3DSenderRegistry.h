// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

// Deprecated, kept for one release (ADR 0007 item 9, WP-A1). Register one descriptor with
// FO3DTransportRegistry::Get().Register(...) and create instances with
// FO3DTransportRegistry::Get().CreateSender(...) ("Transport/O3DTransportRegistry.h") instead.
// These functions forward to that registry, so a transport built against the previous release
// keeps working. Removed in the next minor release together with an O3D_TRANSPORT_API_VERSION bump.

#include "CoreMinimal.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DTransport
{
    /**
     * Deprecated. Sets the sender factory of the legacy descriptor for TransportName, replacing an
     * earlier one. Ignored, with a Warning, for a name registered through
     * FO3DTransportRegistry::Register.
     */
    OPEN3DSENDER_API void RegisterSender(FName TransportName, FO3DSenderFactory&& Factory);

    /** Deprecated. Removes the sender factory set by RegisterSender. */
    OPEN3DSENDER_API void UnregisterSender(FName TransportName);

    /** Deprecated. FO3DTransportRegistry::Get().CreateSender(TransportName). */
    OPEN3DSENDER_API TSharedPtr<IOpen3DSender> CreateSender(FName TransportName);

    /** Deprecated. FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Sender). */
    OPEN3DSENDER_API TArray<FName> GetRegisteredSenders();
}
