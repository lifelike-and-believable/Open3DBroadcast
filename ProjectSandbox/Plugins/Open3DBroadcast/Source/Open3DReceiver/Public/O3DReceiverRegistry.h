#pragma once

#include "CoreMinimal.h"
#include "O3DReceiverInterface.h"

namespace O3DTransport
{
    /** Register a receiver factory under the supplied transport name. Overrides existing registration. */
    OPEN3DRECEIVER_API void RegisterReceiver(FName TransportName, FO3DReceiverFactory&& Factory);

    /** Remove a previously registered receiver factory. */
    OPEN3DRECEIVER_API void UnregisterReceiver(FName TransportName);

    /** Instantiate a receiver for the given transport name, or nullptr if none registered. */
    OPEN3DRECEIVER_API TSharedPtr<IOpen3DReceiver> CreateReceiver(FName TransportName);

    /** Snapshot the list of currently registered receiver transport names. */
    OPEN3DRECEIVER_API TArray<FName> GetRegisteredReceivers();
}
