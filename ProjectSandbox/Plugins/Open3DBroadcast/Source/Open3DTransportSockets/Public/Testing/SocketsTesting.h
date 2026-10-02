// Copyright (c) Open3DStream Contributors

#pragma once

// Test-only access to the TCP and UDP transports (ADR 0006, WP-T2). The transport classes stay
// private to this module; the Open3DBroadcastTests module creates them and reads their
// connection state only through these functions. Compiled out without dev automation tests.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"

namespace O3DSocketsTesting
{
	OPEN3DTRANSPORTSOCKETS_API TSharedRef<IOpen3DSender> CreateTcpSender();
	OPEN3DTRANSPORTSOCKETS_API TSharedRef<IOpen3DReceiver> CreateTcpReceiver();
	OPEN3DTRANSPORTSOCKETS_API TSharedRef<IOpen3DSender> CreateUdpSender();
	OPEN3DTRANSPORTSOCKETS_API TSharedRef<IOpen3DReceiver> CreateUdpReceiver();

	// The accessors below require an instance made by CreateTcpSender() or CreateTcpReceiver().

	/** True while a receiver is connected to the TCP sender's listen socket. */
	OPEN3DTRANSPORTSOCKETS_API bool TcpSenderHasClient(const IOpen3DSender& Sender);
	/** How often the TCP sender's worker waited for socket buffer space (TRB-2). */
	OPEN3DTRANSPORTSOCKETS_API int64 TcpSenderGetSendWaitCount(const IOpen3DSender& Sender);
	OPEN3DTRANSPORTSOCKETS_API bool TcpReceiverIsConnected(const IOpen3DReceiver& Receiver);
	/** Number of successful connects since Initialize (TRB-4). */
	OPEN3DTRANSPORTSOCKETS_API int32 TcpReceiverGetConnectCount(const IOpen3DReceiver& Receiver);
	/** Consecutive failed connects counted by the receiver worker's backoff policy (WP-A1 PR 4b). */
	OPEN3DTRANSPORTSOCKETS_API int32 TcpReceiverGetFailedConnectAttempts(const IOpen3DReceiver& Receiver);
}

#endif // WITH_DEV_AUTOMATION_TESTS
