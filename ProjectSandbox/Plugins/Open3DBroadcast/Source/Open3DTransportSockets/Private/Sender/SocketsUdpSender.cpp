#include "SocketsUdpSender.h"

#include "O3DSenderAudioSinkBase.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "O3DAudioSerialization.h"
#include "O3DUnifiedMessage.h"
#include "O3DTransportTypes.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Logging/LogMacros.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
#include "o3ds/udp_fragment.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogSocketsUdpSender, Log, All);

/**
 * UDP audio sink (WP-S5: TRB-10, TRB-11). Encodes with its own encoders on the calling thread
 * and hands the unified message to the audio worker. Never touches the socket or the sender.
 */
class FSocketsUdpSenderAudioSink final : public FO3DGatedSenderAudioSink
{
public:
	FSocketsUdpSenderAudioSink(TSharedRef<FSocketsUdpPublishState, ESPMode::ThreadSafe> InState, FO3DTransportAudioConfig InConfig, FO3DSinkAudioEncoder::FSettings InEncoderSettings)
		: FO3DGatedSenderAudioSink(MoveTemp(InConfig), InState->Gate, MoveTemp(InEncoderSettings))
		, State(MoveTemp(InState))
	{
	}

protected:
	virtual bool OnSubmitGated(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
	{
		if (!State->bSocketReady.load())
		{
			return false;
		}

		TArray<uint8> Unified;
		if (!GetEncoder().EncodeUnified(StreamLabel, State->LastSubject.Get(), Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Unified))
		{
			return false;
		}

		const int64 Size = Unified.Num();
		if (!State->AudioQueue.Enqueue(MoveTemp(Unified)))
		{
			return false;
		}
		State->AudioBytesQueued.fetch_add(Size);
		return true;
	}

private:
	TSharedRef<FSocketsUdpPublishState, ESPMode::ThreadSafe> State;
};

/** Audio send worker. Lifetime is nested inside the sender's: joined in Stop() and the destructor. */
class FO3DSocketsUdpSender::FUdpAudioRunnable final : public FRunnable
{
public:
	explicit FUdpAudioRunnable(FO3DSocketsUdpSender& InOwner)
		: Owner(InOwner)
	{
	}

	virtual uint32 Run() override
	{
		return Owner.RunAudioWorker();
	}

private:
	FO3DSocketsUdpSender& Owner;
};

FO3DSocketsUdpSender::FO3DSocketsUdpSender()
	: PublishState(MakeShared<FSocketsUdpPublishState, ESPMode::ThreadSafe>())
{
}

FO3DSocketsUdpSender::~FO3DSocketsUdpSender()
{
	// Stop() closes the audio gate first (waiting for in-flight submits), then joins the
	// audio worker, then destroys the socket.
	Stop();
}

bool FO3DSocketsUdpSender::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	{
		FScopeLock Lock(&StatsMutex);
		Stats.Reset();
	}
	RemoteHost.Reset();
	RemotePort = 0;
	StreamId = ActiveConfig.StreamId;
	RemoteAddr.Reset();
	PublishState->LastSubject.Reset();
	PublishState->AudioBytesQueued.store(0);

	ActiveAudioConfig = Config.Audio;
	AudioSourceGuid = FGuid::NewGuid();
	SerializationScratch.clear();
	SerializationScratch.reserve(512 * 1024);
	FragmentScratch.clear();

	if (!O3DSockets::ParseHostPort(Config, RemoteHost, RemotePort, TEXT("udp")))
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender requires udp://host:port URI or explicit host/port options."));
		return false;
	}

	if (RemotePort <= 0)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender requires a valid port (got %d)."), RemotePort);
		return false;
	}

	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(RemoteHost, RemotePort);
		ActiveConfig.StreamId = StreamId;
	}

	bAllowBroadcast = O3DSockets::GetBoolOption(Config, O3DSockets::BroadcastOptionKey, false);
	MaxDatagramBytes = FMath::Clamp(O3DSockets::GetIntOption(Config, O3DSockets::MaxDatagramOptionKey, 64000), 512, 65507);
	MtuBytes = O3DSockets::GetIntOption(Config, O3DSockets::MtuOptionKey, 1200);
	MtuBytes = FMath::Clamp(MtuBytes, 256, MaxDatagramBytes);
	FragmentScratch.reserve(MaxDatagramBytes);

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender could not access socket subsystem."));
		return false;
	}

	if (!ResolveAddress(RemoteHost, RemotePort, RemoteAddr))
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender invalid host '%s'."), *RemoteHost);
		return false;
	}

	// Note: Audio stream label is now automatically derived from StreamId

	PublishState->Gate->Open();

	return true;
}

