// Copyright (c) Open3DStream Contributors

#pragma once

// Shared fixtures for Open3DBroadcast automation tests (ADR 0006, WP-T2). Exported so an add-on
// plugin's test module (for example Open3DBroadcastWebRTC's) can reuse them.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/CriticalSection.h"
#include "Misc/AutomationTest.h"
#include "SerializedFrameConsumerRegistry.h"
#include "Templates/Function.h"

/**
 * Flags for every Open3DBroadcast test. All tests run in the editor (this is an Editor module)
 * and belong to the engine filter. ADR 0006 Q4: the UE 5.7 spelling of an application-wide
 * context for pure-logic tests is unverified, so the EditorContext form that compiles today is
 * used everywhere and changed here in one place once it is checked.
 */
#define O3DB_TEST_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace O3DTests
{
	/**
	 * Runs Pump, then checks Condition, until Condition holds or TimeoutSeconds of wall time
	 * pass. Yields between attempts and never sleeps. Returns the final value of Condition.
	 */
	OPEN3DBROADCASTTESTS_API bool PollUntil(double TimeoutSeconds, TFunctionRef<bool()> Condition, TFunctionRef<void()> Pump);
	OPEN3DBROADCASTTESTS_API bool PollUntil(double TimeoutSeconds, TFunctionRef<bool()> Condition);

	/**
	 * A 127.0.0.1 port that was free a moment ago: binds a socket to port 0, reads the port the
	 * OS chose and closes the socket. Returns 0 on failure. No fixed ports (ADR 0006 §9).
	 */
	OPEN3DBROADCASTTESTS_API int32 FindFreeLoopbackPort(bool bTcp);

	/**
	 * Count full-sync O3DS frames of one subject (three bones whose pose changes per frame),
	 * written through an in-memory capture and read back with ReplayCapture (src/o3ds/capture,
	 * src/o3ds/replay), so they are byte-for-byte what a recorded session would replay.
	 */
	OPEN3DBROADCASTTESTS_API TArray<TArray<uint8>> MakeRecordedFrames(const FString& SubjectName, int32 Count);

	/** Prefix_<Guid>, unique for this process. Used for fake transport names and channel keys. */
	OPEN3DBROADCASTTESTS_API FString MakeUniqueName(const TCHAR* Prefix);

	/** True when the environment variable O3DB_NETWORK_TESTS is "1" (ADR 0006 §6). */
	OPEN3DBROADCASTTESTS_API bool AreNetworkTestsEnabled();
}

/** Serialized-frame consumer that records every frame. Thread-safe. */
class OPEN3DBROADCASTTESTS_API FO3DRecordingFrameConsumer : public ISerializedFrameConsumer
{
public:
	FO3DRecordingFrameConsumer() = default;
	virtual ~FO3DRecordingFrameConsumer() override = default;

	FO3DRecordingFrameConsumer(const FO3DRecordingFrameConsumer&) = delete;
	FO3DRecordingFrameConsumer& operator=(const FO3DRecordingFrameConsumer&) = delete;

	virtual void SubmitFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds) override;

	int32 Num() const;
	TArray<TArray<uint8>> GetFrames() const;
	/** Thread ids (FPlatformTLS) that called SubmitFrame, in call order. */
	TArray<uint32> GetCallerThreads() const;
	void Reset();

private:
	mutable FCriticalSection Mutex;
	TArray<TArray<uint8>> Frames;
	TArray<uint32> CallerThreads;
};

#endif // WITH_DEV_AUTOMATION_TESTS
