// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

// Deprecated, kept for one release (ADR 0007 item 9, WP-A1). Register one descriptor with
// FO3DTransportRegistry::Get().Register(...) and create instances with
// FO3DTransportRegistry::Get().CreateReceiver(...) ("Transport/O3DTransportRegistry.h") instead.
// These functions forward to that registry, so a transport built against the previous release
// keeps working. Removed in the next minor release together with an O3D_TRANSPORT_API_VERSION bump.

#include "CoreMinimal.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DTransport
{
    /**
     * Deprecated. Sets the receiver factory of the legacy descriptor for TransportName, replacing
     * an earlier one. Ignored, with a Warning, for a name registered through
     * FO3DTransportRegistry::Register.
     */
    OPEN3DRECEIVER_API void RegisterReceiver(FName TransportName, FO3DReceiverFactory&& Factory);

    /** Deprecated. Removes the receiver factory set by RegisterReceiver. */
    OPEN3DRECEIVER_API void UnregisterReceiver(FName TransportName);

    /** Deprecated. FO3DTransportRegistry::Get().CreateReceiver(TransportName). */
    OPEN3DRECEIVER_API TSharedPtr<IOpen3DReceiver> CreateReceiver(FName TransportName);

    /** Deprecated. FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Receiver). */
    OPEN3DRECEIVER_API TArray<FName> GetRegisteredReceivers();
}
