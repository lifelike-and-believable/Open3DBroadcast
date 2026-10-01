// Copyright (c) Open3DStream Contributors

#pragma once

// Fake transports for tests (ADR 0006 §3). FO3DFakeSender records what it is given and can
// script backpressure; FO3DFakeReceiver lets a test deliver bytes to its consumer on a chosen
// thread. Both carry control (ADR 0011) in-band: the sender pushes control envelopes through the
// same link as frames, and the receiver classifies each item with O3DS::TryGetControlPayload,
// the classifier real receivers use, so control never reaches the frame consumer. FO3DFakeTransportScope registers both under a name that is unique to one test, so they
// never override or appear next to a real transport.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "HAL/CriticalSection.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Transport/O3DTransportRegistry.h"

#include <atomic>

/** Hand-off from linked fake senders to linked fake receivers (a loopback without a channel registry). */
class OPEN3DBROADCASTTESTS_API FO3DFakeLink
{
public:
	FO3DFakeLink() = default;
	FO3DFakeLink(const FO3DFakeLink&) = delete;
	FO3DFakeLink& operator=(const FO3DFakeLink&) = delete;

	void Push(const TArray<uint8>& Bytes);
	TArray<TArray<uint8>> Drain();

private:
	FCriticalSection Mutex;
	TArray<TArray<uint8>> Pending;
};

using FO3DFakeLinkRef = TSharedRef<FO3DFakeLink, ESPMode::ThreadSafe>;

/** Capabilities of the fake transport (ADR 0007 item 4): in-order link, control, receiver audio. */
OPEN3DBROADCASTTESTS_API FO3DTransportCapabilities GetFakeTransportCapabilities();

/**
 * IOpen3DSender that follows the transport contract the conformance suite checks: sends are
 * rejected outside Initialize+Start (NotRunning), Stop is idempotent, counters only grow, and a
 * full queue drops the frame, counts it and returns DroppedBackpressure. Connected from Start to
 * Stop. Thread-safe.
 */
class OPEN3DBROADCASTTESTS_API FO3DFakeSender : public IOpen3DSender
{
public:
	explicit FO3DFakeSender(TSharedPtr<FO3DFakeLink, ESPMode::ThreadSafe> InLink = nullptr);
	virtual ~FO3DFakeSender() override = default;

	FO3DFakeSender(const FO3DFakeSender&) = delete;
	FO3DFakeSender& operator=(const FO3DFakeSender&) = delete;

	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual bool Send(const O3DS::SubjectList& List) override;
	virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return GetFakeTransportCapabilities(); }
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	/** Same running and backpressure rules as SendSerialized (Invalid for bytes that are not a control envelope); not counted as a frame. */
	virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len) override;

	/**
	 * Moves the connection state as a transport's worker or FFI thread would, on the calling
	 * thread (ignored outside Start..Stop). For connection-state tests.
	 */
	void SimulateConnectionState(EO3DConnectionState State, const FO3DTransportResult& Reason = FO3DTransportResult()) { ConnectionState.Set(State, Reason); }

	/**
	 * Scripted backpressure: at most MaxQueued payloads may wait in the fake's queue; further
	 * sends are dropped and counted until DrainQueue() empties it. Negative means unbounded.
	 * The config key "fake.maxqueued" sets the same value at Initialize.
	 */
	void SetMaxQueued(int32 InMaxQueued);
	/** Empties the queue, as a transport worker would after sending. Returns how many were queued. */
	int32 DrainQueue();

	/** Every accepted payload, in order. */
	TArray<TArray<uint8>> GetRecordedPayloads() const;
	/** Every accepted control envelope, in order. */
	TArray<TArray<uint8>> GetRecordedControl() const;
	int32 GetControlCalls() const { return ControlCalls.load(); }
	int32 GetSendCalls() const { return SendCalls.load(); }
	int32 GetStartCalls() const { return StartCalls.load(); }
	int32 GetStopCalls() const { return StopCalls.load(); }
	FO3DTransportConfig GetLastConfig() const;

private:
	TSharedPtr<FO3DFakeLink, ESPMode::ThreadSafe> Link;
	mutable FCriticalSection Mutex;
	FO3DTransportConfig LastConfig;
	FO3DTransportStats Stats;
	TArray<TArray<uint8>> Recorded;
	TArray<TArray<uint8>> RecordedControl;
	int32 Queued = 0;
	int32 MaxQueued = -1;
	bool bInitialized = false;
	std::atomic<bool> bRunning{false};
	std::atomic<int32> SendCalls{0};
	std::atomic<int32> StartCalls{0};
	std::atomic<int32> StopCalls{0};
	std::atomic<int32> ControlCalls{0};
	FO3DConnectionStateTracker ConnectionState;
};

