// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

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
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"
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
	/** SendSerialized before Initialize and before Start returns NotRunning and counts no frame. */
	SendRejectedWhenNotRunning = 1 << 3,
	/**
	 * A full queue drops the frame (DroppedBackpressure), increments Stats.DroppedFrames and never
	 * blocks. Frames below the queue's size fill it while the fixture holds the sender's worker
	 * (WP-R3, TR-7: one payload larger than the queue is TooLarge, not backpressure).
	 */
	SendBackpressure = 1 << 4,
	/** Four threads call SendSerialized at once; every call returns. */
	SendConcurrent = 1 << 5,
	/** GetStats counters never decrease while four threads send. */
	StatsMonotonic = 1 << 6,
	/** Recorded frames arrive at the consumer byte-exact and in order (reliable transports). */
	RoundTripByteExact = 1 << 7,
	/** Destroying the sender while FFI callbacks are still in flight is safe (fake FFI only). */
	LifetimeDestroyWithCallbacksInFlight = 1 << 8,
	/**
	 * Control (ADR 0011): envelopes sent between mocap frames reach the control sink byte-exact
	 * and in order; the frames still reach the consumer byte-exact and in order, and no control
	 * reaches the consumer or counts as a sent frame. For transports that deliver reliably.
	 */
	ControlRoundTrip = 1 << 9,
	/**
	 * Control: SendControl before Initialize, before Start and after Stop returns NotRunning,
	 * bytes that are not a control envelope return Invalid, and no frame is counted.
	 */
	ControlRejectedWhenNotRunning = 1 << 10,
	/** Control: Stop() while four threads call SendControl returns, every call returns, and later sends are refused. */
	ControlStopWhileSending = 1 << 11,
	/** Receiver: Start without a consumer fails with NoConsumer and leaves the state Idle (ADR 0007 Verification). */
	ReceiverStartWithoutConsumer = 1 << 12,
	/** An empty SendSerialized payload on a started sender returns Invalid and counts no frame. */
	SendEmptyPayloadInvalid = 1 << 13,
	/**
	 * Sender and receiver GetCapabilities() match the profile's ExpectedCapabilities and the
	 * registry descriptor's GetCapabilities for the same config (ADR 0007 item 4, WP-A1 PR 3).
	 */
	CapabilitiesMatch = 1 << 14,
	/**
	 * Connection state (ADR 0007 item 3): Idle before Start; Start and Stop report their changes
	 * through the callback on the calling (game) thread before they return; nothing is reported
	 * after Stop; the last callback always matches GetConnectionState() and Stats.State.
	 */
	ConnectionStateLifecycle = 1 << 15,
	/** Connection state: a sender and receiver that exchanged a frame both reach Connected. */
	ConnectionStateConnected = 1 << 16,
	/**
	 * WP-R3 (mid-project review TR-5, SR-4): frames that reached the receiver are counted in
	 * Stats.FramesSent, and their bytes in the sender metrics handle the config passes
	 * (FO3DTransportConfig::SenderMetrics), which o3d.DumpMetrics and the sender component read.
	 */
	MetricsCountWhatWasSent = 1 << 17,
	/** Receiver GetStats, read from another thread while the game thread polls, never goes backwards (WP-R3, TR-9). */
	ReceiverStatsFromAnyThread = 1 << 18,
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

	/**
	 * SendBackpressure: stops (true) or resumes (false) the sender's worker, so the case's sends
	 * fill the queue instead of racing the worker that drains it. Default: nothing, for transports
	 * whose queue nobody drains during the case (the fake transport, Loopback).
	 */
	virtual void HoldSenderWorker(IOpen3DSender& Sender, bool bHold) {}

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

	/** SendBackpressure: payload size (at most the backpressure queue's size) and number of sends that overflow it. */
	int32 BackpressurePayloadBytes = 0;
	int32 BackpressureSendCount = 1;
	/** SendBackpressure: the queue only fills while a receiver is connected (TCP). */
	bool bBackpressureNeedsPeer = false;

	/** ControlStopWhileSending: start/stop cycles, each on a new sender (1 where a port lingers after close). */
	int32 ControlStopCycles = 5;

	/**
	 * CapabilitiesMatch: what the transport's sender and receiver report for the fixture's configs
	 * (the delivery guarantee of ADR 0005 (iii), audio and control support, the payload limit).
	 */
	FO3DTransportCapabilities ExpectedCapabilities;

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
	 * for WebRTC, which the Open3DBroadcastWebRTC add-on registers, until an add-on test module
	 * registers its profile and removes the deferral (ADR 0006, WP-T2e).
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
