#pragma once

#include "CoreMinimal.h"
#include "O3DSenderInterface.h"

namespace O3DTransport
{
    /** Register a sender factory under the supplied transport name. Overrides existing registration. */
    OPEN3DSENDER_API void RegisterSender(FName TransportName, FO3DSenderFactory&& Factory);

    /** Remove a previously registered sender factory. */
    OPEN3DSENDER_API void UnregisterSender(FName TransportName);

    /** Instantiate a sender for the given transport name, or nullptr if none registered. */
    OPEN3DSENDER_API TSharedPtr<IOpen3DSender> CreateSender(FName TransportName);

    /** Snapshot the list of currently registered sender transport names. */
    OPEN3DSENDER_API TArray<FName> GetRegisteredSenders();
}