bool FO3DSocketsUdpSender::Start()
{
	DestroySocket();
	// Stop() drops SocketSubsystem; fetch it again so Start() after Stop() works without
	// Initialize(), as it does for TCP (TRB-13; WP-T2 conformance Lifecycle.RestartAfterStop).
	if (!SocketSubsystem && RemoteAddr.IsValid())
	{
		SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}
	PublishState->Gate->Open();
	if (!CreateSocket())
	{
		return false;
	}
	StartAudioWorker();
	return true;
}

void FO3DSocketsUdpSender::Stop()
{
	// WP-S5 ordering: close the audio gate (waits for in-flight submits), join the audio
	// worker, destroy the socket, then drop anything still queued.
	PublishState->Gate->Close();
	StopAudioWorker();
	DestroySocket();
	PublishState->AudioQueue.Empty();
	SocketSubsystem = nullptr;
}

void FO3DSocketsUdpSender::StartAudioWorker()
{
	if (AudioWorkerThread)
	{
		return;
	}
	bStopAudioWorker.store(false);
	AudioWorker = new FUdpAudioRunnable(*this);
	AudioWorkerThread = FRunnableThread::Create(AudioWorker, TEXT("O3D_UDP_Audio_Worker"));
	if (!AudioWorkerThread)
	{
		delete AudioWorker;
		AudioWorker = nullptr;
	}
}

void FO3DSocketsUdpSender::StopAudioWorker()
{
	bStopAudioWorker.store(true);
	PublishState->AudioQueue.Wake();
	if (AudioWorkerThread)
	{
		AudioWorkerThread->WaitForCompletion();
		delete AudioWorkerThread;
		AudioWorkerThread = nullptr;
	}
	delete AudioWorker;
	AudioWorker = nullptr;
}

uint32 FO3DSocketsUdpSender::RunAudioWorker()
{
	TArray<uint8> Bytes;
	while (!bStopAudioWorker.load())
	{
		if (!PublishState->AudioQueue.Dequeue(Bytes))
		{
			PublishState->AudioQueue.WaitForWork(50);
			continue;
		}

		FScopeLock Lock(&SocketLock);
		if (Socket && RemoteAddr.IsValid())
		{
			SendPayload(Socket, RemoteAddr, Bytes.GetData(), Bytes.Num(), TEXT("audio"));
		}
	}
	return 0;
}

bool FO3DSocketsUdpSender::Send(const O3DS::SubjectList& List)
{
	// Guards Socket/RemoteAddr against a concurrent CreateSocket()/DestroySocket()
	// from Start()/Stop() (which take the same lock), and against the audio
	// worker's sends. The audio thread itself never takes this lock (WP-S5).
	FScopeLock Lock(&SocketLock);

	if (!Socket || !RemoteAddr.IsValid())
	{
		return false;
	}

	FString ObservedSubject;
	if (!List.mItems.empty() && List.mItems[0])
	{
		ObservedSubject = UTF8_TO_TCHAR(List.mItems[0]->mName.c_str());
	}

	SerializationScratch.clear();
	const double Timestamp = FPlatformTime::Seconds();
	int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(SerializationScratch, Timestamp);
	if (BytesWritten <= 0)
	{
		UE_LOG(LogSocketsUdpSender, Verbose, TEXT("UDP sender failed to serialize SubjectList."));
		{
			FScopeLock StatsLock(&StatsMutex);
			Stats.DroppedFrames++;
		}
		return false;
	}

	if (!ObservedSubject.IsEmpty())
	{
		PublishState->LastSubject.Set(ObservedSubject);
	}

	if (!SendPayload(Socket, RemoteAddr, reinterpret_cast<const uint8*>(SerializationScratch.data()), BytesWritten, TEXT("data")))
	{
		{
			FScopeLock StatsLock(&StatsMutex);
			Stats.DroppedFrames++;
		}
		return false;
	}

	{
		FScopeLock StatsLock(&StatsMutex);
		Stats.FramesSent++;
		Stats.BytesSent += BytesWritten;
	}
	return true;
}

