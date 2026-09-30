// Copyright (c) Open3DStream Contributors

#include "O3DTestFakes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/ScopeLock.h"
#include "O3DTestHarness.h"

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

// ── FO3DFakeSender ───────────────────────────────────────────────────────────────────────

FO3DFakeSender::FO3DFakeSender(TSharedPtr<FO3DFakeLink, ESPMode::ThreadSafe> InLink)
	: Link(MoveTemp(InLink))
{
}

bool FO3DFakeSender::Initialize(const FO3DTransportConfig& Config)
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
	return true;
}

bool FO3DFakeSender::Start()
{
	StartCalls.fetch_add(1);
	FScopeLock Lock(&Mutex);
	if (!bInitialized)
	{
		return false;
	}
	bRunning.store(true);
	return true;
}

void FO3DFakeSender::Stop()
{
	StopCalls.fetch_add(1);
	bRunning.store(false);
}

bool FO3DFakeSender::Send(const O3DS::SubjectList& List)
{
	std::vector<char> Buffer;
	const int32 Bytes = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, 0.0);
	if (Bytes <= 0)
	{
		return false;
	}
	return SendSerialized(reinterpret_cast<const uint8*>(Buffer.data()), Bytes, FString(), 0.0);
}

bool FO3DFakeSender::SendSerialized(const uint8* Data, int32 Len, const FString& /*SubjectName*/, double /*CaptureTimestampSec*/)
{
	SendCalls.fetch_add(1);
	if (!bRunning.load() || Data == nullptr || Len <= 0)
	{
		return false;
	}

	TArray<uint8> Payload(Data, Len);
	{
		FScopeLock Lock(&Mutex);
		if (MaxQueued >= 0 && Queued >= MaxQueued)
		{
			++Stats.DroppedFrames;
			return false;
		}
		++Queued;
		++Stats.FramesSent;
		Stats.BytesSent += Len;
		Recorded.Add(Payload);
	}

	if (Link.IsValid())
	{
		Link->Push(Payload);
	}
	return true;
}

void FO3DFakeSender::Tick(float /*DeltaSeconds*/)
{
}

FO3DTransportStats FO3DFakeSender::GetStats() const
{
	FScopeLock Lock(&Mutex);
	return Stats;
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

bool FO3DFakeReceiver::Initialize(const FO3DTransportConfig& Config)
{
	FScopeLock Lock(&Mutex);
	StreamId = Config.StreamId;
	Stats.Reset();
	Queued.Reset();
	bInitialized = true;
	return true;
}

void FO3DFakeReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	FScopeLock Lock(&Mutex);
	Consumer = InConsumer;
}

bool FO3DFakeReceiver::Start()
{
	FScopeLock Lock(&Mutex);
	if (!bInitialized)
	{
		return false;
	}
	bRunning.store(true);
	return true;
}

void FO3DFakeReceiver::Stop()
{
	bRunning.store(false);
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
	FScopeLock Lock(&Mutex);
	return Stats;
}

void FO3DFakeReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& /*AudioConfig*/)
{
	FScopeLock Lock(&Mutex);
	AudioSink = Sink;
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
