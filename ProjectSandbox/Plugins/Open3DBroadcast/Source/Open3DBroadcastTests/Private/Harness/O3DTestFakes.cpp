// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#include "O3DTestFakes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "O3DTestHarness.h"
#include "O3DUnifiedMessage.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

// ── FO3DFakeLink ─────────────────────────────────────────────────────────────────────────

void FO3DFakeLink::Push(const TArray<uint8>& Bytes)
{
	FScopeLock Lock(&Mutex);
	Pending.Add(Bytes);
}

TArray<TArray<uint8>> FO3DFakeLink::Drain()
{
	FScopeLock Lock(&Mutex);
	TArray<TArray<uint8>> Out = MoveTemp(Pending);
	Pending.Reset();
	return Out;
}

FO3DTransportCapabilities GetFakeTransportCapabilities(const FO3DTransportConfig* Config)
{
	FO3DTransportCapabilities Caps;
	Caps.bSend = true;
	Caps.bReceive = true;
	Caps.bAudioSend = false;
	Caps.bAudioReceive = true;
	Caps.bControl = true;
	Caps.Delivery = EO3DDeliveryGuarantee::ReliableOrdered;
	if (Config != nullptr)
	{
		const FString* Delivery = Config->AdvancedParams.Find(TEXT("fake.delivery"));
		if (Delivery != nullptr && *Delivery == TEXT("unreliable"))
		{
			Caps.Delivery = EO3DDeliveryGuarantee::Unreliable;
		}
	}
	return Caps;
}

// ── FO3DFakeSender ───────────────────────────────────────────────────────────────────────

FO3DTransportCapabilities FO3DFakeSender::GetCapabilities() const
{
	const FO3DTransportConfig Config = GetLastConfig();
	return GetFakeTransportCapabilities(&Config);
}

FO3DFakeSender::FO3DFakeSender(TSharedPtr<FO3DFakeLink, ESPMode::ThreadSafe> InLink)
	: Link(MoveTemp(InLink))
{
}

FO3DTransportResult FO3DFakeSender::Initialize(const FO3DTransportConfig& Config)
{
	FScopeLock Lock(&Mutex);
	LastConfig = Config;
	Stats.Reset();
	Recorded.Reset();
	Queued = 0;
	if (const FString* MaxQueuedValue = Config.AdvancedParams.Find(TEXT("fake.maxqueued")))
	{
		MaxQueued = FCString::Atoi(**MaxQueuedValue);
	}
	bInitialized = true;
	return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DFakeSender::Start()
{
	StartCalls.fetch_add(1);
	{
		FScopeLock Lock(&Mutex);
		if (!bInitialized)
		{
			return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("Fake sender Start() before Initialize()."));
		}
		bRunning.store(true);
	}
	// Outside Mutex: the state callback must not run under a lock the sender's callers can take.
	ConnectionState.Begin(EO3DConnectionState::Connected);
	return FO3DTransportResult::Ok();
}

void FO3DFakeSender::Stop()
{
	StopCalls.fetch_add(1);
	bRunning.store(false);
	ConnectionState.End(EO3DConnectionState::Idle);
}

EO3DSendResult FO3DFakeSender::SendSerialized(FO3DSendPayload&& Payload)
{
	SendCalls.fetch_add(1);
	if (!bRunning.load())
	{
		return EO3DSendResult::NotRunning;
	}
	const int32 Len = Payload.Bytes.Num();
	if (Len <= 0)
	{
		return EO3DSendResult::Invalid;
	}

	{
		FScopeLock Lock(&Mutex);
		if (MaxQueued >= 0 && Queued >= MaxQueued)
		{
			++Stats.DroppedFrames;
			return EO3DSendResult::DroppedBackpressure;
		}
		++Queued;
		++Stats.FramesSent;
		Stats.BytesSent += Len;
		Recorded.Add(Payload.Bytes);
	}

	if (Link.IsValid())
	{
		Link->Push(Payload.Bytes);
	}
	return EO3DSendResult::Queued;
}

EO3DSendResult FO3DFakeSender::SendControl(const uint8* Envelope, int32 Len)
{
	ControlCalls.fetch_add(1);
	if (!bRunning.load())
	{
		return EO3DSendResult::NotRunning;
	}
	TConstArrayView<uint8> Payload;
	if (Envelope == nullptr || Len <= 0 || !O3DS::TryGetControlPayload(Envelope, Len, Payload))
	{
		return EO3DSendResult::Invalid;
	}

	TArray<uint8> Bytes(Envelope, Len);
	{
		FScopeLock Lock(&Mutex);
		if (MaxQueued >= 0 && Queued >= MaxQueued)
		{
			return EO3DSendResult::DroppedBackpressure; // the caller retries; control is never counted as a dropped frame
		}
		++Queued;
		RecordedControl.Add(Bytes);
	}

	if (Link.IsValid())
	{
		Link->Push(Bytes); // in-band, as TCP, UDP and NNG carry it
	}
	return EO3DSendResult::Queued;
}