bool FO3DSocketsUdpSender::SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double /*CaptureTimestampSec*/)
{
	// Same SocketLock discipline as Send(SubjectList&) above - guards
	// Socket/RemoteAddr against a concurrent CreateSocket()/DestroySocket()
	// from Start()/Stop(), and against the audio worker's sends.
	FScopeLock Lock(&SocketLock);

	if (!Socket || !RemoteAddr.IsValid() || Len <= 0)
	{
		return false;
	}

	if (!SubjectName.IsEmpty())
	{
		PublishState->LastSubject.Set(SubjectName);
	}

	if (!SendPayload(Socket, RemoteAddr, Data, Len, TEXT("data")))
	{
		FScopeLock StatsLock(&StatsMutex);
		Stats.DroppedFrames++;
		return false;
	}

	{
		FScopeLock StatsLock(&StatsMutex);
		Stats.FramesSent++;
		Stats.BytesSent += Len;
	}
	return true;
}

void FO3DSocketsUdpSender::Tick(float /*DeltaSeconds*/)
{
	// UDP sender currently has no periodic upkeep.
}

FO3DTransportStats FO3DSocketsUdpSender::GetStats() const
{
	FScopeLock Lock(&StatsMutex);
	FO3DTransportStats Copy = Stats;
	Copy.BytesSent += PublishState->AudioBytesQueued.load();
	return Copy;
}

bool FO3DSocketsUdpSender::SupportsAudio() const
{
	return true;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DSocketsUdpSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
	FO3DTransportAudioConfig EffectiveConfig = ActiveAudioConfig;
	if (AudioConfig.bEnableAudio)
	{
		EffectiveConfig = AudioConfig;
	}

	EffectiveConfig.bEnableAudio = true;
	// Note: Audio stream label is now automatically derived from StreamId

	ActiveAudioConfig = EffectiveConfig;

	// Immutable snapshot for this sink's own encoders (TRB-11): nothing reconfigures them later.
	const FString StreamFallback = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	FO3DSinkAudioEncoder::FSettings EncoderSettings;
	EncoderSettings.Config = ActiveAudioConfig;
	EncoderSettings.DefaultStreamLabel = StreamFallback;
	EncoderSettings.DefaultSubject = StreamFallback;
	EncoderSettings.SourceGuid = AudioSourceGuid;

	return MakeShared<FSocketsUdpSenderAudioSink, ESPMode::ThreadSafe>(PublishState, ActiveAudioConfig, MoveTemp(EncoderSettings));
}

bool FO3DSocketsUdpSender::ResolveRemoteAddress(const FString& Host, int32 Port)
{
	return ResolveAddress(Host, Port, RemoteAddr);
}

bool FO3DSocketsUdpSender::ResolveAddress(const FString& Host, int32 Port, TSharedPtr<FInternetAddr>& OutAddr)
{
	if (!SocketSubsystem)
	{
		return false;
	}

	FString EffectiveHost = Host;
	bool bRequestedBroadcast = false;

	if (EffectiveHost.IsEmpty() || EffectiveHost == TEXT("*"))
	{
		EffectiveHost = TEXT("255.255.255.255");
		bRequestedBroadcast = true;
	}
	else if (EffectiveHost.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
	{
		EffectiveHost = TEXT("127.0.0.1");
	}

	TSharedPtr<FInternetAddr> Candidate = SocketSubsystem->CreateInternetAddr();
	if (!Candidate.IsValid())
	{
		return false;
	}

	bool bIsValid = false;
	Candidate->SetIp(*EffectiveHost, bIsValid);
	if (!bIsValid)
	{
		FIPv4Address IPv4;
		if (FIPv4Address::Parse(EffectiveHost, IPv4))
		{
			Candidate->SetIp(IPv4.Value);
			bIsValid = true;
		}
	}

	if (!bIsValid)
	{
		OutAddr.Reset();
		return false;
	}

	Candidate->SetPort(Port);
	OutAddr = Candidate;

	if (bRequestedBroadcast)
	{
		bAllowBroadcast = true;
	}

	return true;
}

bool FO3DSocketsUdpSender::CreateSocket()
{
	// Same lock as Send()/DestroySocket(); FCriticalSection is recursive in
	// UE so the DestroySocket() call below re-entering the lock on this
	// thread is safe.
	FScopeLock Lock(&SocketLock);

	if (!SocketSubsystem || !RemoteAddr.IsValid())
	{
		return false;
	}

	DestroySocket();

	Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UDP_SENDER"), RemoteAddr->GetProtocolType());
	if (!Socket)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("Failed to create UDP socket."));
		return false;
	}

	Socket->SetReuseAddr(true);
	Socket->SetNonBlocking(true);

	if (bAllowBroadcast)
	{
		Socket->SetBroadcast(true);
	}

	int32 RequestedSize = 2 * 1024 * 1024;
	int32 AppliedSize = 0;
	Socket->SetSendBufferSize(RequestedSize, AppliedSize);

	UE_LOG(LogSocketsUdpSender, Log, TEXT("UDP sender targeting %s:%d (broadcast=%d, maxDatagram=%d, mtu=%d, sendBuf=%d)."),
		*RemoteAddr->ToString(false), RemoteAddr->GetPort(), bAllowBroadcast ? 1 : 0, MaxDatagramBytes, MtuBytes, AppliedSize);

	PublishState->bSocketReady.store(true);
	return true;
}

