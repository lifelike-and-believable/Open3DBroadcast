#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Containers/StringConv.h"
#include "Containers/Ticker.h"

#include "O3DTransportTypes.h"
#include "Sender/MoQSender.h"
#include "Shared/MoQFfiSupport.h"
#include "Shared/MoQHandles.h"
#include "Shared/MoQSessionWrapper.h"
#include "Shared/MoQTypes.h"

#include "o3ds/model.h"

#include <string>
#include <vector>

#if O3D_WITH_TRANSPORT_MOQ

namespace MoQTestHelpers
{
static constexpr const TCHAR* kDefaultRelayUrl = TEXT("https://relay.cloudflare.mediaoverquic.com");
static constexpr double kConnectionTimeout = 15.0;
static constexpr double kOperationTimeout = 10.0;
static constexpr double kSenderTimeout = 20.0;
static constexpr double kTrackPropagationDelay = 0.5; // matches moq-ffi Cloudflare integration expectations

class FMoQSubscribeRetryController;

FString ResolveRelayUrl()
{
	const FString EnvOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("O3D_MOQ_RELAY_URL"));
	if (!EnvOverride.IsEmpty())
	{
		return EnvOverride;
	}
	return kDefaultRelayUrl;
}

FString MakeUniqueNamespace(const FString& Prefix)
{
	const FString GuidString = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	return Prefix.IsEmpty() ? GuidString : FString::Printf(TEXT("%s/%s"), *Prefix, *GuidString);
}

void PopulateSubject(O3DS::SubjectList& List, const FString& SubjectName)
{
	const FTCHARToUTF8 SubjectUtf8(*SubjectName);
	O3DS::Subject* Subject = List.addSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));
	Subject->addTransform("Root", -1);

	static constexpr int32 CurveCount = 3;
	const TCHAR* CurveNames[CurveCount] = { TEXT("Jaw_Open"), TEXT("EyeBlink_L"), TEXT("EyeBlink_R") };
	for (int32 Index = 0; Index < CurveCount; ++Index)
	{
		const FTCHARToUTF8 CurveUtf8(CurveNames[Index]);
		Subject->mCurveNames.emplace_back(CurveUtf8.Get(), CurveUtf8.Length());
		Subject->mCurveValues.push_back(0.1f * static_cast<float>(Index + 1));
	}
}

TArray<uint8> BuildSerializedPayload(const FString& SubjectName)
{
	O3DS::SubjectList Subjects;
	PopulateSubject(Subjects, SubjectName);

	std::vector<char> Buffer;
	Subjects.Serialize(Buffer, FPlatformTime::Seconds());

	TArray<uint8> Payload;
	if (!Buffer.empty())
	{
		Payload.Append(reinterpret_cast<const uint8*>(Buffer.data()), static_cast<int32>(Buffer.size()));
	}
	return Payload;
}

struct FMoQSessionTestHarness : public TSharedFromThis<FMoQSessionTestHarness, ESPMode::ThreadSafe>
{
	FMoQSessionTestHarness(FString InRelayUrl, FString InLabel)
		: RelayUrl(MoveTemp(InRelayUrl))
		, Label(MoveTemp(InLabel))
	{
	}

	~FMoQSessionTestHarness()
	{
		Disconnect();
	}

	bool Initialize(FAutomationTestBase& Test)
	{
		Session = MakeShared<FMoQSessionWrapper>();
		const FMoQResult Result = Session->Initialize(RelayUrl);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: session initializes"), *Label), Result.IsOk()))
		{
			return false;
		}
		InstallDelegate();
		return true;
	}

	bool Connect(FAutomationTestBase& Test)
	{
		const FMoQResult Result = Session->Connect();
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: connect call succeeds"), *Label), Result.IsOk()))
		{
			return false;
		}
		return true;
	}

	void Disconnect()
	{
		if (!Session.IsValid())
		{
			return;
		}

		if (ConnectionHandle.IsValid())
		{
			Session->OnConnectionStateChanged().Remove(ConnectionHandle);
			ConnectionHandle.Reset();
		}

		Session->Disconnect();
		Session.Reset();
		LastState = MOQ_STATE_DISCONNECTED;
		bConnected = false;
	}

	void InstallDelegate()
	{
		TWeakPtr<FMoQSessionTestHarness, ESPMode::ThreadSafe> WeakHarness = AsShared();
		ConnectionHandle = Session->OnConnectionStateChanged().AddLambda([WeakHarness](MoqConnectionState State)
		{
			if (const TSharedPtr<FMoQSessionTestHarness, ESPMode::ThreadSafe> Strong = WeakHarness.Pin())
			{
				Strong->LastState = State;
				if (State == MOQ_STATE_CONNECTED)
				{
					Strong->bConnected = true;
				}
			}
		});
	}