void FO3DFakeSender::Tick(float /*DeltaSeconds*/)
{
}

FO3DTransportStats FO3DFakeSender::GetStats() const
{
	FO3DTransportStats Copy;
	{
		FScopeLock Lock(&Mutex);
		Copy = Stats;
		Copy.PendingFrames = Queued;
	}
	Copy.State = ConnectionState.Get();
	return Copy;
}

void FO3DFakeSender::SetMaxQueued(int32 InMaxQueued)
{
	FScopeLock Lock(&Mutex);
	MaxQueued = InMaxQueued;
}

int32 FO3DFakeSender::DrainQueue()
{
	FScopeLock Lock(&Mutex);
	const int32 Drained = Queued;
	Queued = 0;
	return Drained;
}

TArray<TArray<uint8>> FO3DFakeSender::GetRecordedPayloads() const
{
	FScopeLock Lock(&Mutex);
	return Recorded;
}

TArray<TArray<uint8>> FO3DFakeSender::GetRecordedControl() const
{
	FScopeLock Lock(&Mutex);
	return RecordedControl;
}

FO3DTransportConfig FO3DFakeSender::GetLastConfig() const
{
	FScopeLock Lock(&Mutex);
	return LastConfig;
}

// ── FO3DFakeReceiver ─────────────────────────────────────────────────────────────────────

FO3DFakeReceiver::FO3DFakeReceiver(TSharedPtr<FO3DFakeLink, ESPMode::ThreadSafe> InLink)
	: Link(MoveTemp(InLink))
{
}

FO3DTransportResult FO3DFakeReceiver::Initialize(const FO3DTransportConfig& Config)
{
	FScopeLock Lock(&Mutex);
	StreamId = Config.StreamId;
	Stats.Reset();
	Queued.Reset();
	bInitialized = true;
	return FO3DTransportResult::Ok();
}

void FO3DFakeReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	FScopeLock Lock(&Mutex);
	Consumer = InConsumer;
}

FO3DTransportResult FO3DFakeReceiver::Start()
{
	{
		FScopeLock Lock(&Mutex);
		if (!bInitialized)
		{
			return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("Fake receiver Start() before Initialize()."));
		}
		if (!Consumer.IsValid())
		{
			return FO3DTransportResult::Error(EO3DTransportError::NoConsumer, TEXT("Fake receiver Start() without a frame consumer."));
		}
		bRunning.store(true);
	}
	ConnectionState.Begin(EO3DConnectionState::Connected);
	return FO3DTransportResult::Ok();
}

void FO3DFakeReceiver::Stop()
{
	bRunning.store(false);
	{
		FScopeLock Lock(&Mutex);
		ControlSink.Reset(); // the interface contract: released in Stop
	}
	ConnectionState.End(EO3DConnectionState::Idle);
}

int32 FO3DFakeReceiver::Poll()
{
	if (!bRunning.load())
	{
		return 0;
	}

	TArray<TArray<uint8>> Frames;
	{
		FScopeLock Lock(&Mutex);
		Frames = MoveTemp(Queued);
		Queued.Reset();
	}
	if (Link.IsValid())
	{
		Frames.Append(Link->Drain());
	}

	int32 Delivered = 0;
	for (const TArray<uint8>& Frame : Frames)
	{
		Delivered += Deliver(Frame) ? 1 : 0;
	}
	return Delivered;
}

FO3DTransportStats FO3DFakeReceiver::GetStats() const
{
	FO3DTransportStats Copy;
	{
		FScopeLock Lock(&Mutex);
		Copy = Stats;
	}
	Copy.State = ConnectionState.Get();
	return Copy;
}

void FO3DFakeReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& /*AudioConfig*/)
{
	FScopeLock Lock(&Mutex);
	AudioSink = Sink;
}

void FO3DFakeReceiver::SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink)
{
	FScopeLock Lock(&Mutex);
	ControlSink = Sink;
}

bool FO3DFakeReceiver::HasControlSink() const
{
	FScopeLock Lock(&Mutex);
	return ControlSink.IsValid();
}

void FO3DFakeReceiver::Enqueue(const TArray<uint8>& Bytes)
{
	FScopeLock Lock(&Mutex);
	Queued.Add(Bytes);
}

bool FO3DFakeReceiver::InjectNow(const TArray<uint8>& Bytes)
{
	return bRunning.load() && Deliver(Bytes);
}

