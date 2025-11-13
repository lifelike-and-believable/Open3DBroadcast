#include "SocketsUdpSender.h"

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

#include "o3ds/model.h"
#include "o3ds/udp_fragment.h"

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogSocketsUdpSender, Log, All);

class FSocketsUdpSenderAudioSink final : public IO3DSenderAudioSink
{
public:
	FSocketsUdpSenderAudioSink(FO3DSocketsUdpSender& InOwner, FO3DTransportAudioConfig InConfig)
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
	FO3DSocketsUdpSender& Owner;
	FO3DTransportAudioConfig AudioConfig;
};

FO3DSocketsUdpSender::FO3DSocketsUdpSender() = default;

FO3DSocketsUdpSender::~FO3DSocketsUdpSender()
{
	Stop();
}

bool FO3DSocketsUdpSender::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	Stats.Reset();
	RemoteHost.Reset();
	RemotePort = 0;
	StreamId = ActiveConfig.StreamId;
	RemoteAddr.Reset();
	{
		FScopeLock Lock(&SubjectNameLock);
		LastSubjectName.Reset();
	}

	ActiveAudioConfig = Config.Audio;
	AudioSourceGuid = FGuid::NewGuid();

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

	if (ActiveAudioConfig.StreamLabel.IsEmpty())
	{
		ActiveAudioConfig.StreamLabel = ActiveConfig.StreamId;
	}

	RefreshAudioEncoder();

	return true;
}

bool FO3DSocketsUdpSender::Start()
{
	DestroySocket();
	return CreateSocket();
}

void FO3DSocketsUdpSender::Stop()
{
	DestroySocket();
	SocketSubsystem = nullptr;
}

bool FO3DSocketsUdpSender::Send(const O3DS::SubjectList& List)
{
	if (!Socket || !RemoteAddr.IsValid())
	{
		return false;
	}

	FString ObservedSubject;
	if (!List.mItems.empty() && List.mItems[0])
	{
		ObservedSubject = UTF8_TO_TCHAR(List.mItems[0]->mName.c_str());
	}

	std::vector<char> Buffer;
	const double Timestamp = FPlatformTime::Seconds();
	int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, Timestamp);
	if (BytesWritten <= 0)
	{
		UE_LOG(LogSocketsUdpSender, Verbose, TEXT("UDP sender failed to serialize SubjectList."));
		Stats.DroppedFrames++;
		return false;
	}

	if (!ObservedSubject.IsEmpty())
	{
		FScopeLock Lock(&SubjectNameLock);
		LastSubjectName = MoveTemp(ObservedSubject);
	}

	if (!SendPayload(Socket, RemoteAddr, reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten, TEXT("data")))
	{
		Stats.DroppedFrames++;
		return false;
	}

	Stats.FramesSent++;
	Stats.BytesSent += BytesWritten;
	return true;
}

void FO3DSocketsUdpSender::Tick(float /*DeltaSeconds*/)
{
	// UDP sender currently has no periodic upkeep.
}

FO3DTransportStats FO3DSocketsUdpSender::GetStats() const
{
	return Stats;
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
	if (EffectiveConfig.StreamLabel.IsEmpty())
	{
		EffectiveConfig.StreamLabel = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	}

	ActiveAudioConfig = EffectiveConfig;
	RefreshAudioEncoder();

	return MakeShared<FSocketsUdpSenderAudioSink, ESPMode::ThreadSafe>(*this, ActiveAudioConfig);
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

	return true;
}

void FO3DSocketsUdpSender::DestroySocket()
{
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
		std::vector<char> Out;
		Fragmenter.makeFragment(MessageId, Seq, Out);
		if (!SendDatagram(InSocket, InAddr, reinterpret_cast<const uint8*>(Out.data()), static_cast<int32>(Out.size()), Context))
		{
			UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP %s fragment send failed (seq=%u/%llu)."), Context ? Context : TEXT("data"), Seq, static_cast<unsigned long long>(Fragmenter.mFrames));
			return false;
		}
	}

	return true;
}

void FO3DSocketsUdpSender::RefreshAudioEncoder()
{
	const FString StreamLabelFallback = ActiveAudioConfig.StreamLabel.IsEmpty()
		? (ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId)
		: ActiveAudioConfig.StreamLabel;
	const FString SubjectFallback = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;

	bAudioEncoderInitialized = AudioEncoder.Initialize(ActiveAudioConfig, StreamLabelFallback, SubjectFallback);
}

bool FO3DSocketsUdpSender::ProcessCapturedAudio(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	if (!bAudioEncoderInitialized || !Socket || !RemoteAddr.IsValid())
	{
		return false;
	}

	FString SubjectForAudio;
	{
		FScopeLock Lock(&SubjectNameLock);
		SubjectForAudio = LastSubjectName;
	}
	if (SubjectForAudio.IsEmpty())
	{
		SubjectForAudio = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	}

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

bool FO3DSocketsUdpSender::SendEncodedAudio(const O3DAudio::FEncodedFrame& Frame, double TimestampSec)
{
	if (Frame.Encoded.Num() <= 0)
	{
		return false;
	}

	if (!O3DAudio::CreateUnifiedAudioMessage(Frame, TimestampSec, UnifiedAudioScratch))
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender failed to create unified audio message"));
		return false;
	}

	if (!SendPayload(Socket, RemoteAddr, UnifiedAudioScratch.GetData(), UnifiedAudioScratch.Num(), TEXT("audio")))
	{
		return false;
	}

	Stats.BytesSent += UnifiedAudioScratch.Num();
	return true;
}
