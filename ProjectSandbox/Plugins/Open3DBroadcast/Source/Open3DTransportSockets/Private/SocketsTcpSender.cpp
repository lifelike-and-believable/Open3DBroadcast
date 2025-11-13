#include "SocketsTcpSender.h"
#include "SocketsTcpAudio.h"
#include "SocketsTcpTransport.h"
#include "O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Logging/LogMacros.h"

#include "o3ds/model.h"

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogSocketsTcpSender, Log, All);

class FSocketsTcpSenderAudioSink final : public IO3DSenderAudioSink
{
public:
	FSocketsTcpSenderAudioSink(FO3DSocketsTcpSender& InOwner, FO3DTransportAudioConfig InConfig)
		: Owner(InOwner)
		, AudioConfig(MoveTemp(InConfig))
	{
	}

	virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
	{
		if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
		{
			return false;
		}

		FString EffectiveLabel = StreamLabel;
		if (EffectiveLabel.IsEmpty())
		{
			EffectiveLabel = AudioConfig.StreamLabel;
		}

		return Owner.ProcessCapturedAudio(EffectiveLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
	}

private:
	FO3DSocketsTcpSender& Owner;
	FO3DTransportAudioConfig AudioConfig;
};

FO3DSocketsTcpSender::FO3DSocketsTcpSender() = default;

FO3DSocketsTcpSender::~FO3DSocketsTcpSender()
{
	Stop();
}

bool FO3DSocketsTcpSender::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	Stats.Reset();
	StreamId = ActiveConfig.StreamId;

	ActiveAudioConfig = Config.Audio;
	if (ActiveAudioConfig.StreamLabel.IsEmpty())
	{
		ActiveAudioConfig.StreamLabel = Config.StreamId;
	}
	AudioSourceGuid = FGuid::NewGuid();

	// Parse bind address from config
	if (!O3DSockets::ParseHostPort(Config, BindHost, BindPort, TEXT("tcp")))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender requires tcp://host:port URI or explicit host/port options."));
		return false;
	}

	if (BindPort <= 0)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender requires a valid port (got %d)."), BindPort);
		return false;
	}

	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(BindHost, BindPort);
		ActiveConfig.StreamId = StreamId;
	}

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender could not access socket subsystem."));
		return false;
	}

	RefreshAudioEncoder();

	return true;
}

bool FO3DSocketsTcpSender::Start()
{
	DestroySocket();
	return CreateListenSocket();
}

void FO3DSocketsTcpSender::Stop()
{
	DestroySocket();
	SocketSubsystem = nullptr;
}

bool FO3DSocketsTcpSender::Send(const O3DS::SubjectList& List)
{
	if (!ClientSocket)
	{
		// No client connected yet
		return false;
	}

	std::vector<char> Buffer;
	const double Timestamp = FPlatformTime::Seconds();
	int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, Timestamp);
	if (BytesWritten <= 0)
	{
		UE_LOG(LogSocketsTcpSender, Verbose, TEXT("TCP sender failed to serialize SubjectList."));
		Stats.DroppedFrames++;
		return false;
	}

	if (!SendFramed(ClientSocket, reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten))
	{
		// Send failed - drop client and wait for reconnect
		UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP send failed, dropping client."));
		if (ClientSocket && SocketSubsystem)
		{
			SocketSubsystem->DestroySocket(ClientSocket);
		}
		ClientSocket = nullptr;
		Stats.DroppedFrames++;
		return false;
	}

	Stats.FramesSent++;
	Stats.BytesSent += BytesWritten;
	return true;
}

void FO3DSocketsTcpSender::Tick(float /*DeltaSeconds*/)
{
	TickAcceptClient();
}

FO3DTransportStats FO3DSocketsTcpSender::GetStats() const
{
	return Stats;
}

bool FO3DSocketsTcpSender::SupportsAudio() const
{
	return true;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DSocketsTcpSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
	FO3DTransportAudioConfig EffectiveConfig = ActiveAudioConfig;
	if (AudioConfig.bEnableAudio)
	{
		EffectiveConfig = AudioConfig;
	}

	EffectiveConfig.bEnableAudio = true;
	EffectiveConfig.NumChannels = FMath::Max(EffectiveConfig.NumChannels, 1);
	EffectiveConfig.SampleRate = FMath::Max(EffectiveConfig.SampleRate, 1);
	if (EffectiveConfig.StreamLabel.IsEmpty())
	{
		EffectiveConfig.StreamLabel = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	}

	ActiveAudioConfig = EffectiveConfig;
	RefreshAudioEncoder();

	return MakeShared<FSocketsTcpSenderAudioSink>(*this, ActiveAudioConfig);
}