	const FString& GetLabel() const { return Label; }

	FString RelayUrl;
	FString Label;
	TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Session;
	FDelegateHandle ConnectionHandle;
	TAtomic<MoqConnectionState> LastState{MOQ_STATE_DISCONNECTED};
	TAtomic<bool> bConnected{false};
};

struct FMoQPubSubState : public TSharedFromThis<FMoQPubSubState, ESPMode::ThreadSafe>
{
	TSharedPtr<FMoQPublisherHandle> Publisher;
	TSharedPtr<FMoQSubscriberHandle> Subscriber;
	TAtomic<bool> bPayloadReceived{false};
	TAtomic<bool> bTrackPrimed{false};
	TAtomic<bool> bSubscriberReady{false};
	TAtomic<bool> bSubscribeFailed{false};
	FString SubscribeFailureMessage;
	TSharedPtr<FMoQSubscribeRetryController, ESPMode::ThreadSafe> SubscribeController;
	FString Namespace;
	FString TrackName;
};

struct FMoQSenderHarness : public TSharedFromThis<FMoQSenderHarness, ESPMode::ThreadSafe>
{
	~FMoQSenderHarness()
	{
		Shutdown();
	}

	bool Initialize(const FString& RelayUrl, const FString& Namespace, const FString& TrackName, FAutomationTestBase& Test)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.Uri = RelayUrl;
		Config.StreamId = FString::Printf(TEXT("%s/%s"), *Namespace, *TrackName);
		Config.AdvancedParams.Add(TEXT("track_namespace"), Namespace);
		Config.AdvancedParams.Add(TEXT("track_name"), TrackName);
		Config.AdvancedParams.Add(TEXT("relay_url"), RelayUrl);

		if (!Sender.Initialize(Config))
		{
			Test.AddError(TEXT("Sender initialization failed"));
			return false;
		}

		if (!Sender.Start())
		{
			Test.AddError(TEXT("Sender start failed"));
			Sender.Stop();
			return false;
		}

		bStarted = true;
		return true;
	}

	void Shutdown()
	{
		if (bStarted)
		{
			Sender.Stop();
			bStarted = false;
		}
	}

	FO3DMoQSender Sender;
	bool bStarted = false;
};

class FMoQSubscribeRetryController : public TSharedFromThis<FMoQSubscribeRetryController, ESPMode::ThreadSafe>
{
public:
	FMoQSubscribeRetryController(
		TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> InHarness,
		FString InNamespace,
		FString InTrackName,
		TSharedRef<FMoQPubSubState, ESPMode::ThreadSafe> InState,
		int32 InMaxAttempts,
		double InRetryDelaySeconds,
		FAutomationTestBase* InTest)
		: Harness(InHarness)
		, NamespaceValue(MoveTemp(InNamespace))
		, TrackValue(MoveTemp(InTrackName))
		, PubSubState(InState)
		, MaxAttempts(InMaxAttempts)
		, RetryDelaySeconds(InRetryDelaySeconds)
		, Test(InTest)
	{
	}

	~FMoQSubscribeRetryController()
	{
		Stop();
	}

	void Start()
	{
		if (bRunning)
		{
			return;
		}

		bRunning = true;
		Attempt = 0;
		PubSubState->bSubscriberReady.Store(false);
		PubSubState->bSubscribeFailed.Store(false);
		PubSubState->SubscribeFailureMessage.Reset();
		AttemptSubscribe();
	}

	void Stop()
	{
		bRunning = false;
		if (RetryHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(RetryHandle);
			RetryHandle = FTSTicker::FDelegateHandle();
		}
	}

private:
	void AttemptSubscribe()
	{
		if (!bRunning)
		{
			return;
		}

		++Attempt;
		PubSubState->Subscriber.Reset();
		const TSharedPtr<FMoQSessionTestHarness, ESPMode::ThreadSafe> HarnessPtr = Harness.Pin();
		if (!HarnessPtr.IsValid())
		{
			OnAttemptComplete(FMoQResult::FromCode(EMoQErrorCode::Internal, TEXT("Session harness destroyed before subscribe")), nullptr);
			return;
		}

		FMoQSubscriptionConfig Config;
		Config.Namespace = NamespaceValue;
		Config.TrackName = TrackValue;
		Config.OnData = [State = PubSubState](const TArray64<uint8>& Payload)
		{
			if (!Payload.IsEmpty())
			{
				State->bPayloadReceived.Store(true);
			}
		};

		TWeakPtr<FMoQSubscribeRetryController, ESPMode::ThreadSafe> ControllerWeak = AsShared();
		const FMoQResult ScheduleResult = HarnessPtr->Session->SubscribeAsync(Config,
			[ControllerWeak](FMoQResult Result, TSharedPtr<FMoQSubscriberHandle> Subscriber)
			{
				if (const TSharedPtr<FMoQSubscribeRetryController, ESPMode::ThreadSafe> Strong = ControllerWeak.Pin())
				{
					Strong->OnAttemptComplete(Result, Subscriber);
				}
			});

		if (!ScheduleResult.IsOk())
		{
			OnAttemptComplete(ScheduleResult, nullptr);
		}
	}

