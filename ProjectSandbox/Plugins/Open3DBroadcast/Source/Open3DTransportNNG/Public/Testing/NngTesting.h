// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

// Test-only access to the NNG transport (ADR 0006, WP-T2). The sender and receiver classes stay
// private to this module. Compiled out without dev automation tests.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DReceiverInterface.h"
#include "O3DSenderInterface.h"
#include "O3DTransportTypes.h"

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

	/** What the option parser made of a config (TRB-39, TRB-40). Strings as written in URIs. */
	struct FResolvedEndpoint
	{
		FString Mode;
		FString Role;
		FString Host;
		int32 Port = 0;
		FString TcpAddress;
		bool bListen = false;
	};

	/** Runs the sender or receiver option parser on Config. False (with OutError) if it rejects it. */
	OPEN3DTRANSPORTNNG_API bool ResolveEndpoint(const FO3DTransportConfig& Config, bool bSender, FResolvedEndpoint& OutEndpoint, FString& OutError);
}

#endif // WITH_DEV_AUTOMATION_TESTS
