#if WITH_DEV_AUTOMATION_TESTS

#include "Sender/NngSender.h"
#include "Receiver/NngReceiver.h"
#include "Shared/NngHelpers.h"

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "O3DTransportTypes.h"
#include "SerializedFrameConsumerRegistry.h"

#include "o3ds/model.h"

#include <string>
#include <vector>

namespace
{
	class FTestFrameConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& InStreamId, const TArray<uint8>& InPayload, double InTimestamp) override
		{
			StreamId = InStreamId;
			Payload = InPayload;
			Timestamp = InTimestamp;
			bInvoked = true;
		}

		bool WasInvoked() const { return bInvoked; }
		const FString& GetStreamId() const { return StreamId; }
		const TArray<uint8>& GetPayload() const { return Payload; }
		double GetTimestamp() const { return Timestamp; }
		void Reset()
		{
			bInvoked = false;
			StreamId.Reset();
			Payload.Reset();
			Timestamp = 0.0;
		}

	private:
		bool bInvoked = false;
		FString StreamId;
		TArray<uint8> Payload;
		double Timestamp = 0.0;
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

		FSocket* TempSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("NNGTransportTestPortProbe"), false);
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

	void PumpTransports(FO3DNngSender& Sender, FO3DNngReceiver& Receiver, double DurationSeconds, double SleepSeconds = 0.001)
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

	FO3DTransportConfig BuildSenderConfig(int32 Port, uint64 QueueLimitBytes = 0)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("nng");
		Config.Role = TEXT("sender");
		Config.Uri = FString::Printf(TEXT("tcp://0.0.0.0:%d"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(O3DNNG::ModeOptionKey, TEXT("pub"));
		Config.AdvancedParams.Add(O3DNNG::HostOptionKey, TEXT("0.0.0.0"));
		Config.AdvancedParams.Add(O3DNNG::PortOptionKey, FString::FromInt(Port));
		Config.AdvancedParams.Add(O3DNNG::RoleOptionKey, TEXT("server"));
		if (QueueLimitBytes > 0)
		{
			Config.AdvancedParams.Add(O3DNNG::QueueOptionKey, FString::Printf(TEXT("%llu"), QueueLimitBytes));
		}
		return Config;
	}

	FO3DTransportConfig BuildReceiverConfig(int32 Port)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("nng");
		Config.Role = TEXT("receiver");
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(O3DNNG::ModeOptionKey, TEXT("sub"));
		Config.AdvancedParams.Add(O3DNNG::HostOptionKey, TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(O3DNNG::PortOptionKey, FString::FromInt(Port));
		Config.AdvancedParams.Add(O3DNNG::RoleOptionKey, TEXT("client"));
		return Config;
	}

	void PopulateSubjectList(O3DS::SubjectList& List, const TCHAR* SubjectLabel, int32 NumCurves)
	{
		FTCHARToUTF8 SubjectUtf8(SubjectLabel);
		O3DS::Subject* Subject = List.addSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));
		Subject->addTransform("Root", -1);

		for (int32 Index = 0; Index < NumCurves; ++Index)
		{
			const FString CurveName = FString::Printf(TEXT("Curve_%d"), Index);
			FTCHARToUTF8 CurveUtf8(*CurveName);
			Subject->mCurveNames.emplace_back(CurveUtf8.Get(), CurveUtf8.Length());
			Subject->mCurveValues.push_back(static_cast<float>(Index) / FMath::Max(1, NumCurves));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngDataRoundTripTest, "Open3DStream.TransportNNG.Data.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DNngDataRoundTripTest::RunTest(const FString& Parameters)
{
	const int32 Port = FindAvailableTcpPort();
	TestTrue(TEXT("Data port allocated"), Port > 0);
	if (Port <= 0)
	{
		return false;
	}

	FO3DTransportConfig SenderConfig = BuildSenderConfig(Port);
	FO3DTransportConfig ReceiverConfig = BuildReceiverConfig(Port);

	FO3DNngSender Sender;
	FO3DNngReceiver Receiver;

	const bool bSenderInitialized = Sender.Initialize(SenderConfig);
	TestTrue(TEXT("Sender initializes"), bSenderInitialized);
	if (!bSenderInitialized)
	{
		return false;
	}

	const bool bReceiverInitialized = Receiver.Initialize(ReceiverConfig);
	TestTrue(TEXT("Receiver initializes"), bReceiverInitialized);
	if (!bReceiverInitialized)
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Sender.Stop();
	};
	ON_SCOPE_EXIT
	{
		Receiver.Stop();
	};

	TSharedPtr<FTestFrameConsumer, ESPMode::ThreadSafe> FrameConsumer = MakeShared<FTestFrameConsumer, ESPMode::ThreadSafe>();
	Receiver.SetConsumer(FrameConsumer);

	const bool bReceiverStarted = Receiver.Start();
	TestTrue(TEXT("Receiver starts"), bReceiverStarted);
	if (!bReceiverStarted)
	{
		return false;
	}

	const bool bSenderStarted = Sender.Start();
	TestTrue(TEXT("Sender starts"), bSenderStarted);
	if (!bSenderStarted)
	{
		return false;
	}

	PumpTransports(Sender, Receiver, 0.2);

	static constexpr const TCHAR* SubjectLabel = TEXT("NNGSubject");
	O3DS::SubjectList SubjectList;
	PopulateSubjectList(SubjectList, SubjectLabel, 8);

	const bool bSendQueued = Sender.Send(SubjectList);
	TestTrue(TEXT("Sender queued frame"), bSendQueued);

	const double TimeoutSeconds = 5.0;
	const double StartTime = FPlatformTime::Seconds();
	while (!FrameConsumer->WasInvoked() && (FPlatformTime::Seconds() - StartTime) < TimeoutSeconds)
	{
		PumpTransports(Sender, Receiver, 0.05);
	}

	TestTrue(TEXT("Receiver consumed frame"), FrameConsumer->WasInvoked());

	if (FrameConsumer->WasInvoked())
	{
		const TArray<uint8>& Payload = FrameConsumer->GetPayload();
		TestTrue(TEXT("Payload non-empty"), Payload.Num() > 0);

		O3DS::SubjectList Parsed;
		const bool bParse = Parsed.Parse(reinterpret_cast<const char*>(Payload.GetData()), Payload.Num());
		TestTrue(TEXT("Payload parses"), bParse);

		if (bParse)
		{
			FTCHARToUTF8 SubjectUtf8(SubjectLabel);
			O3DS::Subject* ParsedSubject = Parsed.findSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));
			TestNotNull(TEXT("Subject round-tripped"), ParsedSubject);
			if (ParsedSubject)
			{
				TestEqual(TEXT("Curve count preserved"), static_cast<int32>(ParsedSubject->mCurveNames.size()), 8);
			}
		}
	}

	const FO3DTransportStats SenderStats = Sender.GetStats();
	TestTrue(TEXT("Sender recorded frames"), SenderStats.FramesSent > 0);
	TestTrue(TEXT("Sender recorded bytes"), SenderStats.BytesSent > 0);

	const FO3DTransportStats ReceiverStats = Receiver.GetStats();
	TestTrue(TEXT("Receiver recorded frames"), ReceiverStats.FramesReceived > 0);
	TestTrue(TEXT("Receiver recorded bytes"), ReceiverStats.BytesReceived > 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngQueueLimitTest, "Open3DStream.TransportNNG.Queue.Limit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DNngQueueLimitTest::RunTest(const FString& Parameters)
{
	const int32 Port = FindAvailableTcpPort();
	TestTrue(TEXT("Data port allocated"), Port > 0);
	if (Port <= 0)
	{
		return false;
	}

	const uint64 QueueLimit = 64ull * 1024ull; // Minimum enforced queue size inside the sender.
	FO3DTransportConfig SenderConfig = BuildSenderConfig(Port, QueueLimit);

	FO3DNngSender Sender;
	const bool bSenderInitialized = Sender.Initialize(SenderConfig);
	TestTrue(TEXT("Sender initializes"), bSenderInitialized);
	if (!bSenderInitialized)
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Sender.Stop();
	};

	const bool bSenderStarted = Sender.Start();
	TestTrue(TEXT("Sender starts"), bSenderStarted);
	if (!bSenderStarted)
	{
		return false;
	}

	FPlatformProcess::Sleep(0.05); // allow worker thread to spin up

	static constexpr int32 NumCurves = 8000;
	O3DS::SubjectList PreviewList;
	PopulateSubjectList(PreviewList, TEXT("PreviewSubject"), NumCurves);

	std::vector<char> PreviewBuffer;
	const int32 PreviewBytes = PreviewList.Serialize(PreviewBuffer, 0.0);
	TestTrue(TEXT("Preview payload larger than queue"), PreviewBytes > static_cast<int32>(QueueLimit));

	O3DS::SubjectList LargeList;
	PopulateSubjectList(LargeList, TEXT("LargeSubject"), NumCurves);

	const bool bSendQueued = Sender.Send(LargeList);
	TestFalse(TEXT("Large payload rejected due to queue limit"), bSendQueued);

	const FO3DTransportStats SenderStats = Sender.GetStats();
	TestEqual(TEXT("Dropped frame recorded"), static_cast<int64>(SenderStats.DroppedFrames), static_cast<int64>(1));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
