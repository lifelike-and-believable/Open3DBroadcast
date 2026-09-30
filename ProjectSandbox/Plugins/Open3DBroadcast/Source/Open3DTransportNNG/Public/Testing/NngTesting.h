// Copyright (c) Open3DStream Contributors

#pragma once

// Test-only access to the NNG transport (ADR 0006, WP-T2). The sender and receiver classes stay
// private to this module. Compiled out without dev automation tests.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DReceiverInterface.h"
#include "O3DSenderInterface.h"

namespace O3DNngTesting
{
	OPEN3DTRANSPORTNNG_API TSharedRef<IOpen3DSender> CreateSender();
	OPEN3DTRANSPORTNNG_API TSharedRef<IOpen3DReceiver> CreateReceiver();

	/**
	 * Runs the receiver's demux on Bytes as if they had arrived from the socket (TRB-37). The
	 * consumer set with SetConsumer() is called on this thread. Receiver must come from
	 * CreateReceiver().
	 */
	OPEN3DTRANSPORTNNG_API bool ProcessReceivedPayload(IOpen3DReceiver& Receiver, const TArray<uint8>& Bytes);
}

#endif // WITH_DEV_AUTOMATION_TESTS
