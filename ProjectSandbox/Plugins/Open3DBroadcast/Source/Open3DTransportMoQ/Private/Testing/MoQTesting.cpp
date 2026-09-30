// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Testing/MoQTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Receiver/MoQReceiver.h"
#include "Sender/MoQSender.h"
#include "Shared/MoQAsyncDispatcher.h"
#include "Shared/MoQHandles.h"
#include "Shared/MoQHelpers.h"
#include "Shared/MoQSessionWrapper.h"
#include "Shared/MoQTypes.h"
#include "Misc/ScopeLock.h"

/**
 * White-box access to FMoQSessionWrapper, which befriends this class. It lives here, inside the
 * module, so tests in other modules never see the wrapper's private members (ADR 0006).
 */
class FMoQSessionWrapperTestHelper
{
public:
	static void InvokeConnectionState(FMoQSessionWrapper& Wrapper, MoqConnectionState State)
	{
		Wrapper.HandleConnectionStateInternal(State);
	}

	static void InvokeSubscriberCallback(const TFunction<void(const TArray64<uint8>&)>& Callback, const TArray64<uint8>& Payload)
	{
		FMoQSessionWrapper::InvokeSubscriberThunkForTest(Callback, Payload);
	}

	static void InvokeSubscriberThunkWithToken(void* Token, const TArray64<uint8>& Payload)
	{
		const uint8* DataPtr = Payload.Num() > 0 ? Payload.GetData() : nullptr;
		FMoQSessionWrapper::HandleSubscriberDataThunk(Token, DataPtr, static_cast<size_t>(Payload.Num()));
	}

	static void InvokeConnectionThunkWithToken(void* Token, MoqConnectionState State)
	{
		FMoQSessionWrapper::HandleConnectionStateThunk(Token, State);
	}

	static void* GetConnectionToken(const FMoQSessionWrapper& Wrapper)
	{
		return Wrapper.ActiveAttemptToken;
	}

	static int32 GetAnnouncedNamespaceCount(const FMoQSessionWrapper& Wrapper)
	{
		FScopeLock Lock(&Wrapper.NamespaceMutex);
		return Wrapper.AnnouncedNamespaces.Num();
	}
};

namespace
{
	FMoQTestResult Flatten(const FMoQResult& Result)
	{
		FMoQTestResult Out;
		Out.bOk = Result.IsOk();
		Out.Code = LexToString(Result.Code);
		Out.Message = Result.Message;
		return Out;
	}
}

struct FMoQTestSession::FImpl
{
	TSharedPtr<FMoQSessionWrapper, ESPMode::ThreadSafe> Session;
	TArray<TSharedPtr<FMoQPublisherHandle>> Publishers;
	TArray<TSharedPtr<FMoQSubscriberHandle>> Subscribers;
};

FMoQTestSession::FMoQTestSession(TSharedPtr<const FMoQFfiApi, ESPMode::ThreadSafe> Api)
	: Impl(MakeUnique<FImpl>())
{
	if (Api.IsValid())
	{
		Impl->Session = MakeShared<FMoQSessionWrapper, ESPMode::ThreadSafe>(Api.ToSharedRef());
	}
	else
	{
		Impl->Session = MakeShared<FMoQSessionWrapper, ESPMode::ThreadSafe>();
	}
}

FMoQTestSession::~FMoQTestSession()
{
	// Children first, then the session (its destructor disconnects).
	Impl->Subscribers.Reset();
	Impl->Publishers.Reset();
	Impl->Session.Reset();
}

FMoQTestResult FMoQTestSession::Initialize(const FString& RelayUrl)
{
	return Flatten(Impl->Session->Initialize(RelayUrl));
}

FMoQTestResult FMoQTestSession::Connect()
{
	return Flatten(Impl->Session->Connect());
}

void FMoQTestSession::Disconnect()
{
	Impl->Subscribers.Reset();
	Impl->Publishers.Reset();
	Impl->Session->Disconnect();
}

bool FMoQTestSession::IsConnected() const
{
	return Impl->Session->IsConnected();
}

FString FMoQTestSession::GetLastConnectError() const
{
	return Impl->Session->GetLastConnectError();
}

FDelegateHandle FMoQTestSession::AddConnectionStateHandler(TFunction<void(MoqConnectionState)> Handler)
{
	return Impl->Session->OnConnectionStateChanged().AddLambda([Handler = MoveTemp(Handler)](MoqConnectionState State)
	{
		if (Handler)
		{
			Handler(State);
		}
	});
}

void FMoQTestSession::RemoveConnectionStateHandler(FDelegateHandle Handle)
{
	Impl->Session->OnConnectionStateChanged().Remove(Handle);
}

FMoQTestResult FMoQTestSession::AnnounceNamespace(const FString& Namespace)
{
	return Flatten(Impl->Session->AnnounceNamespace(Namespace));
}

FMoQTestResult FMoQTestSession::CreatePublisher(const FString& Namespace, const FString& TrackName, MoqDeliveryMode DeliveryMode, bool& bOutCreated)
{
	FMoQPublisherConfig Config;
	Config.Namespace = Namespace;
	Config.TrackName = TrackName;
	Config.DeliveryMode = DeliveryMode;

	TSharedPtr<FMoQPublisherHandle> Publisher;
	const FMoQResult Result = Impl->Session->CreatePublisher(Config, Publisher);
	bOutCreated = Publisher.IsValid();
	if (bOutCreated)
	{
		Impl->Publishers.Add(Publisher);
	}
	return Flatten(Result);
}