/**
 * IOpen3DReceiver whose frames come from a test. Poll() delivers linked-sender frames and frames
 * queued with Enqueue() on the calling (game) thread. InjectNow() and InjectOnBackgroundThread()
 * call the consumer directly, which is how a transport's network thread would misbehave; they
 * exist to test consumers against that. Start without a consumer returns NoConsumer; Connected
 * from Start to Stop.
 */
class OPEN3DBROADCASTTESTS_API FO3DFakeReceiver : public IOpen3DReceiver
{
public:
	explicit FO3DFakeReceiver(TSharedPtr<FO3DFakeLink, ESPMode::ThreadSafe> InLink = nullptr);
	virtual ~FO3DFakeReceiver() override = default;

	FO3DFakeReceiver(const FO3DFakeReceiver&) = delete;
	FO3DFakeReceiver& operator=(const FO3DFakeReceiver&) = delete;

	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) override;
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return GetFakeTransportCapabilities(); }
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
	virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override;

	/** Queues Bytes for the next Poll(). Any thread. */
	void Enqueue(const TArray<uint8>& Bytes);
	/** Calls the consumer with Bytes on the calling thread. Returns false without a consumer or while stopped. */
	bool InjectNow(const TArray<uint8>& Bytes);
	/**
	 * Calls Receiver's consumer with Bytes on a background task-graph thread; wait on the
	 * returned event. The task holds a weak reference, so it never keeps the receiver alive.
	 */
	static FGraphEventRef InjectOnBackgroundThread(const TSharedRef<FO3DFakeReceiver>& Receiver, const TArray<uint8>& Bytes);
	/** Calls the audio sink with PCM16 bytes on the calling thread. Returns false without a sink. */
	bool InjectAudio(const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& Pcm16);

	bool HasConsumer() const;
	bool HasControlSink() const;
	/** Control payloads handed to the control sink so far. */
	int32 GetControlDelivered() const { return ControlDelivered.load(); }
	/** Items whose envelope said Control but failed TryGetControlPayload; dropped. */
	int32 GetControlRejected() const { return ControlRejected.load(); }

private:
	bool Deliver(const TArray<uint8>& Bytes);

	TSharedPtr<FO3DFakeLink, ESPMode::ThreadSafe> Link;
	mutable FCriticalSection Mutex;
	TSharedPtr<ISerializedFrameConsumer> Consumer;
	TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
	TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe> ControlSink;
	TArray<TArray<uint8>> Queued;
	FString StreamId;
	FO3DTransportStats Stats;
	bool bInitialized = false;
	std::atomic<bool> bRunning{false};
	std::atomic<int32> ControlDelivered{0};
	std::atomic<int32> ControlRejected{0};
	FO3DConnectionStateTracker ConnectionState;
};

/**
 * Registers a linked fake sender and receiver with FO3DTransportRegistry (one descriptor) under
 * O3DTestFake_<Guid> for the lifetime of the scope, and unregisters them in the destructor.
 * Game thread.
 */
class OPEN3DBROADCASTTESTS_API FO3DFakeTransportScope
{
public:
	FO3DFakeTransportScope();
	~FO3DFakeTransportScope();

	FO3DFakeTransportScope(const FO3DFakeTransportScope&) = delete;
	FO3DFakeTransportScope& operator=(const FO3DFakeTransportScope&) = delete;

	FName GetName() const { return Name; }
	FO3DFakeLinkRef GetLink() const { return Link; }
	/** The most recent instance the registry created under this name, if it is still alive. */
	TSharedPtr<FO3DFakeSender> GetLastSender() const;
	TSharedPtr<FO3DFakeReceiver> GetLastReceiver() const;

	/** Prefix shared by every fake transport name. The conformance suite skips these names. */
	static const TCHAR* GetNamePrefix() { return TEXT("O3DTestFake"); }

private:
	struct FCreated
	{
		FCriticalSection Mutex;
		TWeakPtr<FO3DFakeSender> Sender;
		TWeakPtr<FO3DFakeReceiver> Receiver;
	};

	FName Name;
	FO3DFakeLinkRef Link;
	TSharedRef<FCreated, ESPMode::ThreadSafe> Created;
	FO3DTransportRegistration Registration;
};

#endif // WITH_DEV_AUTOMATION_TESTS
