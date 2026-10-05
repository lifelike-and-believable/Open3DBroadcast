// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#include "O3DConformance.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/ScopeLock.h"
#include "Transport/O3DTransportRegistry.h"

namespace
{
	struct FProfileRegistry
	{
		FCriticalSection Mutex;
		TMap<FName, FO3DConformanceProfile> Profiles;
		TMap<FName, FString> Deferred;
	};

	FProfileRegistry& GetRegistry()
	{
		static FProfileRegistry Registry;
		return Registry;
	}
}

FO3DConformanceFixture::FO3DConformanceFixture(FName InTransportName)
	: TransportName(InTransportName)
{
}

FO3DConformanceFixture::~FO3DConformanceFixture() = default;

TSharedPtr<IOpen3DSender> FO3DConformanceFixture::CreateSender()
{
	return FO3DTransportRegistry::Get().CreateSender(TransportName);
}

TSharedPtr<IOpen3DReceiver> FO3DConformanceFixture::CreateReceiver()
{
	return FO3DTransportRegistry::Get().CreateReceiver(TransportName);
}

bool FO3DConformanceFixture::RunDestroyWithCallbacksInFlight(FAutomationTestBase& Test)
{
	Test.AddError(FString::Printf(TEXT("%s lists LifetimeDestroyWithCallbacksInFlight but its fixture does not implement it."), *TransportName.ToString()));
	return false;
}

namespace O3DTests
{
	void RegisterConformanceProfile(FName Transport, const FO3DConformanceProfile& Profile)
	{
		FProfileRegistry& Registry = GetRegistry();
		FScopeLock Lock(&Registry.Mutex);
		Registry.Profiles.Add(Transport, Profile);
	}

	void UnregisterConformanceProfile(FName Transport)
	{
		FProfileRegistry& Registry = GetRegistry();
		FScopeLock Lock(&Registry.Mutex);
		Registry.Profiles.Remove(Transport);
	}

	bool FindConformanceProfile(FName Transport, FO3DConformanceProfile& OutProfile)
	{
		FProfileRegistry& Registry = GetRegistry();
		FScopeLock Lock(&Registry.Mutex);
		if (const FO3DConformanceProfile* Found = Registry.Profiles.Find(Transport))
		{
			OutProfile = *Found;
			return true;
		}
		return false;
	}

	TArray<FName> GetSelfRegisteringProfiles()
	{
		FProfileRegistry& Registry = GetRegistry();
		FScopeLock Lock(&Registry.Mutex);
		TArray<FName> Names;
		for (const TPair<FName, FO3DConformanceProfile>& Pair : Registry.Profiles)
		{
			if (Pair.Value.bSelfRegistering)
			{
				Names.Add(Pair.Key);
			}
		}
		return Names;
	}

	void DeferConformanceProfile(FName Transport, const FString& Reason)
	{
		FProfileRegistry& Registry = GetRegistry();
		FScopeLock Lock(&Registry.Mutex);
		Registry.Deferred.Add(Transport, Reason);
	}

	void UndeferConformanceProfile(FName Transport)
	{
		FProfileRegistry& Registry = GetRegistry();
		FScopeLock Lock(&Registry.Mutex);
		Registry.Deferred.Remove(Transport);
	}

	bool IsConformanceProfileDeferred(FName Transport, FString* OutReason)
	{
		FProfileRegistry& Registry = GetRegistry();
		FScopeLock Lock(&Registry.Mutex);
		if (const FString* Found = Registry.Deferred.Find(Transport))
		{
			if (OutReason)
			{
				*OutReason = *Found;
			}
			return true;
		}
		return false;
	}

	FString GetConformanceCaseName(EO3DConformanceCase Case)
	{
		switch (Case)
		{
		case EO3DConformanceCase::LifecycleStopIsIdempotent: return TEXT("Lifecycle.StopIsIdempotent");
		case EO3DConformanceCase::LifecycleRestartAfterStop: return TEXT("Lifecycle.RestartAfterStop");
		case EO3DConformanceCase::ReceiverLifecycle: return TEXT("Lifecycle.ReceiverStopIsIdempotent");
		case EO3DConformanceCase::SendRejectedWhenNotRunning: return TEXT("Send.RejectedWhenNotRunning");
		case EO3DConformanceCase::SendBackpressure: return TEXT("Send.BackpressureDropsWithoutBlocking");
		case EO3DConformanceCase::SendConcurrent: return TEXT("Send.ConcurrentFromFourThreads");
		case EO3DConformanceCase::StatsMonotonic: return TEXT("Stats.MonotonicUnderLoad");
		case EO3DConformanceCase::RoundTripByteExact: return TEXT("RoundTrip.RecordedFramesByteExact");
		case EO3DConformanceCase::LifetimeDestroyWithCallbacksInFlight: return TEXT("Lifetime.DestroyWithCallbacksInFlight");
		case EO3DConformanceCase::ControlRoundTrip: return TEXT("Control.RoundTripBesideMocap");
		case EO3DConformanceCase::ControlRejectedWhenNotRunning: return TEXT("Control.RejectedWhenNotRunning");
		case EO3DConformanceCase::ControlStopWhileSending: return TEXT("Control.StopWhileFourThreadsSend");
		case EO3DConformanceCase::ReceiverStartWithoutConsumer: return TEXT("Lifecycle.ReceiverStartWithoutConsumerFails");
		case EO3DConformanceCase::SendEmptyPayloadInvalid: return TEXT("Send.EmptyPayloadIsInvalid");
		case EO3DConformanceCase::CapabilitiesMatch: return TEXT("Capabilities.MatchProfileAndDescriptor");
		case EO3DConformanceCase::ConnectionStateLifecycle: return TEXT("State.StartStopReportOnCallingThread");
		case EO3DConformanceCase::ConnectionStateConnected: return TEXT("State.ConnectedAfterExchange");
		default: return FString();
		}
	}

	TArray<EO3DConformanceCase> GetAllConformanceCases()
	{
		return {
			EO3DConformanceCase::LifecycleStopIsIdempotent,
			EO3DConformanceCase::LifecycleRestartAfterStop,
			EO3DConformanceCase::ReceiverLifecycle,
			EO3DConformanceCase::SendRejectedWhenNotRunning,
			EO3DConformanceCase::SendBackpressure,
			EO3DConformanceCase::SendConcurrent,
			EO3DConformanceCase::StatsMonotonic,
			EO3DConformanceCase::RoundTripByteExact,
			EO3DConformanceCase::LifetimeDestroyWithCallbacksInFlight,
			EO3DConformanceCase::ControlRoundTrip,
			EO3DConformanceCase::ControlRejectedWhenNotRunning,
			EO3DConformanceCase::ControlStopWhileSending,
			EO3DConformanceCase::ReceiverStartWithoutConsumer,
			EO3DConformanceCase::SendEmptyPayloadInvalid,
			EO3DConformanceCase::CapabilitiesMatch,
			EO3DConformanceCase::ConnectionStateLifecycle,
			EO3DConformanceCase::ConnectionStateConnected,
		};
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
