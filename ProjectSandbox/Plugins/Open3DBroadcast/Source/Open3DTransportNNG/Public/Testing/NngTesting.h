// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

// Test-only access to the NNG transport (ADR 0006, WP-T2). The sender and receiver classes stay
// private to this module. Compiled out without dev automation tests.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"

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

	/**
	 * Pauses or resumes the sender's worker (WP-A1 PR 4d): while paused it sends nothing, so the
	 * send queue's policy can be observed. Sender must come from CreateSender().
	 */
	OPEN3DTRANSPORTNNG_API void SenderSetWorkerPaused(IOpen3DSender& Sender, bool bPaused);

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