void FO3DSocketsUdpSender::DestroySocket()
{
	// Same lock as Send()/CreateSocket(); recursive-safe when called from
	// CreateSocket() above, and blocks until any in-flight audio-worker send
	// has finished reading Socket.
	FScopeLock Lock(&SocketLock);
	PublishState->bSocketReady.store(false);

	if (Socket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(Socket);
	}
	Socket = nullptr;
}

bool FO3DSocketsUdpSender::SendPayload(FSocket* InSocket, const TSharedPtr<FInternetAddr>& InAddr, const uint8* Data, int32 Size, const TCHAR* Context)
{
	if (!InSocket || !InAddr.IsValid())
	{
		return false;
	}

	if (Size <= MaxDatagramBytes)
	{
		return SendDatagram(InSocket, InAddr, Data, Size, Context);
	}

	return SendFragmented(InSocket, InAddr, Data, Size, Context);
}

bool FO3DSocketsUdpSender::SendDatagram(FSocket* InSocket, const TSharedPtr<FInternetAddr>& InAddr, const uint8* Data, int32 Size, const TCHAR* Context)
{
	if (!InSocket || !InAddr.IsValid() || Data == nullptr || Size <= 0)
	{
		return false;
	}

	int32 BytesSent = 0;
	if (!InSocket->SendTo(Data, Size, BytesSent, *InAddr))
	{
		const ESocketErrors Error = SocketSubsystem ? SocketSubsystem->GetLastErrorCode() : SE_NO_ERROR;
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP %s send failed (size=%d, error=%d)."), Context ? Context : TEXT("data"), Size, static_cast<int32>(Error));
		return false;
	}

	if (BytesSent != Size)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP %s partial send (requested=%d, sent=%d)."), Context ? Context : TEXT("data"), Size, BytesSent);
		return false;
	}

	return true;
}

bool FO3DSocketsUdpSender::SendFragmented(FSocket* InSocket, const TSharedPtr<FInternetAddr>& InAddr, const uint8* Data, int32 Size, const TCHAR* Context)
{
	if (!InSocket || !InAddr.IsValid() || Data == nullptr || Size <= 0)
	{
		return false;
	}

	constexpr int32 FragmentHeaderSize = 16;
	const int32 FragmentPayload = FMath::Clamp(MtuBytes - FragmentHeaderSize, 256, MaxDatagramBytes);
	if (FragmentPayload <= 0)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP fragmentation disabled due to invalid MTU (%d)."), MtuBytes);
		return false;
	}

	UdpFragmenter Fragmenter(reinterpret_cast<const char*>(Data), static_cast<size_t>(Size), static_cast<size_t>(FragmentPayload));
	const uint32 MessageId = static_cast<uint32>(MessageCounter.Increment());

	for (uint32 Seq = 0; Seq < static_cast<uint32>(Fragmenter.mFrames); ++Seq)
	{
		FragmentScratch.clear();
		Fragmenter.makeFragment(MessageId, Seq, FragmentScratch);
		if (!SendDatagram(InSocket, InAddr, reinterpret_cast<const uint8*>(FragmentScratch.data()), static_cast<int32>(FragmentScratch.size()), Context))
		{
			UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP %s fragment send failed (seq=%u/%llu)."), Context ? Context : TEXT("data"), Seq, static_cast<unsigned long long>(Fragmenter.mFrames));
			return false;
		}
	}

	return true;
}
