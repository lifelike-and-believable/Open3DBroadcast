// Copyright (c) Open3DStream Contributors
//
// WP-S5 acceptance (TRB-35, TRB-11, TRB-12, NNG pipe-callback tokens): start/stop the NNG
// sender 1,000 times while a fake audio thread submits PCM, including cycles where the sender
// is destroyed while the audio thread still holds its sink. The NNG helpers only build tcp://
// addresses, so this listens on 127.0.0.1 (no inproc/ipc support in the option parser).
// Run under ASan to catch use-after-free.

// Option keys are spelled out: they are the user-facing names persisted in settings
// (NngHelpers.h), so these tests also pin them.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG

#include "Testing/NngTesting.h"
#include "Testing/O3DLifetimeTestUtils.h"

#include "Misc/AutomationTest.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

namespace
{
	int32 FindFreeLoopbackTcpPort()
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return 0;
		}

		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bIsValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
		Addr->SetPort(0);

		FSocket* Probe = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_NngLifetimePortProbe"), false);
		if (!Probe)
		{
			return 0;
		}

		int32 Port = 0;
		if (bIsValid && Probe->Bind(*Addr))
		{
			Probe->GetAddress(*Addr);
			Port = Addr->GetPort();
		}
		SocketSubsystem->DestroySocket(Probe);
		return Port;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngLifetimeStressTest, "Open3DBroadcast.Transport.NNG.Lifetime.StartStopWithAudio", O3DB_TEST_FLAGS)
bool FO3DNngLifetimeStressTest::RunTest(const FString& Parameters)
{
	const O3DLifetimeTest::FStressResult Result = O3DLifetimeTest::RunSenderStressWith(
		[]() -> TSharedPtr<IOpen3DSender> { return O3DNngTesting::CreateSender(); },
		[](int32)
	{
		// A fresh port per cycle so a lingering listener can never make Start() fail.
		const int32 Port = FindFreeLoopbackTcpPort();
		FO3DTransportConfig Config;
		Config.Transport = TEXT("nng");
		Config.Role = TEXT("sender");
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(TEXT("nng.mode"), TEXT("pub"));
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		Config.AdvancedParams.Add(TEXT("nng.role"), TEXT("server"));
		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 1;
		return Config;
	}, /*bStart=*/true);

	TestEqual(TEXT("All cycles ran"), Result.CyclesRun, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("Every cycle started"), Result.StartFailures, 0);
	TestEqual(TEXT("Every cycle produced a sink"), Result.SinksCreated, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("No sink accepts PCM after its sender stopped"), Result.StaleSinkAccepted, 0);
	AddInfo(FString::Printf(TEXT("Fake audio thread submitted %lld buffers, %lld accepted"), Result.Submitted, Result.Accepted));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG
