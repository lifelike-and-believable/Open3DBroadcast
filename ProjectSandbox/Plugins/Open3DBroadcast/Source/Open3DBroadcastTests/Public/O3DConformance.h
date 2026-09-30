// Copyright (c) Open3DStream Contributors

#pragma once

// Transport conformance registry (ADR 0006 §4). Each transport supplies an offline profile: a
// fixture that builds local configs (127.0.0.1 sockets or a fake FFI table) and the set of cases
// that apply. The suite (Open3DBroadcast.Conformance.<Transport>.<Case>) enumerates the
// registered senders and receivers and runs every case of every profile. A registered transport
// with neither a profile nor a recorded deferral gets one failing test,
// Open3DBroadcast.Conformance.<Transport>.HasProfile, so a new transport cannot silently escape.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DReceiverInterface.h"
#include "O3DSenderInterface.h"
#include "O3DTransportTypes.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

/** Conformance cases. A profile lists the ones that apply to its transport. */
enum class EO3DConformanceCase : uint32
{
	None = 0,
	/** Initialize, Start, Stop, Stop; a send after Stop is rejected. */
	LifecycleStopIsIdempotent = 1 << 0,
	/** Start after Stop works without Initialize, and Initialize+Start works again after that. */
	LifecycleRestartAfterStop = 1 << 1,
	/** Receiver: Poll before Start and after Stop delivers nothing; Stop twice is safe. */
	ReceiverLifecycle = 1 << 2,
	/** SendSerialized before Initialize and before Start returns false and counts no frame. */
	SendRejectedWhenNotRunning = 1 << 3,
	/** A full queue drops the frame, increments Stats.DroppedFrames and never blocks. */
	SendBackpressure = 1 << 4,
	/** Four threads call SendSerialized at once; every call returns. */
	SendConcurrent = 1 << 5,
	/** GetStats counters never decrease while four threads send. */
	StatsMonotonic = 1 << 6,
	/** Recorded frames arrive at the consumer byte-exact and in order (reliable transports). */
	RoundTripByteExact = 1 << 7,
	/** Destroying the sender while FFI callbacks are still in flight is safe (fake FFI only). */
	LifetimeDestroyWithCallbacksInFlight = 1 << 8,
};
ENUM_CLASS_FLAGS(EO3DConformanceCase)

/**
 * One test's view of a transport. Created fresh for every conformance test, so it can own
 * per-test state such as a fake FFI or a unique channel name.
 */
class OPEN3DBROADCASTTESTS_API FO3DConformanceFixture
{
public:
	explicit FO3DConformanceFixture(FName InTransportName);
	virtual ~FO3DConformanceFixture();

	FO3DConformanceFixture(const FO3DConformanceFixture&) = delete;
	FO3DConformanceFixture& operator=(const FO3DConformanceFixture&) = delete;

	FName GetTransportName() const { return TransportName; }

	/** A new sender. Default: the registry entry for the transport name. */
	virtual TSharedPtr<IOpen3DSender> CreateSender();
	/** A new receiver. Default: the registry entry for the transport name. */
	virtual TSharedPtr<IOpen3DReceiver> CreateReceiver();

	/** Sender and receiver configs for one local endpoint, shared by both sides. */
	virtual FO3DTransportConfig MakeSenderConfig() = 0;
	virtual FO3DTransportConfig MakeReceiverConfig() = 0;
	/** A sender config whose queue is small enough for the profile's backpressure payloads. */
	virtual FO3DTransportConfig MakeBackpressureSenderConfig() { return MakeSenderConfig(); }

	/** Delivers deferred work, for example FFI callbacks queued for the game thread. Game thread. */
	virtual void Pump() {}

	/** LifetimeDestroyWithCallbacksInFlight. Only fixtures with a fake FFI implement it. */
	virtual bool RunDestroyWithCallbacksInFlight(FAutomationTestBase& Test);

	/**
	 * Registers the warnings and errors the transport is expected to log during Case (with
	 * FAutomationTestBase::AddExpectedError), so a documented log line is not reported as a test
	 * failure. Called before the case runs. Default: none.
	 */
	virtual void AddExpectedMessages(FAutomationTestBase& Test, EO3DConformanceCase Case) {}

private:
	FName TransportName;
};

struct FO3DConformanceProfile
{
	/** Builds the fixture for one test. Required. */
	TFunction<TUniquePtr<FO3DConformanceFixture>()> MakeFixture;
	EO3DConformanceCase Cases = EO3DConformanceCase::None;

	/** SendBackpressure: payload size and number of sends that overflow the backpressure config. */
	int32 BackpressurePayloadBytes = 0;
	int32 BackpressureSendCount = 1;
	/** SendBackpressure: the queue only fills while a receiver is connected (TCP). */
	bool bBackpressureNeedsPeer = false;

	/** Wall-clock limit for a sender and receiver to find each other on 127.0.0.1. */
	double ConnectTimeoutSeconds = 10.0;

	/**
	 * The fixture registers its own transport, so the suite lists the profile even though no
	 * module registered the name (the built-in Fake profile).
	 */
	bool bSelfRegistering = false;
};

namespace O3DTests
{
	/** Game thread, normally from StartupModule. Replaces an earlier profile for the name. */
	OPEN3DBROADCASTTESTS_API void RegisterConformanceProfile(FName Transport, const FO3DConformanceProfile& Profile);
	OPEN3DBROADCASTTESTS_API void UnregisterConformanceProfile(FName Transport);
	OPEN3DBROADCASTTESTS_API bool FindConformanceProfile(FName Transport, FO3DConformanceProfile& OutProfile);
	OPEN3DBROADCASTTESTS_API TArray<FName> GetSelfRegisteringProfiles();

	/**
	 * Records that a transport's profile lives in another module that is not built yet, with the
	 * work package that adds it. The suite then emits no HasProfile failure for that name. Used
	 * for WebRTC until WP-F11 creates Open3DBroadcastWebRTCTests, which registers the profile
	 * and removes the deferral.
	 */
	OPEN3DBROADCASTTESTS_API void DeferConformanceProfile(FName Transport, const FString& Reason);
	OPEN3DBROADCASTTESTS_API void UndeferConformanceProfile(FName Transport);
	OPEN3DBROADCASTTESTS_API bool IsConformanceProfileDeferred(FName Transport, FString* OutReason = nullptr);

	/** "Lifecycle.StopIsIdempotent" and so on; empty for None or a combination. */
	OPEN3DBROADCASTTESTS_API FString GetConformanceCaseName(EO3DConformanceCase Case);
	/** Every single case, in the order the suite lists them. */
	OPEN3DBROADCASTTESTS_API TArray<EO3DConformanceCase> GetAllConformanceCases();
}

#endif // WITH_DEV_AUTOMATION_TESTS
