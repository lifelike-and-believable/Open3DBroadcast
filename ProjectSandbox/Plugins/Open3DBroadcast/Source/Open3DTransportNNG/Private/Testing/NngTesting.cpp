// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_NNG // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Testing/NngTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Receiver/NngReceiver.h"
#include "Sender/NngSender.h"
#include "Shared/NngHelpers.h"

/** Befriended by FO3DNngReceiver; kept inside the module so tests never see its private members. */
struct FO3DNngReceiverTestAccessor
{
	static bool ProcessReceivedPayload(FO3DNngReceiver& Receiver, const TArray<uint8>& Bytes)
	{
		return Receiver.ProcessReceivedPayload(Bytes.GetData(), Bytes.Num());
	}
};

namespace O3DNngTesting
{
	TSharedRef<IOpen3DSender> CreateSender()
	{
		return MakeShared<FO3DNngSender>();
	}

	TSharedRef<IOpen3DReceiver> CreateReceiver()
	{
		return MakeShared<FO3DNngReceiver>();
	}

	bool ProcessReceivedPayload(IOpen3DReceiver& Receiver, const TArray<uint8>& Bytes)
	{
		return FO3DNngReceiverTestAccessor::ProcessReceivedPayload(static_cast<FO3DNngReceiver&>(Receiver), Bytes);
	}

	bool ResolveEndpoint(const FO3DTransportConfig& Config, bool bSender, FResolvedEndpoint& OutEndpoint, FString& OutError)
	{
		OutEndpoint = FResolvedEndpoint();
		if (bSender)
		{
			O3DNNG::FNngSenderOptions Options;
			if (!O3DNNG::ParseSenderOptions(Config, Options, OutError))
			{
				return false;
			}
			OutEndpoint.Mode = O3DNNG::ModeToString(Options.Mode);
			OutEndpoint.Role = O3DNNG::RoleToString(Options.Role);
			OutEndpoint.Host = Options.Host;
			OutEndpoint.Port = Options.Port;
			OutEndpoint.TcpAddress = Options.TcpAddress;
			OutEndpoint.bListen = Options.bListen;
			return true;
		}

		O3DNNG::FNngReceiverOptions Options;
		if (!O3DNNG::ParseReceiverOptions(Config, Options, OutError))
		{
			return false;
		}
		OutEndpoint.Mode = O3DNNG::ModeToString(Options.Mode);
		OutEndpoint.Role = O3DNNG::RoleToString(Options.Role);
		OutEndpoint.Host = Options.Host;
		OutEndpoint.Port = Options.Port;
		OutEndpoint.TcpAddress = Options.TcpAddress;
		OutEndpoint.bListen = Options.bListen;
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_NNG