	void OnAttemptComplete(FMoQResult Result, TSharedPtr<FMoQSubscriberHandle> Subscriber)
	{
		if (!bRunning)
		{
			return;
		}

		if (Result.IsOk() && Subscriber.IsValid())
		{
			PubSubState->Subscriber = Subscriber;
			PubSubState->bSubscriberReady.Store(true);
			PubSubState->bSubscribeFailed.Store(false);
			if (Test)
			{
				Test->AddInfo(FString::Printf(TEXT("Subscriber creation succeeded after %d attempt(s)"), Attempt));
			}
			Stop();
			return;
		}

		const FString Message = Result.Message.IsEmpty() ? TEXT("Unknown subscribe failure") : Result.Message;
		if (Test)
		{
			Test->AddWarning(FString::Printf(TEXT("Subscriber attempt %d/%d failed: %s"), Attempt, MaxAttempts, *Message));
		}

		if (Attempt >= MaxAttempts)
		{
			PubSubState->bSubscribeFailed.Store(true);
			PubSubState->SubscribeFailureMessage = Message;
			Stop();
			return;
		}

		ScheduleRetry();
	}

	void ScheduleRetry()
	{
		if (!bRunning)
		{
			return;
		}

		if (RetryDelaySeconds <= 0.0)
		{
			AttemptSubscribe();
			return;
		}

		if (RetryHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(RetryHandle);
			RetryHandle = FTSTicker::FDelegateHandle();
		}

		RetryRemainingSeconds = RetryDelaySeconds;
		TWeakPtr<FMoQSubscribeRetryController, ESPMode::ThreadSafe> ControllerWeak = AsShared();
		RetryHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([ControllerWeak](float DeltaTime)
		{
			if (const TSharedPtr<FMoQSubscribeRetryController, ESPMode::ThreadSafe> Strong = ControllerWeak.Pin())
			{
				Strong->RetryRemainingSeconds -= DeltaTime;
				if (Strong->RetryRemainingSeconds <= 0.0)
				{
					Strong->AttemptSubscribe();
					Strong->RetryHandle = FTSTicker::FDelegateHandle();
					return false;
				}
				return true;
			}
			return false;
		}));
	}

	TWeakPtr<FMoQSessionTestHarness, ESPMode::ThreadSafe> Harness;
	FString NamespaceValue;
	FString TrackValue;
	TSharedRef<FMoQPubSubState, ESPMode::ThreadSafe> PubSubState;
	int32 MaxAttempts = 1;
	double RetryDelaySeconds = 0.0;
	FAutomationTestBase* Test = nullptr;
	int32 Attempt = 0;
	bool bRunning = false;
	double RetryRemainingSeconds = 0.0;
	FTSTicker::FDelegateHandle RetryHandle;
};
} // namespace MoQTestHelpers

using namespace MoQTestHelpers;

class FMoQSessionTeardownCommand : public IAutomationLatentCommand
{
public:
	explicit FMoQSessionTeardownCommand(TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> InHarness)
		: Harness(MoveTemp(InHarness))
	{
	}

	virtual bool Update() override
	{
		Harness->Disconnect();
		return true;
	}

private:
	TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> Harness;
};

class FMoQCallLambdaLatentCommand : public IAutomationLatentCommand
{
public:
	explicit FMoQCallLambdaLatentCommand(TFunction<void()> InAction)
		: Action(MoveTemp(InAction))
	{
	}

	virtual bool Update() override
	{
		if (Action)
		{
			Action();
		}
		return true;
	}

private:
	TFunction<void()> Action;
};

