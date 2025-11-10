#pragma once

#include "O3DTransportTypes.h"

#if __has_include("Open3DSender/O3DSenderRegistry.h")
#include "Open3DSender/O3DSenderRegistry.h"
#endif

#if __has_include("Open3DReceiver/O3DReceiverRegistry.h")
#include "Open3DReceiver/O3DReceiverRegistry.h"
#endif

// Transitional umbrella header preserved for convenience. Prefer including the
// sender-only / receiver-only registry headers directly to minimise cross-module
// dependencies when compiling sender-only or receiver-only builds.