FMoQTestResult FMoQTestSession::Subscribe(const FString& Namespace, const FString& TrackName, TFunction<void(const TArray64<uint8>&)> OnData, bool& bOutCreated)
{
	FMoQSubscriptionConfig Config;
	Config.Namespace = Namespace;
	Config.TrackName = TrackName;
	Config.OnData = MoveTemp(OnData);

	TSharedPtr<FMoQSubscriberHandle> Subscriber;
	const FMoQResult Result = Impl->Session->Subscribe(Config, Subscriber);
	bOutCreated = Subscriber.IsValid();
	if (bOutCreated)
	{
		Impl->Subscribers.Add(Subscriber);
	}
	return Flatten(Result);
}

void FMoQTestSession::InvokeConnectionState(MoqConnectionState State)
{
	FMoQSessionWrapperTestHelper::InvokeConnectionState(*Impl->Session, State);
}

void* FMoQTestSession::GetConnectionToken() const
{
	return FMoQSessionWrapperTestHelper::GetConnectionToken(*Impl->Session);
}

int32 FMoQTestSession::GetAnnouncedNamespaceCount() const
{
	return FMoQSessionWrapperTestHelper::GetAnnouncedNamespaceCount(*Impl->Session);
}

namespace MoQTesting
{
	TSharedRef<IOpen3DSender> CreateSenderForTest(FMoQFfiApiRef Api, TFunction<double()> Clock, uint64 JitterSeed)
	{
		return MakeShared<FO3DMoQSender>(MoveTemp(Api), MoveTemp(Clock), JitterSeed);
	}

	TSharedRef<IOpen3DReceiver> CreateReceiverForTest(FMoQFfiApiRef Api, TFunction<double()> Clock, uint64 JitterSeed)
	{
		return MakeShared<FO3DMoQReceiver>(MoveTemp(Api), MoveTemp(Clock), JitterSeed);
	}

	int32 PumpDispatcher()
	{
		return FMoQAsyncDispatcher::Get().DrainOnGameThread();
	}

	void InitializeDispatcher()
	{
		FMoQAsyncDispatcher::Get().Initialize();
	}

	void ShutdownDispatcher()
	{
		FMoQAsyncDispatcher::Get().Shutdown();
	}

	bool IsDispatcherAccepting()
	{
		return FMoQAsyncDispatcher::Get().IsAcceptingTasks();
	}

	bool EnqueueOnDispatcher(TUniqueFunction<void()>&& Task)
	{
		return FMoQAsyncDispatcher::Get().EnqueueGameThreadTask(MoveTemp(Task));
	}

	void InvokeSubscriberCallback(const TFunction<void(const TArray64<uint8>&)>& Callback, const TArray64<uint8>& Payload)
	{
		FMoQSessionWrapperTestHelper::InvokeSubscriberCallback(Callback, Payload);
	}

	void InvokeSubscriberThunkWithToken(void* Token, const TArray64<uint8>& Payload)
	{
		FMoQSessionWrapperTestHelper::InvokeSubscriberThunkWithToken(Token, Payload);
	}

	void InvokeConnectionThunkWithToken(void* Token, MoqConnectionState State)
	{
		FMoQSessionWrapperTestHelper::InvokeConnectionThunkWithToken(Token, State);
	}

	FMoQTestBackoffLimits GetBackoffLimits()
	{
		FMoQTestBackoffLimits Limits;
		Limits.MinReconnectDelaySeconds = MoQHelpers::kMinReconnectDelaySeconds;
		Limits.MaxReconnectDelaySeconds = MoQHelpers::kMaxReconnectDelaySeconds;
		Limits.BackoffJitterFraction = MoQHelpers::kBackoffJitterFraction;
		Limits.DefaultConnectTimeoutSeconds = MoQHelpers::kDefaultConnectTimeoutSeconds;
		Limits.MinConnectTimeoutSeconds = MoQHelpers::kMinConnectTimeoutSeconds;
		Limits.MaxConnectTimeoutSeconds = MoQHelpers::kMaxConnectTimeoutSeconds;
		Limits.MinQueueBytes = MoQHelpers::kMinQueueBytes;
		return Limits;
	}

	double ComputeReconnectDelaySeconds(int32 ConsecutiveFailures)
	{
		return MoQHelpers::ComputeReconnectDelaySeconds(ConsecutiveFailures);
	}

	double ComputeBackoffDelaySeconds(int32 ConsecutiveFailures, uint64 JitterSeed)
	{
		return MoQHelpers::ComputeBackoffDelaySeconds(ConsecutiveFailures, JitterSeed);
	}

	double ResolveConnectTimeoutSeconds(const FO3DTransportConfig& Config)
	{
		return MoQHelpers::ResolveConnectTimeoutSeconds(Config);
	}

	bool TryGetAudioCodecFromFrame(const uint8* Payload, int32 PayloadSize, O3DS::EUnifiedCodec& OutCodec)
	{
		return MoQHelpers::TryGetAudioCodecFromFrame(Payload, PayloadSize, OutCodec);
	}

	FMoQTestResult MakeResultFromRawCode(MoqResultCode RawCode, const FString& Message)
	{
		return Flatten(FMoQResult::FromCode(ToMoQErrorCode(RawCode), Message, RawCode));
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_MOQ