class FWaitForMoQStateCommand : public IAutomationLatentCommand
{
public:
	FWaitForMoQStateCommand(TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> InHarness, MoqConnectionState InDesiredState, double InTimeoutSeconds, FAutomationTestBase* InTest, FString InStepLabel)
		: Harness(MoveTemp(InHarness))
		, DesiredState(InDesiredState)
		, TimeoutSeconds(InTimeoutSeconds)
		, Test(InTest)
		, StepLabel(MoveTemp(InStepLabel))
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		const MoqConnectionState Current = Harness->LastState.Load();
		if (Current == DesiredState)
		{
			return true;
		}

		if (Current == MOQ_STATE_FAILED)
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s: session entered FAILED state while waiting for %s"), *StepLabel, *LexToString(DesiredState)));
			}
			return true;
		}

		if ((FPlatformTime::Seconds() - StartTime) >= TimeoutSeconds)
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s: timed out waiting for %s (last=%s)"), *StepLabel, *LexToString(DesiredState), *LexToString(Current)));
			}
			return true;
		}

		return false;
	}

private:
	TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> Harness;
	MoqConnectionState DesiredState;
	double TimeoutSeconds;
	FAutomationTestBase* Test = nullptr;
	FString StepLabel;
	const double StartTime;
};

class FWaitForPubSubPayloadCommand : public IAutomationLatentCommand
{
public:
	FWaitForPubSubPayloadCommand(TSharedRef<FMoQPubSubState, ESPMode::ThreadSafe> InState, double InTimeoutSeconds, FAutomationTestBase* InTest, FString InStepLabel)
		: PubSubState(MoveTemp(InState))
		, TimeoutSeconds(InTimeoutSeconds)
		, Test(InTest)
		, StepLabel(MoveTemp(InStepLabel))
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		if (PubSubState->bPayloadReceived.Load())
		{
			return true;
		}

		if (PubSubState->bSubscribeFailed.Load())
		{
			if (Test)
			{
				const FString FailureMessage = PubSubState->SubscribeFailureMessage.IsEmpty() ? TEXT("Unknown subscribe failure") : PubSubState->SubscribeFailureMessage;
				Test->AddError(FString::Printf(TEXT("%s: subscriber failed to initialize: %s"), *StepLabel, *FailureMessage));
			}
			return true;
		}

		if ((FPlatformTime::Seconds() - StartTime) >= TimeoutSeconds)
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s: timed out waiting for subscriber payload"), *StepLabel));
			}
			return true;
		}

		return false;
	}

private:
	TSharedRef<FMoQPubSubState, ESPMode::ThreadSafe> PubSubState;
	double TimeoutSeconds;
	FAutomationTestBase* Test = nullptr;
	FString StepLabel;
	const double StartTime;
};

class FMoQDelayLatentCommand : public IAutomationLatentCommand
{
public:
	explicit FMoQDelayLatentCommand(double InSeconds)
		: DurationSeconds(InSeconds)
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		return (FPlatformTime::Seconds() - StartTime) >= DurationSeconds;
	}

private:
	double DurationSeconds = 0.0;
	const double StartTime;
};

class FMoQSubscribeWithRetryCommand : public IAutomationLatentCommand
{
public:
	FMoQSubscribeWithRetryCommand(
		TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> InHarness,
		FString InNamespace,
		FString InTrackName,
		TSharedRef<FMoQPubSubState, ESPMode::ThreadSafe> InState,
		int32 InMaxAttempts,
		double InRetryDelaySeconds,
		FAutomationTestBase* InTest)
		: Harness(MoveTemp(InHarness))
		, NamespaceValue(MoveTemp(InNamespace))
		, TrackValue(MoveTemp(InTrackName))
		, PubSubState(MoveTemp(InState))
		, MaxAttempts(InMaxAttempts)
		, RetryDelaySeconds(InRetryDelaySeconds)
		, Test(InTest)
	{
	}

	virtual bool Update() override
	{
		if (bStarted)
		{
			return true;
		}

		bStarted = true;
		PubSubState->bPayloadReceived.Store(false);
		PubSubState->bSubscriberReady.Store(false);
		PubSubState->bSubscribeFailed.Store(false);
		PubSubState->SubscribeFailureMessage.Reset();

		PubSubState->SubscribeController = MakeShared<FMoQSubscribeRetryController, ESPMode::ThreadSafe>(
			Harness,
			NamespaceValue,
			TrackValue,
			PubSubState,
			MaxAttempts,
			RetryDelaySeconds,
			Test);
		PubSubState->SubscribeController->Start();
		return true;
	}

private:
	TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> Harness;
	FString NamespaceValue;
	FString TrackValue;
	TSharedRef<FMoQPubSubState, ESPMode::ThreadSafe> PubSubState;
	int32 MaxAttempts = 1;
	double RetryDelaySeconds = 0.0;
	FAutomationTestBase* Test = nullptr;
	bool bStarted = false;
};

