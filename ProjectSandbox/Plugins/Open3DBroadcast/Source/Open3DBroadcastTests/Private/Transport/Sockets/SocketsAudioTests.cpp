#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

#include "../Sender/SocketsTcpSender.h"
#include "../Receiver/SocketsTcpReceiver.h"
#include "../Shared/SocketsTransportCommon.h"

#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "O3DTransportTypes.h"
#include "SerializedFrameConsumerRegistry.h"

namespace
{
	class FTestReceiverAudioSink final : public IO3DReceiverAudioSink
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

	int32 FindAvailableTcpPort()
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

	void PumpTransports(FO3DSocketsTcpSender& Sender, FO3DSocketsTcpReceiver& Receiver, double DurationSeconds, double SleepSeconds = 0.001)
	{
		const double Deadline = FPlatformTime::Seconds() + DurationSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Receiver.Poll();
			Sender.Tick(0.0f);
			if (SleepSeconds > 0.0)
			{
				FPlatformProcess::Sleep(SleepSeconds);
			}
		}
	}

	FO3DTransportConfig BuildSenderConfig(int32 DataPort, int32 AudioPort)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("sockets.tcp");
		Config.Role = TEXT("sender");
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), DataPort);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), DataPort);
		Config.AdvancedParams.Add(O3DSockets::BindOptionKey, TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(DataPort));
		Config.AdvancedParams.Add(O3DSockets::AudioBindOptionKey, TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));

		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 2;
		// Note: Audio stream label is now automatically derived from StreamId
		return Config;
	}

	FO3DTransportConfig BuildReceiverConfig(int32 DataPort, int32 AudioPort)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("sockets.tcp");
		Config.Role = TEXT("receiver");
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), DataPort);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), DataPort);
		Config.AdvancedParams.Add(O3DSockets::HostOptionKey, TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(DataPort));
		Config.AdvancedParams.Add(O3DSockets::AudioHostOptionKey, TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));

		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 2;
		// Note: Audio stream label is now automatically derived from StreamId
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsAudioRoundTripTest, "Open3DBroadcast.Open3DTransportSockets.Audio.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSocketsAudioRoundTripTest::RunTest(const FString& Parameters)
{
	const int32 DataPort = FindAvailableTcpPort();
	TestTrue(TEXT("Data port allocated"), DataPort > 0);

	int32 AudioPort = 0;
	for (int32 Attempt = 0; Attempt < 5 && (AudioPort == 0 || AudioPort == DataPort); ++Attempt)
	{
		AudioPort = FindAvailableTcpPort();
	}
	TestTrue(TEXT("Audio port allocated"), AudioPort > 0);
	TestNotEqual(TEXT("Distinct ports"), DataPort, AudioPort);

	FO3DTransportConfig SenderConfig = BuildSenderConfig(DataPort, AudioPort);
	FO3DTransportConfig ReceiverConfig = BuildReceiverConfig(DataPort, AudioPort);

	FO3DSocketsTcpSender Sender;
	FO3DSocketsTcpReceiver Receiver;

	TestTrue(TEXT("Sender initializes"), Sender.Initialize(SenderConfig));
	TestTrue(TEXT("Receiver initializes"), Receiver.Initialize(ReceiverConfig));

	TSharedPtr<FNullFrameConsumer> FrameConsumer = MakeShared<FNullFrameConsumer>();
	Receiver.SetConsumer(FrameConsumer);

	TSharedPtr<FTestReceiverAudioSink, ESPMode::ThreadSafe> ReceiverAudioSink = MakeShared<FTestReceiverAudioSink, ESPMode::ThreadSafe>();
	Receiver.SetAudioSink(ReceiverAudioSink, ReceiverConfig.Audio);

	TestTrue(TEXT("Sender starts"), Sender.Start());
	TestTrue(TEXT("Receiver starts"), Receiver.Start());

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
		PumpTransports(Sender, Receiver, 0.05);
		bSubmitted = SenderAudioSink->SubmitPcm(TEXT("audio_test"), Samples.GetData(), NumFrames, NumChannels, SenderConfig.Audio.SampleRate, 123.45);
	}
	TestTrue(TEXT("Audio frame submitted"), bSubmitted);

	const double ReceiveStart = FPlatformTime::Seconds();
	while ((FPlatformTime::Seconds() - ReceiveStart) < TimeoutSeconds && !ReceiverAudioSink->WasInvoked())
	{
		PumpTransports(Sender, Receiver, 0.05);
	}

	TestTrue(TEXT("Receiver sink invoked"), ReceiverAudioSink->WasInvoked());

	if (ReceiverAudioSink->WasInvoked())
	{
		const TArray<uint8>& Payload = ReceiverAudioSink->GetPayload();
		TestEqual(TEXT("Payload size"), Payload.Num(), NumFrames * NumChannels * static_cast<int32>(sizeof(int16)));

		const int16* PcmData = reinterpret_cast<const int16*>(Payload.GetData());
		const int32 ExpectedFirst = FMath::Clamp(FMath::RoundToInt(Samples[0] * 32767.0f), -32768, 32767);
		TestEqual(TEXT("PCM16 conversion"), PcmData[0], static_cast<int16>(ExpectedFirst));
		// Note: Stream label is now automatically derived from StreamId
		TestEqual(TEXT("Meta stream label matches StreamId"), ReceiverAudioSink->GetMeta().StreamLabel, SenderConfig.StreamId);
		TestEqual(TEXT("Meta channel count"), ReceiverAudioSink->GetMeta().NumChannels, NumChannels);
		TestEqual(TEXT("Meta sample rate"), ReceiverAudioSink->GetMeta().SampleRate, SenderConfig.Audio.SampleRate);
	}

	Receiver.Stop();
	Sender.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsAudioQueueOverflowTest, "Open3DBroadcast.Open3DTransportSockets.Audio.QueueOverflow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSocketsAudioQueueOverflowTest::RunTest(const FString& Parameters)
{
	const int32 DataPort = FindAvailableTcpPort();
	TestTrue(TEXT("Data port allocated"), DataPort > 0);

	int32 AudioPort = 0;
	for (int32 Attempt = 0; Attempt < 5 && (AudioPort == 0 || AudioPort == DataPort); ++Attempt)
	{
		AudioPort = FindAvailableTcpPort();
	}
	TestTrue(TEXT("Audio port allocated"), AudioPort > 0);
	TestNotEqual(TEXT("Ports differ"), DataPort, AudioPort);

	FO3DTransportConfig SenderConfig = BuildSenderConfig(DataPort, AudioPort);
	FO3DSocketsTcpSender Sender;

	TestTrue(TEXT("Sender initializes"), Sender.Initialize(SenderConfig));
	TestTrue(TEXT("Sender starts"), Sender.Start());

	TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> SenderAudioSink = Sender.CreateAudioSink(SenderConfig.Audio);
	TestTrue(TEXT("Audio sink created"), SenderAudioSink.IsValid());

	const float SampleValue = 0.25f;
	// Note: Stream label is now automatically derived from StreamId
	const bool bFrameAccepted = SenderAudioSink->SubmitPcm(SenderConfig.StreamId, &SampleValue, 1, 1, SenderConfig.Audio.SampleRate, 0.0);
	TestFalse(TEXT("Frame dropped without receiver connection"), bFrameAccepted);

	Sender.Stop();
	return true;
}

#endif // WITH_AUTOMATION_TESTS

