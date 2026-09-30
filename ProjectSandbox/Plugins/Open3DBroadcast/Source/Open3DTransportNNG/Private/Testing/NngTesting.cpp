// Copyright (c) Open3DStream Contributors

#include "Testing/NngTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Receiver/NngReceiver.h"
#include "Sender/NngSender.h"

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
}

#endif // WITH_DEV_AUTOMATION_TESTS