class FWaitForSenderFramesCommand : public IAutomationLatentCommand
{
public:
	FWaitForSenderFramesCommand(TSharedRef<FMoQSenderHarness, ESPMode::ThreadSafe> InHarness, int32 InMinimumFrames, double InTimeoutSeconds, FAutomationTestBase* InTest, FString InStepLabel)
		: SenderHarness(MoveTemp(InHarness))
		, MinimumFrames(InMinimumFrames)
		, TimeoutSeconds(InTimeoutSeconds)
		, Test(InTest)
		, StepLabel(MoveTemp(InStepLabel))
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		SenderHarness->Sender.Tick(0.0f);
		const FO3DTransportStats Stats = SenderHarness->Sender.GetStats();
		if (Stats.FramesSent >= MinimumFrames)
		{
			return true;
		}

		if ((FPlatformTime::Seconds() - StartTime) >= TimeoutSeconds)
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s: expected %d frames but only %lld sent (dropped=%lld)"), *StepLabel, MinimumFrames, Stats.FramesSent, Stats.DroppedFrames));
			}
			return true;
		}

		return false;
	}

private:
	TSharedRef<FMoQSenderHarness, ESPMode::ThreadSafe> SenderHarness;
	int32 MinimumFrames;
	double TimeoutSeconds;
	FAutomationTestBase* Test = nullptr;
	FString StepLabel;
	const double StartTime;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoQCloudflareRelayBasicTest,
	"Open3DBroadcast.Open3DTransportMoQ.Cloudflare.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMoQCloudflareRelayBasicTest::RunTest(const FString& Parameters)
{
	const FString RelayUrl = ResolveRelayUrl();
	AddInfo(TEXT("Validating basic MoQ session bootstrap using moq_ffi.dll"));
	AddInfo(FString::Printf(TEXT("Relay target: %s"), *RelayUrl));

	if (!TestTrue(TEXT("MoQ FFI should already be loaded"), FMoQFfiSupport::IsLoaded()))
	{
		AddError(FString::Printf(TEXT("moq_ffi.dll unavailable: %s"), *FMoQFfiSupport::GetStatusMessage()));
		return false;
	}

	TSharedPtr<FMoQSessionWrapper> Session = MakeShared<FMoQSessionWrapper>();
	TestTrue(TEXT("Session wrapper instantiates"), Session.IsValid());

	const FMoQResult InitResult = Session->Initialize(RelayUrl);
	if (!TestTrue(TEXT("Initialize returns success"), InitResult.IsOk()))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitResult.Message));
		return false;
	}

	const FMoQResult ConnectResult = Session->Connect();
	if (!TestTrue(TEXT("Connect() should enqueue async connection without crashing"), ConnectResult.IsOk()))
	{
		AddError(FString::Printf(TEXT("Connect failed immediately: %s"), *ConnectResult.Message));
		Session->Disconnect();
		return false;
	}

	Session->Disconnect();
	AddInfo(TEXT("Session disconnected cleanly (no panic paths triggered)."));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoQCloudflareRelayConnectivityTest,
	"Open3DBroadcast.Open3DTransportMoQ.Cloudflare.Connectivity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMoQCloudflareRelayConnectivityTest::RunTest(const FString& Parameters)
{
	if (!FMoQFfiSupport::IsLoaded())
	{
		AddError(TEXT("moq_ffi.dll must be loaded before running relay connectivity tests."));
		return false;
	}

	const FString RelayUrl = ResolveRelayUrl();
	AddInfo(FString::Printf(TEXT("Connecting to Cloudflare MoQ relay at %s"), *RelayUrl));
	AddInfo(TEXT("Verifying outbound internet access via Cloudflare relay."));

	TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> Harness = MakeShared<FMoQSessionTestHarness, ESPMode::ThreadSafe>(RelayUrl, TEXT("Connectivity"));
	if (!Harness->Initialize(*this))
	{
		return false;
	}

	if (!Harness->Connect(*this))
	{
		Harness->Disconnect();
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForMoQStateCommand(Harness, MOQ_STATE_CONNECTED, kConnectionTimeout, this, TEXT("Cloudflare connectivity")));
	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([this, Harness]()
	{
		if (Harness->bConnected.Load())
		{
			AddInfo(TEXT("Outbound internet access confirmed (MoQ relay handshake succeeded)."));
		}
		else
		{
			AddWarning(TEXT("MoQ relay handshake did not complete. Ensure outbound internet access is available."));
		}
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FMoQSessionTeardownCommand(Harness));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoQCloudflareRelayPublishTest,
	"Open3DBroadcast.Open3DTransportMoQ.Cloudflare.Publish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMoQCloudflareRelayPublishTest::RunTest(const FString& Parameters)
{
	if (!FMoQFfiSupport::IsLoaded())
	{
		AddError(TEXT("moq_ffi.dll must be loaded for publish tests."));
		return false;
	}

	const FString RelayUrl = ResolveRelayUrl();
	const FString Namespace = MakeUniqueNamespace(TEXT("o3ds/automation/publish"));
	const FString TrackName = TEXT("character");

	TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> Harness = MakeShared<FMoQSessionTestHarness, ESPMode::ThreadSafe>(RelayUrl, TEXT("Publish"));
	if (!Harness->Initialize(*this))
	{
		return false;
	}

	if (!Harness->Connect(*this))
	{
		Harness->Disconnect();
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForMoQStateCommand(Harness, MOQ_STATE_CONNECTED, kConnectionTimeout, this, TEXT("Publish.Connect")));
	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([this, Harness, Namespace, TrackName]()
	{
		const FMoQResult AnnounceResult = Harness->Session->AnnounceNamespace(Namespace);
		TestTrue(TEXT("Namespace announcement succeeds"), AnnounceResult.IsOk());

		FMoQPublisherConfig Config;
		Config.Namespace = Namespace;
		Config.TrackName = TrackName;
		Config.DeliveryMode = MOQ_DELIVERY_STREAM;

		TSharedPtr<FMoQPublisherHandle> Publisher;
		const FMoQResult CreateResult = Harness->Session->CreatePublisher(Config, Publisher);
		TestTrue(TEXT("Publisher creation succeeds"), CreateResult.IsOk());

		if (Publisher.IsValid())
		{
			const TArray<uint8> Payload = BuildSerializedPayload(TEXT("PublishPayload"));
			TestTrue(TEXT("Payload should not be empty"), Payload.Num() > 0);
			if (!Payload.IsEmpty())
			{
				const MoqResult PublishResult = moq_publish_data(Publisher->Get(), Payload.GetData(), Payload.Num(), Config.DeliveryMode);
				TestTrue(TEXT("moq_publish_data returns OK"), PublishResult.code == MOQ_OK);
			}
		}
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FMoQSessionTeardownCommand(Harness));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoQCloudflareRelayPublishSubscribeTest,
	"Open3DBroadcast.Open3DTransportMoQ.Cloudflare.PublishSubscribe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMoQCloudflareRelayPublishSubscribeTest::RunTest(const FString& Parameters)
{
	if (!FMoQFfiSupport::IsLoaded())
	{
		AddError(TEXT("moq_ffi.dll must be loaded for publish/subscribe tests."));
		return false;
	}

	const FString RelayUrl = ResolveRelayUrl();
	const FString Namespace = MakeUniqueNamespace(TEXT("o3ds/automation/pubsub"));
	const FString TrackName = TEXT("character");

	TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> PublisherHarness = MakeShared<FMoQSessionTestHarness, ESPMode::ThreadSafe>(RelayUrl, TEXT("PubSub-Publisher"));
	TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> SubscriberHarness = MakeShared<FMoQSessionTestHarness, ESPMode::ThreadSafe>(RelayUrl, TEXT("PubSub-Subscriber"));

	if (!PublisherHarness->Initialize(*this) || !SubscriberHarness->Initialize(*this))
	{
		return false;
	}

	if (!PublisherHarness->Connect(*this) || !SubscriberHarness->Connect(*this))
	{
		PublisherHarness->Disconnect();
		SubscriberHarness->Disconnect();
		return false;
	}

	TSharedRef<FMoQPubSubState, ESPMode::ThreadSafe> PubSubState = MakeShared<FMoQPubSubState, ESPMode::ThreadSafe>();

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForMoQStateCommand(PublisherHarness, MOQ_STATE_CONNECTED, kConnectionTimeout, this, TEXT("PubSub.PublisherConnect")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForMoQStateCommand(SubscriberHarness, MOQ_STATE_CONNECTED, kConnectionTimeout, this, TEXT("PubSub.SubscriberConnect")));

	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([this, PublisherHarness, Namespace, TrackName, PubSubState]()
	{
		PubSubState->Namespace = Namespace;
		PubSubState->TrackName = TrackName;

		const FMoQResult AnnounceResult = PublisherHarness->Session->AnnounceNamespace(Namespace);
		TestTrue(TEXT("Publisher namespace announcement succeeds"), AnnounceResult.IsOk());

		FMoQPublisherConfig Config;
		Config.Namespace = Namespace;
		Config.TrackName = TrackName;
		Config.DeliveryMode = MOQ_DELIVERY_STREAM;

		const FMoQResult CreateResult = PublisherHarness->Session->CreatePublisher(Config, PubSubState->Publisher);
		TestTrue(TEXT("Publisher for pub/sub created"), CreateResult.IsOk());

		if (!PubSubState->Publisher.IsValid())
		{
			AddError(TEXT("Publisher handle invalid before subscribe/publish"));
			return;
		}

		const TArray<uint8> PrimePayload = BuildSerializedPayload(TEXT("PubSubPrime"));
		TestTrue(TEXT("Prime payload should not be empty"), PrimePayload.Num() > 0);
		if (!PrimePayload.IsEmpty())
		{
			const MoqResult PrimeResult = moq_publish_data(PubSubState->Publisher->Get(), PrimePayload.GetData(), PrimePayload.Num(), Config.DeliveryMode);
			const bool bPrimeOk = (PrimeResult.code == MOQ_OK);
			TestTrue(TEXT("Initial publish to prime track succeeds"), bPrimeOk);
			if (bPrimeOk)
			{
				PubSubState->bTrackPrimed.Store(true);
			}
		}
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FMoQDelayLatentCommand(kTrackPropagationDelay));
	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([this, PubSubState]()
	{
		TestTrue(TEXT("Track should be primed before subscribing"), PubSubState->bTrackPrimed.Load());
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FMoQSubscribeWithRetryCommand(SubscriberHarness, Namespace, TrackName, PubSubState, 3, 0.5, this));

	ADD_LATENT_AUTOMATION_COMMAND(FMoQDelayLatentCommand(0.25));

	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([this, PubSubState]()
	{
		if (!PubSubState->Publisher.IsValid())
		{
			AddError(TEXT("Publisher handle invalid before publish"));
			return;
		}

		FMoQPublisherConfig Config;
		Config.Namespace = PubSubState->Namespace;
		Config.TrackName = PubSubState->TrackName;
		Config.DeliveryMode = MOQ_DELIVERY_STREAM;

		const TArray<uint8> Payload = BuildSerializedPayload(TEXT("PubSubPayload"));
		TestTrue(TEXT("Pub/Sub payload should not be empty"), Payload.Num() > 0);
		if (!Payload.IsEmpty())
		{
			const MoqResult PublishResult = moq_publish_data(PubSubState->Publisher->Get(), Payload.GetData(), Payload.Num(), Config.DeliveryMode);
			TestTrue(TEXT("Publish call succeeds"), PublishResult.code == MOQ_OK);
		}
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForPubSubPayloadCommand(PubSubState, kOperationTimeout, this, TEXT("PubSub.Payload")));
	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([PubSubState]()
	{
		if (PubSubState->SubscribeController.IsValid())
		{
			PubSubState->SubscribeController->Stop();
			PubSubState->SubscribeController.Reset();
		}
		PubSubState->Subscriber.Reset();
		PubSubState->Publisher.Reset();
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FMoQSessionTeardownCommand(PublisherHarness));
	ADD_LATENT_AUTOMATION_COMMAND(FMoQSessionTeardownCommand(SubscriberHarness));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoQCloudflareRelaySenderTest,
	"Open3DBroadcast.Open3DTransportMoQ.Cloudflare.Sender",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMoQCloudflareRelaySenderTest::RunTest(const FString& Parameters)
{
	if (!FMoQFfiSupport::IsLoaded())
	{
		AddError(TEXT("moq_ffi.dll must be loaded for sender tests."));
		return false;
	}

	const FString RelayUrl = ResolveRelayUrl();
	const FString Namespace = MakeUniqueNamespace(TEXT("o3ds/automation/sender"));
	const FString TrackName = TEXT("rig");

	TSharedRef<FMoQSenderHarness, ESPMode::ThreadSafe> SenderHarness = MakeShared<FMoQSenderHarness, ESPMode::ThreadSafe>();
	if (!SenderHarness->Initialize(RelayUrl, Namespace, TrackName, *this))
	{
		SenderHarness->Shutdown();
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([this, SenderHarness]()
	{
		O3DS::SubjectList Subjects;
		PopulateSubject(Subjects, TEXT("SenderSubject"));
		O3DS::Subject* Subject = Subjects.size() > 0 ? Subjects[0] : nullptr;

		for (int32 Index = 0; Index < 5; ++Index)
		{
			if (Subject && !Subject->mCurveValues.empty())
			{
				Subject->mCurveValues[0] = 0.1f * static_cast<float>(Index);
			}
			TestTrue(TEXT("Sender should accept payload"), SenderHarness->Sender.Send(Subjects));
		}
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForSenderFramesCommand(SenderHarness, 2, kSenderTimeout, this, TEXT("Sender.Frames")));
	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([SenderHarness]()
	{
		SenderHarness->Shutdown();
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoQCloudflareRelayMultiPublisherTest,
	"Open3DBroadcast.Open3DTransportMoQ.Cloudflare.MultiPublisher",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMoQCloudflareRelayMultiPublisherTest::RunTest(const FString& Parameters)
{
	if (!FMoQFfiSupport::IsLoaded())
	{
		AddError(TEXT("moq_ffi.dll must be loaded for multi-publisher tests."));
		return false;
	}

	const FString RelayUrl = ResolveRelayUrl();
	constexpr int32 PublisherCount = 3;
	AddInfo(FString::Printf(TEXT("Spawning %d concurrent publishers against %s"), PublisherCount, *RelayUrl));

	TArray<TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe>> Harnesses;
	Harnesses.Reserve(PublisherCount);

	for (int32 Index = 0; Index < PublisherCount; ++Index)
	{
		const FString Label = FString::Printf(TEXT("Multi-%d"), Index);
		TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe> Harness = MakeShared<FMoQSessionTestHarness, ESPMode::ThreadSafe>(RelayUrl, Label);
		if (!Harness->Initialize(*this))
		{
			return false;
		}
		if (!Harness->Connect(*this))
		{
			Harness->Disconnect();
			return false;
		}
		Harnesses.Add(Harness);
	}

	for (int32 Index = 0; Index < Harnesses.Num(); ++Index)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForMoQStateCommand(Harnesses[Index], MOQ_STATE_CONNECTED, kConnectionTimeout, this, FString::Printf(TEXT("Multi.Connect.%d"), Index)));
	}

	TSharedRef<TArray<TSharedPtr<FMoQPublisherHandle>>, ESPMode::ThreadSafe> Publishers = MakeShared<TArray<TSharedPtr<FMoQPublisherHandle>>, ESPMode::ThreadSafe>();
	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([this, Harnesses, Publishers]()
	{
		Publishers->Reset();
		for (int32 Index = 0; Index < Harnesses.Num(); ++Index)
		{
			const FString Namespace = MakeUniqueNamespace(FString::Printf(TEXT("o3ds/automation/multi/%d"), Index));
			const FMoQResult AnnounceResult = Harnesses[Index]->Session->AnnounceNamespace(Namespace);
			TestTrue(*FString::Printf(TEXT("Publisher %d announce"), Index), AnnounceResult.IsOk());

			FMoQPublisherConfig Config;
			Config.Namespace = Namespace;
			Config.TrackName = TEXT("primary");
			Config.DeliveryMode = MOQ_DELIVERY_STREAM;

			TSharedPtr<FMoQPublisherHandle> Publisher;
			const FMoQResult CreateResult = Harnesses[Index]->Session->CreatePublisher(Config, Publisher);
			TestTrue(*FString::Printf(TEXT("Publisher %d created"), Index), CreateResult.IsOk());

			if (Publisher.IsValid())
			{
				const TArray<uint8> Payload = BuildSerializedPayload(FString::Printf(TEXT("MultiPayload-%d"), Index));
				if (!Payload.IsEmpty())
				{
					const MoqResult PublishResult = moq_publish_data(Publisher->Get(), Payload.GetData(), Payload.Num(), Config.DeliveryMode);
					TestTrue(*FString::Printf(TEXT("Publisher %d send"), Index), PublishResult.code == MOQ_OK);
				}
				Publishers->Add(Publisher);
			}
		}

		TestEqual(TEXT("Publisher handles"), Publishers->Num(), Harnesses.Num());
	}));

	ADD_LATENT_AUTOMATION_COMMAND(FMoQCallLambdaLatentCommand([Publishers]()
	{
		Publishers->Reset();
	}));

	for (const TSharedRef<FMoQSessionTestHarness, ESPMode::ThreadSafe>& Harness : Harnesses)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FMoQSessionTeardownCommand(Harness));
	}

	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
