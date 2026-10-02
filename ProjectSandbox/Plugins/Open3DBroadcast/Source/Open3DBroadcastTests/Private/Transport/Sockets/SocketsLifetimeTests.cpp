// Copyright (c) Open3DStream Contributors
//
// WP-S5 acceptance (TRB-10, TRB-11, TRB-12): start/stop the TCP and UDP senders 1,000 times
// on 127.0.0.1 while a fake audio thread submits PCM, including cycles where the sender is
// destroyed while the audio thread still holds its sink. Run under ASan to catch
// use-after-free. No network beyond the loopback interface.

// Option keys are spelled out: they are the user-facing names persisted in settings
// (SocketsTransportCommon.h, SocketsTcpTransport.h), so these tests also pin them.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "Testing/O3DLifetimeTestUtils.h"
#include "Testing/SocketsTesting.h"

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

namespace
{
	/** Binds a socket of the given type to 127.0.0.1:0 and returns it with its port. */
	FSocket* BindLoopbackSocket(const FName SocketType, int32& OutPort)
	{
		OutPort = 0;
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return nullptr;
		}

		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bIsValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
		Addr->SetPort(0);

		FSocket* Socket = SocketSubsystem->CreateSocket(SocketType, TEXT("O3DS_LifetimeTestSocket"), false);
		if (!Socket)
		{
			return nullptr;
		}
		if (!bIsValid || !Socket->Bind(*Addr))
		{
			SocketSubsystem->DestroySocket(Socket);
			return nullptr;
		}
		Socket->GetAddress(*Addr);
		OutPort = Addr->GetPort();
		return Socket;
	}

	int32 FindFreeTcpPort()
	{
		int32 Port = 0;
		if (FSocket* Probe = BindLoopbackSocket(NAME_Stream, Port))
		{
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Probe);
		}
		return Port;
	}

	FO3DTransportConfig MakeSocketsConfig(const TCHAR* Scheme, int32 Port)
	{
		FO3DTransportConfig Config;
		// The registered name, "TCP" or "UDP" (TRB-27).
		Config.Transport = FName(*FString(Scheme).ToUpper());
		Config.Role = EO3DTransportRole::Sender;
		Config.Uri = FString::Printf(TEXT("%s://127.0.0.1:%d"), Scheme, Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(TEXT("bind"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 1;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsTcpLifetimeStressTest, "Open3DBroadcast.Transport.Sockets.Lifetime.TcpStartStopWithAudio", O3DB_TEST_FLAGS)
bool FO3DSocketsTcpLifetimeStressTest::RunTest(const FString& Parameters)
{
	const int32 Port = FindFreeTcpPort();
	TestTrue(TEXT("Loopback TCP port allocated"), Port > 0);
	if (Port <= 0)
	{
		return false;
	}

	// No client connects, so the sink rejects PCM (no peer), but every submit still runs the
	// gate and races Stop(); that is the path TRB-10/TRB-12 were about.
	const O3DLifetimeTest::FStressResult Result = O3DLifetimeTest::RunSenderStressWith(
		[]() -> TSharedPtr<IOpen3DSender> { return O3DSocketsTesting::CreateTcpSender(); },
		[Port](int32)
	{
		return MakeSocketsConfig(TEXT("tcp"), Port);
	}, /*bStart=*/true);

	TestEqual(TEXT("All cycles ran"), Result.CyclesRun, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("Every cycle started"), Result.StartFailures, 0);
	TestEqual(TEXT("Every cycle produced a sink"), Result.SinksCreated, Result.CyclesRun - Result.StartFailures);
	TestEqual(TEXT("No sink accepts PCM after its sender stopped"), Result.StaleSinkAccepted, 0);
	AddInfo(FString::Printf(TEXT("Fake audio thread submitted %lld buffers, %lld accepted"), Result.Submitted, Result.Accepted));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpLifetimeStressTest, "Open3DBroadcast.Transport.Sockets.Lifetime.UdpStartStopWithAudio", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpLifetimeStressTest::RunTest(const FString& Parameters)
{
	// A bound UDP socket on 127.0.0.1 receives (and discards) the datagrams, so audio sends
	// have a real destination and the audio worker exercises SendTo during Stop().
	int32 Port = 0;
	FSocket* SinkSocket = BindLoopbackSocket(NAME_DGram, Port);
	TestNotNull(TEXT("Loopback UDP sink socket bound"), SinkSocket);
	if (!SinkSocket)
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(SinkSocket);
	};

	const O3DLifetimeTest::FStressResult Result = O3DLifetimeTest::RunSenderStressWith(
		[]() -> TSharedPtr<IOpen3DSender> { return O3DSocketsTesting::CreateUdpSender(); },
		[Port](int32)
	{
		return MakeSocketsConfig(TEXT("udp"), Port);
	}, /*bStart=*/true);

	TestEqual(TEXT("All cycles ran"), Result.CyclesRun, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("Every cycle started"), Result.StartFailures, 0);
	TestEqual(TEXT("Every cycle produced a sink"), Result.SinksCreated, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("No sink accepts PCM after its sender stopped"), Result.StaleSinkAccepted, 0);
	AddInfo(FString::Printf(TEXT("Fake audio thread submitted %lld buffers, %lld accepted"), Result.Submitted, Result.Accepted));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
