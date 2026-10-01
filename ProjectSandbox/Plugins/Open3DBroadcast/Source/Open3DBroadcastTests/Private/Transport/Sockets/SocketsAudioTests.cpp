// Copyright (c) Open3DStream Contributors
//
// TCP audio path on 127.0.0.1. The transport is reached through Testing/SocketsTesting.h (WP-T2).
// Option keys are spelled out: they are the user-facing names persisted in settings
// (SocketsTransportCommon.h, SocketsTcpTransport.h), so these tests also pin them.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "Testing/SocketsTesting.h"

#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "Transport/O3DTransportTypes.h"
#include "Transport/O3DSerializedFrameConsumer.h"

namespace
{
	class FSocketsTestAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& InMeta, const uint8* Data, int32 NumBytes) override
		{
			Meta = InMeta;
			Payload.Reset();
			if (Data && NumBytes > 0)
			{
				Payload.AddUninitialized(NumBytes);
				FMemory::Memcpy(Payload.GetData(), Data, NumBytes);
			}
			bInvoked = true;
		}

		bool WasInvoked() const { return bInvoked; }
		const O3DS::FAudioFrameMeta& GetMeta() const { return Meta; }
		const TArray<uint8>& GetPayload() const { return Payload; }
		void Reset()
		{
			bInvoked = false;
			Payload.Reset();
		}

	private:
		bool bInvoked = false;
		O3DS::FAudioFrameMeta Meta;
		TArray<uint8> Payload;
	};

	class FNullFrameConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString&, const TArray<uint8>&, double) override {}
	};

	int32 FindAvailableAudioTestPort()
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return 0;
		}

		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bIsValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
		if (!bIsValid)
		{
			return 0;
		}
		Addr->SetPort(0);

		FSocket* TempSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("SocketsTransportTestPortProbe"), false);
		if (!TempSocket)
		{
			return 0;
		}

		TempSocket->SetReuseAddr(true);
		int32 Port = 0;
		if (TempSocket->Bind(*Addr))
		{
			TempSocket->Listen(1);
			TempSocket->GetAddress(*Addr);
			Port = Addr->GetPort();
		}

		SocketSubsystem->DestroySocket(TempSocket);
		return Port;
	}

	/** Polls and ticks both sides for DurationSeconds of wall time. Yields; never sleeps. */
	void PumpTcpTransports(IOpen3DSender& Sender, IOpen3DReceiver& Receiver, double DurationSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + DurationSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Receiver.Poll();
			Sender.Tick(0.0f);
			FPlatformProcess::YieldThread();
		}
	}

	FO3DTransportConfig BuildTcpAudioSenderConfig(int32 DataPort, int32 AudioPort)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("sockets.tcp");
		Config.Role = TEXT("sender");
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), DataPort);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), DataPort);
		Config.AdvancedParams.Add(TEXT("bind"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(DataPort));
		Config.AdvancedParams.Add(TEXT("audio.bind"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("audio.port"), FString::FromInt(AudioPort));

		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 2;
		return Config;
	}

	FO3DTransportConfig BuildTcpAudioReceiverConfig(int32 DataPort, int32 AudioPort)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("sockets.tcp");
		Config.Role = TEXT("receiver");
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), DataPort);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), DataPort);
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(DataPort));
		Config.AdvancedParams.Add(TEXT("audio.host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("audio.port"), FString::FromInt(AudioPort));

		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 2;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsAudioRoundTripTest, "Open3DBroadcast.Transport.Sockets.Tcp.AudioRoundTrip", O3DB_TEST_FLAGS)
bool FO3DSocketsAudioRoundTripTest::RunTest(const FString& Parameters)
{
	const int32 DataPort = FindAvailableAudioTestPort();
	TestTrue(TEXT("Data port allocated"), DataPort > 0);

	int32 AudioPort = 0;
	for (int32 Attempt = 0; Attempt < 5 && (AudioPort == 0 || AudioPort == DataPort); ++Attempt)
	{
		AudioPort = FindAvailableAudioTestPort();
	}
	TestTrue(TEXT("Audio port allocated"), AudioPort > 0);
	TestNotEqual(TEXT("Distinct ports"), DataPort, AudioPort);

	FO3DTransportConfig SenderConfig = BuildTcpAudioSenderConfig(DataPort, AudioPort);
	FO3DTransportConfig ReceiverConfig = BuildTcpAudioReceiverConfig(DataPort, AudioPort);

	const TSharedRef<IOpen3DSender> SenderRef = O3DSocketsTesting::CreateTcpSender();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = O3DSocketsTesting::CreateTcpReceiver();
	IOpen3DSender& Sender = *SenderRef;
	IOpen3DReceiver& Receiver = *ReceiverRef;

	TestTrue(TEXT("Sender initializes"), Sender.Initialize(SenderConfig).IsOk());
	TestTrue(TEXT("Receiver initializes"), Receiver.Initialize(ReceiverConfig).IsOk());

	TSharedPtr<FNullFrameConsumer> FrameConsumer = MakeShared<FNullFrameConsumer>();
	Receiver.SetConsumer(FrameConsumer);

	TSharedPtr<FSocketsTestAudioSink, ESPMode::ThreadSafe> ReceiverAudioSink = MakeShared<FSocketsTestAudioSink, ESPMode::ThreadSafe>();
	Receiver.SetAudioSink(ReceiverAudioSink, ReceiverConfig.Audio);

	TestTrue(TEXT("Sender starts"), Sender.Start().IsOk());
	TestTrue(TEXT("Receiver starts"), Receiver.Start().IsOk());

	TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> SenderAudioSink = Sender.CreateAudioSink(SenderConfig.Audio);
	TestTrue(TEXT("Sender audio sink created"), SenderAudioSink.IsValid());

	const int32 NumFrames = 4;
	const int32 NumChannels = SenderConfig.Audio.NumChannels;
	TArray<float> Samples;
	Samples.SetNumUninitialized(NumFrames * NumChannels);
	for (int32 Index = 0; Index < NumFrames * NumChannels; ++Index)
	{
		Samples[Index] = (static_cast<float>(Index) / 4.0f) - 0.5f;
	}

	const double TimeoutSeconds = 5.0;
	const double StartTime = FPlatformTime::Seconds();
	bool bSubmitted = false;

	while ((FPlatformTime::Seconds() - StartTime) < TimeoutSeconds && !bSubmitted)
	{
		PumpTcpTransports(Sender, Receiver, 0.05);
		bSubmitted = SenderAudioSink->SubmitPcm(TEXT("audio_test"), Samples.GetData(), NumFrames, NumChannels, SenderConfig.Audio.SampleRate, 123.45);
	}
	TestTrue(TEXT("Audio frame submitted"), bSubmitted);

	const double ReceiveStart = FPlatformTime::Seconds();
	while ((FPlatformTime::Seconds() - ReceiveStart) < TimeoutSeconds && !ReceiverAudioSink->WasInvoked())
	{
		PumpTcpTransports(Sender, Receiver, 0.05);
	}

	TestTrue(TEXT("Receiver sink invoked"), ReceiverAudioSink->WasInvoked());

	if (ReceiverAudioSink->WasInvoked())
	{
		const TArray<uint8>& Payload = ReceiverAudioSink->GetPayload();
		TestEqual(TEXT("Payload size"), Payload.Num(), NumFrames * NumChannels * static_cast<int32>(sizeof(int16)));

		const int16* PcmData = reinterpret_cast<const int16*>(Payload.GetData());
		const int32 ExpectedFirst = FMath::Clamp(FMath::RoundToInt(Samples[0] * 32767.0f), -32768, 32767);
		TestEqual(TEXT("PCM16 conversion"), PcmData[0], static_cast<int16>(ExpectedFirst));
		// The label the capture path passes is what goes on the wire, as the Loopback audio test
		// also asserts: the shared sink code keeps one encoder per submitted label (ADR 0007 WP-S5
		// addendum). The old expectation, the StreamId, matched neither this nor the pre-WP-S5 code.
		TestEqual(TEXT("Meta stream label is the submitted label"), ReceiverAudioSink->GetMeta().StreamLabel, FString(TEXT("audio_test")));
		TestEqual(TEXT("Meta channel count"), ReceiverAudioSink->GetMeta().NumChannels, NumChannels);
		TestEqual(TEXT("Meta sample rate"), ReceiverAudioSink->GetMeta().SampleRate, SenderConfig.Audio.SampleRate);
	}

	Receiver.Stop();
	Sender.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsAudioQueueOverflowTest, "Open3DBroadcast.Transport.Sockets.Tcp.AudioRejectedWithoutClient", O3DB_TEST_FLAGS)
bool FO3DSocketsAudioQueueOverflowTest::RunTest(const FString& Parameters)
{
	const int32 DataPort = FindAvailableAudioTestPort();
	TestTrue(TEXT("Data port allocated"), DataPort > 0);

	int32 AudioPort = 0;
	for (int32 Attempt = 0; Attempt < 5 && (AudioPort == 0 || AudioPort == DataPort); ++Attempt)
	{
		AudioPort = FindAvailableAudioTestPort();
	}
	TestTrue(TEXT("Audio port allocated"), AudioPort > 0);
	TestNotEqual(TEXT("Ports differ"), DataPort, AudioPort);

	FO3DTransportConfig SenderConfig = BuildTcpAudioSenderConfig(DataPort, AudioPort);
	const TSharedRef<IOpen3DSender> SenderRef = O3DSocketsTesting::CreateTcpSender();
	IOpen3DSender& Sender = *SenderRef;

	TestTrue(TEXT("Sender initializes"), Sender.Initialize(SenderConfig).IsOk());
	TestTrue(TEXT("Sender starts"), Sender.Start().IsOk());

	TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> SenderAudioSink = Sender.CreateAudioSink(SenderConfig.Audio);
	TestTrue(TEXT("Audio sink created"), SenderAudioSink.IsValid());

	const float SampleValue = 0.25f;
	const bool bFrameAccepted = SenderAudioSink->SubmitPcm(SenderConfig.StreamId, &SampleValue, 1, 1, SenderConfig.Audio.SampleRate, 0.0);
	TestFalse(TEXT("Frame rejected while no receiver is connected"), bFrameAccepted);

	Sender.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