bool FO3DSocketsTcpSender::CreateListenSocket()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	bool bValid = false;
	TSharedPtr<FInternetAddr> BindAddr = CreateBindAddress(BindHost, BindPort, bValid);
	if (!bValid || !BindAddr.IsValid())
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Invalid bind address %s:%d"), *BindHost, BindPort);
		return false;
	}

	ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_LISTEN"), BindAddr->GetProtocolType());
	if (!ListenSocket)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Failed to create listen socket."));
		return false;
	}

	ListenSocket->SetReuseAddr(true);
	ListenSocket->SetNonBlocking(true);

	if (!ListenSocket->Bind(*BindAddr))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Bind failed on %s"), *BindAddr->ToString(true));
		DestroySocket();
		return false;
	}

	if (!ListenSocket->Listen(8))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Listen failed on %s"), *BindAddr->ToString(true));
		DestroySocket();
		return false;
	}

	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender listening on %s"), *BindAddr->ToString(true));
	return true;
}

void FO3DSocketsTcpSender::DestroySocket()
{
	if (ClientSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(ClientSocket);
	}
	ClientSocket = nullptr;

	if (ListenSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(ListenSocket);
	}
	ListenSocket = nullptr;
}

void FO3DSocketsTcpSender::TickAcceptClient()
{
	if (!ListenSocket || ClientSocket)
	{
		return; // Already have a client
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastAcceptPollTime < 0.01) // Poll every 10ms
	{
		return;
	}
	LastAcceptPollTime = Now;

	TSharedRef<FInternetAddr> PeerAddr = SocketSubsystem->CreateInternetAddr();
	FSocket* Accepted = ListenSocket->Accept(*PeerAddr, TEXT("O3DS_TCP_CLIENT"));
	if (Accepted)
	{
		Accepted->SetNonBlocking(true);
		ClientSocket = Accepted;
		UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender accepted client %s"), *PeerAddr->ToString(true));
	}
}

bool FO3DSocketsTcpSender::SendFramed(FSocket* InSocket, const uint8* Data, int32 Size)
{
	if (!InSocket || !Data || Size <= 0)
	{
		return false;
	}

	// Send header
	uint8 Header[O3DSockets::Tcp::FrameHeaderSize];
	O3DSockets::Tcp::WriteFrameHeader(Header, Size);

	int32 HeaderSent = 0;
	if (!InSocket->Send(Header, O3DSockets::Tcp::FrameHeaderSize, HeaderSent) || HeaderSent != O3DSockets::Tcp::FrameHeaderSize)
	{
		return false;
	}

	// Send payload
	int32 PayloadSent = 0;
	if (!InSocket->Send(Data, Size, PayloadSent) || PayloadSent != Size)
	{
		return false;
	}

	return true;
}

void FO3DSocketsTcpSender::RefreshAudioEncoder()
{
	const FString StreamLabelFallback = ActiveAudioConfig.StreamLabel.IsEmpty()
		? (ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId)
		: ActiveAudioConfig.StreamLabel;
	const FString SubjectFallback = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;

	bAudioEncoderInitialized = AudioEncoder.Initialize(ActiveAudioConfig, StreamLabelFallback, SubjectFallback);
}

bool FO3DSocketsTcpSender::ProcessCapturedAudio(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	if (!bAudioEncoderInitialized || !ClientSocket)
	{
		return false;
	}

	const FString SubjectForAudio = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;

	O3DAudio::FEncodedFrame Frame;
	if (!AudioEncoder.BuildEncodedFrame(StreamLabel, SubjectForAudio, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Frame))
	{
		return false;
	}

	if (Frame.Meta.SubjectName.IsEmpty())
	{
		Frame.Meta.SubjectName = SubjectForAudio;
	}
	Frame.Meta.SourceGuid = AudioSourceGuid;

	return SendEncodedAudio(Frame, TimestampSec);
}

bool FO3DSocketsTcpSender::SendEncodedAudio(const O3DAudio::FEncodedFrame& Frame, double TimestampSec)
{
	if (!ClientSocket || Frame.Encoded.Num() <= 0)
	{
		return false;
	}

	if (!O3DAudio::CreateUnifiedAudioMessage(Frame, TimestampSec, UnifiedAudioScratch))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender failed to create unified audio message"));
		return false;
	}

	if (!SendFramed(ClientSocket, UnifiedAudioScratch.GetData(), UnifiedAudioScratch.Num()))
	{
		UE_LOG(LogSocketsTcpSender, Verbose, TEXT("TCP sender failed to send audio frame"));
		return false;
	}

	Stats.BytesSent += UnifiedAudioScratch.Num();
	return true;
}

TSharedPtr<FInternetAddr> FO3DSocketsTcpSender::CreateBindAddress(const FString& Host, int32 Port, bool& bOutValid)
{
	bOutValid = false;
	if (!SocketSubsystem)
	{
		return nullptr;
	}

	TSharedPtr<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	if (!Addr.IsValid())
	{
		return nullptr;
	}

	FString HostToUse = Host;
	if (HostToUse.IsEmpty() || HostToUse == TEXT("*") || HostToUse == TEXT("0.0.0.0"))
	{
		HostToUse = TEXT("0.0.0.0");
	}

	Addr->SetPort(Port);
	Addr->SetIp(*HostToUse, bOutValid);
	return Addr;
}
