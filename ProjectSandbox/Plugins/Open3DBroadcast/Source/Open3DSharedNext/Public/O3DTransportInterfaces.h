#pragma once

#include "O3DTransportTypes.h"

#if __has_include("Open3DSender/O3DSenderInterface.h")
#include "Open3DSender/O3DSenderInterface.h"
#endif

#if __has_include("Open3DReceiver/O3DReceiverInterface.h")
#include "Open3DReceiver/O3DReceiverInterface.h"
#endif

// Transitional umbrella header preserved for convenience. Prefer including the specific
// sender/receiver interface headers to minimise dependencies for sender-only / receiver-only builds.
