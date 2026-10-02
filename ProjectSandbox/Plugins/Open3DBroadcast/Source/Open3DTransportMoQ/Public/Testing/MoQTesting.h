// Copyright (c) Open3DStream Contributors

#pragma once

// Test-only access to the MoQ transport (ADR 0006, WP-T2). The Open3DBroadcastTests module is
// the only intended caller. Everything here compiles out when dev automation tests are off, so
// no test code ships in Shipping or Test builds. The session wrapper, handles and helpers stay
// private to this module; tests reach them only through the functions and the façade below.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MoQFfiApi.h"
THIRD_PARTY_INCLUDES_START
#include "moq_ffi.h"
THIRD_PARTY_INCLUDES_END
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

/** Result of a façade call: the private FMoQResult flattened to plain values. */
struct FMoQTestResult
{
	bool bOk = true;
	/** LexToString(EMoQErrorCode), for example "Ok", "InvalidArgument", "NotConnected". */
	FString Code = TEXT("Ok");
	FString Message;

	bool IsOk() const { return bOk; }
};

/** Backoff and timeout limits the MoQ helpers use (MoQHelpers constants). */
struct FMoQTestBackoffLimits
{
	double MinReconnectDelaySeconds = 0.0;
	double MaxReconnectDelaySeconds = 0.0;
	double BackoffJitterFraction = 0.0;
	double DefaultConnectTimeoutSeconds = 0.0;
	double MinConnectTimeoutSeconds = 0.0;
	double MaxConnectTimeoutSeconds = 0.0;
	uint64 MinQueueBytes = 0;
};

/**
 * One FMoQSessionWrapper seen from a test. Owns the wrapper and any publisher or subscriber it
 * creates; destroying the façade releases them and disconnects. Game thread only, like the
 * wrapper itself.
 */
class OPEN3DTRANSPORTMOQ_API FMoQTestSession
{
public:
	/** Uses Api, or the production moq-ffi table when Api is null. */
	explicit FMoQTestSession(TSharedPtr<const FMoQFfiApi, ESPMode::ThreadSafe> Api = nullptr);
	~FMoQTestSession();

	FMoQTestSession(const FMoQTestSession&) = delete;
	FMoQTestSession& operator=(const FMoQTestSession&) = delete;
	FMoQTestSession(FMoQTestSession&&) = delete;
	FMoQTestSession& operator=(FMoQTestSession&&) = delete;

	FMoQTestResult Initialize(const FString& RelayUrl);
	FMoQTestResult Connect();
	void Disconnect();
	bool IsConnected() const;
	FString GetLastConnectError() const;

	/** Adds a handler to OnConnectionStateChanged. The handler runs on the game thread. */
	FDelegateHandle AddConnectionStateHandler(TFunction<void(MoqConnectionState)> Handler);
	void RemoveConnectionStateHandler(FDelegateHandle Handle);

	FMoQTestResult AnnounceNamespace(const FString& Namespace);
	/** bOutCreated reports whether a publisher handle came back; the façade keeps it alive. */
	FMoQTestResult CreatePublisher(const FString& Namespace, const FString& TrackName, MoqDeliveryMode DeliveryMode, bool& bOutCreated);
	/** OnData may be empty to exercise the "callback required" check. */
	FMoQTestResult Subscribe(const FString& Namespace, const FString& TrackName, TFunction<void(const TArray64<uint8>&)> OnData, bool& bOutCreated);

	/** White-box: records State for the current attempt and dispatches it (FFI thread path). */
	void InvokeConnectionState(MoqConnectionState State);
	/** White-box: token of the current connect attempt, or null when none is current. */
	void* GetConnectionToken() const;
	/** White-box: namespaces announced on the current client. */
	int32 GetAnnouncedNamespaceCount() const;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};

namespace MoQTesting
{
	/** A MoQ sender that uses Api, Clock (null = platform clock) and a fixed jitter seed (ADR 0006 F2). */
	OPEN3DTRANSPORTMOQ_API TSharedRef<IOpen3DSender> CreateSenderForTest(FMoQFfiApiRef Api, TFunction<double()> Clock, uint64 JitterSeed);

	/** A MoQ receiver that uses Api, Clock (null = platform clock) and a fixed jitter seed. */
	OPEN3DTRANSPORTMOQ_API TSharedRef<IOpen3DReceiver> CreateReceiverForTest(FMoQFfiApiRef Api, TFunction<double()> Clock, uint64 JitterSeed);

	/** Runs every FFI callback queued for the game thread, as the dispatcher's ticker would. Game thread. */
	OPEN3DTRANSPORTMOQ_API int32 PumpDispatcher();
	OPEN3DTRANSPORTMOQ_API void InitializeDispatcher();
	OPEN3DTRANSPORTMOQ_API void ShutdownDispatcher();
	OPEN3DTRANSPORTMOQ_API bool IsDispatcherAccepting();
	OPEN3DTRANSPORTMOQ_API bool EnqueueOnDispatcher(TUniqueFunction<void()>&& Task);

	/** Runs the private subscriber thunk with a freshly registered binding for Callback. */
	OPEN3DTRANSPORTMOQ_API void InvokeSubscriberCallback(const TFunction<void(const TArray64<uint8>&)>& Callback, const TArray64<uint8>& Payload);
	/** Runs the raw FFI subscriber thunk with an arbitrary user_data value (stale or unknown token). */
	OPEN3DTRANSPORTMOQ_API void InvokeSubscriberThunkWithToken(void* Token, const TArray64<uint8>& Payload);
	/** Runs the raw FFI connection thunk with an arbitrary user_data value. */
	OPEN3DTRANSPORTMOQ_API void InvokeConnectionThunkWithToken(void* Token, MoqConnectionState State);

	OPEN3DTRANSPORTMOQ_API FMoQTestBackoffLimits GetBackoffLimits();
	OPEN3DTRANSPORTMOQ_API double ComputeReconnectDelaySeconds(int32 ConsecutiveFailures);
	OPEN3DTRANSPORTMOQ_API double ComputeBackoffDelaySeconds(int32 ConsecutiveFailures, uint64 JitterSeed);
	OPEN3DTRANSPORTMOQ_API double ResolveConnectTimeoutSeconds(const FO3DTransportConfig& Config);
	OPEN3DTRANSPORTMOQ_API bool TryGetAudioCodecFromFrame(const uint8* Payload, int32 PayloadSize, O3DS::EUnifiedCodec& OutCodec);

	/**
	 * Pauses or resumes the sender's worker (WP-A1 PR 4e): while paused it publishes nothing, so
	 * the send queue's policy can be observed. Pausing returns once the worker has seen the flag.
	 * Sender must come from CreateSenderForTest().
	 */
	OPEN3DTRANSPORTMOQ_API void SenderSetWorkerPaused(IOpen3DSender& Sender, bool bPaused);

	/** FMoQResult::FromCode(ToMoQErrorCode(RawCode), Message), flattened. */
	OPEN3DTRANSPORTMOQ_API FMoQTestResult MakeResultFromRawCode(MoqResultCode RawCode, const FString& Message);
}

#endif // WITH_DEV_AUTOMATION_TESTS