FGraphEventRef FO3DFakeReceiver::InjectOnBackgroundThread(const TSharedRef<FO3DFakeReceiver>& Receiver, const TArray<uint8>& Bytes)
{
	const TWeakPtr<FO3DFakeReceiver> Weak = Receiver;
	return FFunctionGraphTask::CreateAndDispatchWhenReady([Weak, Bytes]()
	{
		if (const TSharedPtr<FO3DFakeReceiver> Pinned = Weak.Pin())
		{
			Pinned->InjectNow(Bytes);
		}
	}, TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask);
}

bool FO3DFakeReceiver::InjectAudio(const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& Pcm16)
{
	TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> Sink;
	{
		FScopeLock Lock(&Mutex);
		Sink = AudioSink;
	}
	if (!Sink.IsValid() || Pcm16.Num() == 0)
	{
		return false;
	}
	Sink->SubmitPcm16(Meta, Pcm16.GetData(), Pcm16.Num());
	return true;
}

bool FO3DFakeReceiver::HasConsumer() const
{
	FScopeLock Lock(&Mutex);
	return Consumer.IsValid();
}

bool FO3DFakeReceiver::Deliver(const TArray<uint8>& Bytes)
{
	// Control first: an envelope of kind Control goes to the control sink when well-formed and is
	// dropped otherwise. It is never a frame.
	O3DS::FUnifiedHeader Header;
	const uint8* EnvelopePayload = nullptr;
	int32 EnvelopePayloadSize = 0;
	if (O3DS::ParseUnifiedMessage(Bytes.GetData(), Bytes.Num(), Header, EnvelopePayload, EnvelopePayloadSize)
		&& Header.GetKind() == O3DS::EUnifiedKind::Control)
	{
		TConstArrayView<uint8> Payload;
		if (!O3DS::TryGetControlPayload(Bytes.GetData(), Bytes.Num(), Payload))
		{
			ControlRejected.fetch_add(1);
			return false;
		}
		TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe> Sink;
		FString Stream;
		{
			FScopeLock Lock(&Mutex);
			Sink = ControlSink;
			Stream = StreamId;
		}
		if (Sink.IsValid())
		{
			Sink->SubmitControl(Payload, Stream, FPlatformTime::Seconds());
			ControlDelivered.fetch_add(1);
		}
		return false;
	}

	TSharedPtr<ISerializedFrameConsumer> Target;
	FString Stream;
	{
		FScopeLock Lock(&Mutex);
		Target = Consumer;
		Stream = StreamId;
		if (Target.IsValid())
		{
			++Stats.FramesReceived;
			Stats.BytesReceived += Bytes.Num();
		}
	}
	if (!Target.IsValid())
	{
		return false;
	}
	Target->SubmitFrame(Stream, Bytes, 0.0);
	return true;
}

// ── FO3DFakeTransportScope ───────────────────────────────────────────────────────────────

FO3DFakeTransportScope::FO3DFakeTransportScope()
	: Name(*O3DTests::MakeUniqueName(GetNamePrefix()))
	, Link(MakeShared<FO3DFakeLink, ESPMode::ThreadSafe>())
	, Created(MakeShared<FCreated, ESPMode::ThreadSafe>())
{
	const FO3DFakeLinkRef LinkForFactories = Link;
	const TSharedRef<FCreated, ESPMode::ThreadSafe> CreatedForFactories = Created;

	// One descriptor with both factories (ADR 0006 §3, ADR 0007 item 4).
	FO3DTransportDescriptor Descriptor;
	Descriptor.Name = Name;
	Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
	Descriptor.GetCapabilities = [](const FO3DTransportConfig& Config) { return GetFakeTransportCapabilities(&Config); };
	Descriptor.CreateSender = [LinkForFactories, CreatedForFactories]() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>
	{
		TSharedRef<FO3DFakeSender> Sender = MakeShared<FO3DFakeSender>(LinkForFactories);
		FScopeLock Lock(&CreatedForFactories->Mutex);
		CreatedForFactories->Sender = Sender;
		return Sender;
	};
	Descriptor.CreateReceiver = [LinkForFactories, CreatedForFactories]() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>
	{
		TSharedRef<FO3DFakeReceiver> Receiver = MakeShared<FO3DFakeReceiver>(LinkForFactories);
		FScopeLock Lock(&CreatedForFactories->Mutex);
		CreatedForFactories->Receiver = Receiver;
		return Receiver;
	};
	Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
}

FO3DFakeTransportScope::~FO3DFakeTransportScope()
{
	Registration.Reset();
}

TSharedPtr<FO3DFakeSender> FO3DFakeTransportScope::GetLastSender() const
{
	FScopeLock Lock(&Created->Mutex);
	return Created->Sender.Pin();
}

TSharedPtr<FO3DFakeReceiver> FO3DFakeTransportScope::GetLastReceiver() const
{
	FScopeLock Lock(&Created->Mutex);
	return Created->Receiver.Pin();
}

#endif // WITH_DEV_AUTOMATION_TESTS
