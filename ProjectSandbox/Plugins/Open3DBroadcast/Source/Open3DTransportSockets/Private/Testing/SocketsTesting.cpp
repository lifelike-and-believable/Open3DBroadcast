// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Testing/SocketsTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Receiver/SocketsTcpReceiver.h"
#include "Receiver/SocketsUdpReceiver.h"
#include "Sender/SocketsTcpSender.h"
#include "Sender/SocketsUdpSender.h"

namespace O3DSocketsTesting
{
	TSharedRef<IOpen3DSender> CreateTcpSender()
	{
		return MakeShared<FO3DSocketsTcpSender>();
	}

	TSharedRef<IOpen3DReceiver> CreateTcpReceiver()
	{
		return MakeShared<FO3DSocketsTcpReceiver>();
	}

	TSharedRef<IOpen3DSender> CreateUdpSender()
	{
		return MakeShared<FO3DSocketsUdpSender>();
	}

	TSharedRef<IOpen3DReceiver> CreateUdpReceiver()
	{
		return MakeShared<FO3DSocketsUdpReceiver>();
	}

	bool TcpSenderHasClient(const IOpen3DSender& Sender)
	{
		return static_cast<const FO3DSocketsTcpSender&>(Sender).HasClient();
	}

	int64 TcpSenderGetSendWaitCount(const IOpen3DSender& Sender)
	{
		return static_cast<const FO3DSocketsTcpSender&>(Sender).GetSendWaitCount();
	}

	bool TcpReceiverIsConnected(const IOpen3DReceiver& Receiver)
	{
		return static_cast<const FO3DSocketsTcpReceiver&>(Receiver).IsConnected();
	}

	int32 TcpReceiverGetConnectCount(const IOpen3DReceiver& Receiver)
	{
		return static_cast<const FO3DSocketsTcpReceiver&>(Receiver).GetConnectCount();
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_SOCKETS
