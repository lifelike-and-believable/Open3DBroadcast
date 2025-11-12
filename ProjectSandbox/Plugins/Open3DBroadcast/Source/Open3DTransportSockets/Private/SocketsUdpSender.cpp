#include "SocketsUdpSender.h"

#include "O3DAudioSerialization.h"
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

		const int32 NumSamples = NumFrames * NumChannels;
		if (NumSamples <= 0)
		{
			return false;
		}

		TArray<uint8> Encoded;
		Encoded.SetNumUninitialized(NumSamples * static_cast<int32>(sizeof(int16)));
		int16* Dest = reinterpret_cast<int16*>(Encoded.GetData());
		for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
		{
			const float Clamped = FMath::Clamp(Interleaved[SampleIndex], -1.0f, 1.0f);
			const int32 Scaled = FMath::RoundToInt(Clamped * 32767.0f);
			Dest[SampleIndex] = static_cast<int16>(FMath::Clamp(Scaled, -32768, 32767));
		}

		FString EffectiveLabel = StreamLabel;
		if (EffectiveLabel.IsEmpty())
		{
			EffectiveLabel = AudioConfig.StreamLabel;
		}

		return Owner.SendAudioFrame(EffectiveLabel, Encoded.GetData(), Encoded.Num(), NumChannels, SampleRate, TimestampSec);
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
	bAudioEnabled = ActiveAudioConfig.bEnableAudio;
	AudioRemoteHost.Reset();
	AudioRemotePort = 0;
	AudioRemoteAddr.Reset();
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

	AudioRemoteHost = O3DSockets::GetOptionValue(Config, O3DSockets::AudioHostOptionKey);
	if (AudioRemoteHost.IsEmpty())
	{
		AudioRemoteHost = RemoteHost;
	}

	AudioRemotePort = O3DSockets::GetIntOption(Config, O3DSockets::AudioPortOptionKey, 0);
	if (AudioRemotePort <= 0 && RemotePort > 0)
	{
		AudioRemotePort = RemotePort + 1;
	}

	if (ActiveAudioConfig.StreamLabel.IsEmpty())
	{
		ActiveAudioConfig.StreamLabel = ActiveConfig.StreamId;
	}

	if (AudioRemotePort > 0)
	{
		if (!ResolveAddress(AudioRemoteHost, AudioRemotePort, AudioRemoteAddr))
		{
			UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender invalid audio host '%s'."), *AudioRemoteHost);
			AudioRemoteAddr.Reset();
			bAudioEnabled = false;
		}
	}
	else
	{
		if (bAudioEnabled)
		{
			UE_LOG(LogSocketsUdpSender, Warning, TEXT("Audio support requested but no valid audio port specified."));
		}
		bAudioEnabled = false;
	}

	return true;
}

bool FO3DSocketsUdpSender::Start()
{
	DestroySocket();
	DestroyAudioSocket();

	const bool bDataStarted = CreateSocket();
	bool bAudioStarted = true;
	if (bAudioEnabled)
	{
		bAudioStarted = CreateAudioSocket();
		if (!bAudioStarted)
		{
			UE_LOG(LogSocketsUdpSender, Warning, TEXT("Failed to initialise UDP audio socket; disabling audio channel."));
			bAudioEnabled = false;
			DestroyAudioSocket();
			bAudioStarted = true;
		}
	}

	return bDataStarted && bAudioStarted;
}

void FO3DSocketsUdpSender::Stop()
{
	DestroySocket();
	DestroyAudioSocket();
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
	bAudioEnabled = true;

	if (AudioRemotePort <= 0)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("CreateAudioSink failed: no valid audio port configured."));
		bAudioEnabled = false;
		return nullptr;
	}

	if (!AudioRemoteAddr.IsValid())
	{
		if (!ResolveAddress(AudioRemoteHost, AudioRemotePort, AudioRemoteAddr))
		{
			UE_LOG(LogSocketsUdpSender, Warning, TEXT("CreateAudioSink failed: invalid audio host '%s'."), *AudioRemoteHost);
			bAudioEnabled = false;
			return nullptr;
		}
	}

	if (SocketSubsystem && Socket && bAudioEnabled && !AudioSocket)
	{
		if (!CreateAudioSocket())
		{
			UE_LOG(LogSocketsUdpSender, Warning, TEXT("CreateAudioSink failed: could not create audio socket."));
			bAudioEnabled = false;
			return nullptr;
		}
	}

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

bool FO3DSocketsUdpSender::CreateAudioSocket()
{
	if (!SocketSubsystem || !AudioRemoteAddr.IsValid())
	{
		return false;
	}

	DestroyAudioSocket();

	AudioSocket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UDP_AUDIO_SENDER"), AudioRemoteAddr->GetProtocolType());
	if (!AudioSocket)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("Failed to create UDP audio socket."));
		return false;
	}

	AudioSocket->SetReuseAddr(true);
	AudioSocket->SetNonBlocking(true);

	if (bAllowBroadcast)
	{
		AudioSocket->SetBroadcast(true);
	}

	int32 RequestedSize = 2 * 1024 * 1024;
	int32 AppliedSize = 0;
	AudioSocket->SetSendBufferSize(RequestedSize, AppliedSize);

	UE_LOG(LogSocketsUdpSender, Log, TEXT("UDP audio sender targeting %s:%d (broadcast=%d, sendBuf=%d)."),
		*AudioRemoteAddr->ToString(false), AudioRemoteAddr->GetPort(), bAllowBroadcast ? 1 : 0, AppliedSize);

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

void FO3DSocketsUdpSender::DestroyAudioSocket()
{
	if (AudioSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(AudioSocket);
	}
	AudioSocket = nullptr;
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

bool FO3DSocketsUdpSender::SendAudioFrame(const FString& StreamLabel, const uint8* PCM16Data, int32 NumBytes, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	if (!bAudioEnabled || PCM16Data == nullptr || NumBytes <= 0)
	{
		return false;
	}

	if (!SocketSubsystem || !AudioRemoteAddr.IsValid())
	{
		return false;
	}

	if (!AudioSocket)
	{
		if (!CreateAudioSocket())
		{
			return false;
		}
	}

	O3DS::FAudioFrameMeta Meta;
	Meta.SourceGuid = AudioSourceGuid;
	Meta.StreamLabel = StreamLabel.IsEmpty()
		? (ActiveAudioConfig.StreamLabel.IsEmpty() ? (ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId) : ActiveAudioConfig.StreamLabel)
		: StreamLabel;
	FString SubjectForAudio;
	{
		FScopeLock Lock(&SubjectNameLock);
		SubjectForAudio = LastSubjectName;
	}
	if (SubjectForAudio.IsEmpty())
	{
		SubjectForAudio = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	}
	Meta.SubjectName = MoveTemp(SubjectForAudio);
	Meta.NumChannels = NumChannels > 0 ? NumChannels : FMath::Max(ActiveAudioConfig.NumChannels, 1);
	Meta.SampleRate = SampleRate > 0 ? SampleRate : FMath::Max(ActiveAudioConfig.SampleRate, 1);
	Meta.TimestampSec = TimestampSec;

	TArray<uint8> Payload;
	if (!O3DAudio::SerializePcm16Frame(Meta, PCM16Data, NumBytes, Payload))
	{
		return false;
	}

	if (!SendPayload(AudioSocket, AudioRemoteAddr, Payload.GetData(), Payload.Num(), TEXT("audio")))
	{
		return false;
	}

	Stats.BytesSent += Payload.Num();
	return true;
}
