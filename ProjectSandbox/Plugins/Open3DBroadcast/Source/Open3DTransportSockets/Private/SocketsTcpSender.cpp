#include "SocketsTcpSender.h"
#include "SocketsTcpAudio.h"
#include "SocketsTcpTransport.h"

#include "O3DTransportTypes.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Logging/LogMacros.h"

#include "o3ds/model.h"

#include <vector>

namespace
{
	inline constexpr int32 DesiredSendBufferBytes = 512 * 1024;

	inline FString ResolveBindHost(const FO3DTransportConfig& Config)
	{
		const FString BindOverride = O3DSockets::GetOptionValue(Config, O3DSockets::BindOptionKey);
		if (!BindOverride.IsEmpty())
		{
			return BindOverride;
		}
		FString Host;
		int32 Port = 0;
		if (O3DSockets::ParseHostPort(Config, Host, Port))
		{
			return Host;
		}
		return TEXT("0.0.0.0");
	}

	inline int32 ResolveBindPort(const FO3DTransportConfig& Config)
	{
		int32 Port = 0;
		FString Host;
		if (O3DSockets::ParseHostPort(Config, Host, Port))
		{
			return Port;
		}
		return O3DSockets::GetIntOption(Config, O3DSockets::PortOptionKey, 0);
	}

	inline FString ResolveAudioBindHost(const FO3DTransportConfig& Config, const FString& DefaultHost)
	{
		const FString AudioBindOverride = O3DSockets::GetOptionValue(Config, O3DSockets::AudioBindOptionKey);
		if (!AudioBindOverride.IsEmpty())
		{
			return AudioBindOverride;
		}
		return DefaultHost;
	}

	inline int32 ResolveAudioBindPort(const FO3DTransportConfig& Config, int32 DefaultPort)
	{
		const int32 ExplicitPort = O3DSockets::GetIntOption(Config, O3DSockets::AudioPortOptionKey, 0);
		if (ExplicitPort > 0)
		{
			return ExplicitPort;
		}
		return DefaultPort > 0 ? DefaultPort + 1 : 0;
	}

	inline bool IsRecoverableSendError(ESocketErrors Error)
	{
		return Error == SE_EWOULDBLOCK ||
			Error == SE_EINTR ||
			Error == SE_EINPROGRESS ||
			Error == SE_ENOBUFS;
	}

}

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

		const int32 NumSamples = NumFrames * NumChannels;
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
	FO3DSocketsTcpSender& Owner;
	FO3DTransportAudioConfig AudioConfig;
};

DEFINE_LOG_CATEGORY_STATIC(LogSocketsTcpSender, Log, All);

// ---- FO3DSocketsTcpSender ----------------------------------------------------------------------

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

	BindHost = ResolveBindHost(Config);
	BindPort = ResolveBindPort(Config);
	StreamId = O3DSockets::ComposeStreamId(BindHost, BindPort);

	if (BindPort <= 0)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender requires a valid port (got %d)."), BindPort);
		return false;
	}

	if (ActiveConfig.Uri.IsEmpty())
	{
		ActiveConfig.Uri = O3DSockets::BuildTcpUri(BindHost, BindPort);
	}

	if (ActiveConfig.StreamId.IsEmpty())
	{
		ActiveConfig.StreamId = StreamId;
	}

	ActiveAudioConfig = Config.Audio;
	bAudioEnabled = ActiveAudioConfig.bEnableAudio;
	AudioBindHost = ResolveAudioBindHost(Config, BindHost);
	AudioBindPort = ResolveAudioBindPort(Config, BindPort);
	if (bAudioEnabled)
	{
		if (AudioBindPort <= 0)
		{
			UE_LOG(LogSocketsTcpSender, Warning, TEXT("Audio support requested but no valid audio port specified."));
			bAudioEnabled = false;
		}
		if (ActiveAudioConfig.StreamLabel.IsEmpty())
		{
			ActiveAudioConfig.StreamLabel = ActiveConfig.StreamId;
		}
	}
	AudioSourceGuid = FGuid::NewGuid();
	LastAudioAcceptPollSeconds = 0.0;

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	return SocketSubsystem != nullptr;
}

bool FO3DSocketsTcpSender::Start()
{
	DestroySockets();
	DestroyAudioSockets();

	if (!CreateListenSocket())
	{
		return false;
	}

	if (bAudioEnabled)
	{
		if (!CreateAudioListenSocket())
		{
			UE_LOG(LogSocketsTcpSender, Warning, TEXT("Failed to create TCP audio listen socket on %s:%d; disabling audio channel."), *AudioBindHost, AudioBindPort);
			bAudioEnabled = false;
			DestroyAudioSockets();
		}
	}

	return true;
}

void FO3DSocketsTcpSender::Stop()
{
	DestroySockets();
	SocketSubsystem = nullptr;
}

bool FO3DSocketsTcpSender::Send(const O3DS::SubjectList& List)
{
	if (!ClientSocket)
	{
		AcceptClient();
	}

	if (!ClientSocket || ClientSocket->GetConnectionState() != SCS_Connected)
	{
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

	if (!SendFramedPayload(reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten))
	{
		Stats.DroppedFrames++;
		return false;
	}

	Stats.FramesSent++;
	Stats.BytesSent += BytesWritten;
	return true;
}

void FO3DSocketsTcpSender::Tick(float /*DeltaSeconds*/)
{
	if (!ClientSocket)
	{
		AcceptClient();
	}

	if (bAudioEnabled)
	{
		AcceptAudioClient();
	}
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
	else if (!EffectiveConfig.bEnableAudio)
	{
		return nullptr;
	}

	EffectiveConfig.bEnableAudio = true;
	EffectiveConfig.NumChannels = FMath::Max(EffectiveConfig.NumChannels, 1);
	EffectiveConfig.SampleRate = FMath::Max(EffectiveConfig.SampleRate, 1);
	if (EffectiveConfig.StreamLabel.IsEmpty())
	{
		EffectiveConfig.StreamLabel = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	}

	ActiveAudioConfig = EffectiveConfig;
	bAudioEnabled = true;

	if (SocketSubsystem && ListenSocket && bAudioEnabled && !AudioListenSocket)
	{
		if (!CreateAudioListenSocket())
		{
			UE_LOG(LogSocketsTcpSender, Warning, TEXT("Failed to create TCP audio listen socket when enabling audio."));
			bAudioEnabled = false;
			return nullptr;
		}
	}

	return MakeShared<FSocketsTcpSenderAudioSink>(*this, ActiveAudioConfig);
}

bool FO3DSocketsTcpSender::CreateListenSocket()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	bool bAddrValid = false;
	TSharedPtr<FInternetAddr> BindAddr = CreateBindAddress(BindHost, BindPort, bAddrValid);
	if (!bAddrValid || !BindAddr.IsValid())
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
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Bind failed on %s."), *BindAddr->ToString(true));
		DestroySockets();
		return false;
	}

	if (!ListenSocket->Listen(8))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Listen failed on %s."), *BindAddr->ToString(true));
		DestroySockets();
		return false;
	}

	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender listening on %s"), *BindAddr->ToString(true));
	return true;
}

bool FO3DSocketsTcpSender::CreateAudioListenSocket()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	bool bAddrValid = false;
	TSharedPtr<FInternetAddr> BindAddr = CreateBindAddress(AudioBindHost, AudioBindPort, bAddrValid);
	if (!bAddrValid || !BindAddr.IsValid())
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Invalid audio bind address %s:%d"), *AudioBindHost, AudioBindPort);
		return false;
	}

	AudioListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_AUDIO_LISTEN"), BindAddr->GetProtocolType());
	if (!AudioListenSocket)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Failed to create audio listen socket."));
		return false;
	}

	AudioListenSocket->SetReuseAddr(true);
	AudioListenSocket->SetNonBlocking(true);

	if (!AudioListenSocket->Bind(*BindAddr))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Audio bind failed on %s."), *BindAddr->ToString(true));
		DestroyAudioSockets();
		return false;
	}

	if (!AudioListenSocket->Listen(8))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Audio listen failed on %s."), *BindAddr->ToString(true));
		DestroyAudioSockets();
		return false;
	}

	AudioListenSocket->GetAddress(*BindAddr);
	AudioBindPort = BindAddr->GetPort();
	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender listening for audio on %s"), *BindAddr->ToString(true));
	return true;
}

void FO3DSocketsTcpSender::DestroySockets()
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

	DestroyAudioSockets();
}

void FO3DSocketsTcpSender::DestroyAudioSockets()
{
	FScopeLock Lock(&AudioSocketLock);
	if (AudioClientSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(AudioClientSocket);
	}
	AudioClientSocket = nullptr;

	if (AudioListenSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(AudioListenSocket);
	}
	AudioListenSocket = nullptr;
}

void FO3DSocketsTcpSender::ResetClientSocket()
{
	if (ClientSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(ClientSocket);
	}
	ClientSocket = nullptr;
}

void FO3DSocketsTcpSender::ResetAudioClientSocket()
{
	FScopeLock Lock(&AudioSocketLock);
	if (AudioClientSocket && SocketSubsystem)
	{
		UE_LOG(LogSocketsTcpSender, Verbose, TEXT("ResetAudioClientSocket invoked."));
		SocketSubsystem->DestroySocket(AudioClientSocket);
	}
	AudioClientSocket = nullptr;
}

bool FO3DSocketsTcpSender::AcceptClient()
{
	if (!ListenSocket || !SocketSubsystem)
	{
		return false;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastAcceptPollSeconds < 0.005)
	{
		return false;
	}
	LastAcceptPollSeconds = Now;

	TSharedRef<FInternetAddr> PeerAddr = SocketSubsystem->CreateInternetAddr();
	FSocket* Accepted = ListenSocket->Accept(*PeerAddr, TEXT("O3DS_TCP_ACCEPT"));
	if (!Accepted)
	{
		return false;
	}

	ClientSocket = Accepted;
	ClientSocket->SetNonBlocking(true);
	
	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender accepted client %s"), *PeerAddr->ToString(true));
	return true;
}

bool FO3DSocketsTcpSender::AcceptAudioClient()
{
	if (!AudioListenSocket || !SocketSubsystem)
	{
		return false;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastAudioAcceptPollSeconds < 0.005)
	{
		return false;
	}
	LastAudioAcceptPollSeconds = Now;

	TSharedRef<FInternetAddr> PeerAddr = SocketSubsystem->CreateInternetAddr();
	FScopeLock Lock(&AudioSocketLock);
	if (AudioClientSocket)
	{
		return true;
	}

	FSocket* Accepted = AudioListenSocket->Accept(*PeerAddr, TEXT("O3DS_TCP_AUDIO_ACCEPT"));
	if (!Accepted)
	{
		return false;
	}

	AudioClientSocket = Accepted;
	AudioClientSocket->SetNonBlocking(true);
	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender accepted audio client %s"), *PeerAddr->ToString(true));
	return true;
}

bool FO3DSocketsTcpSender::SendFramedPayload(const uint8* Data, int32 Size)
{
	if (!ClientSocket || !SocketSubsystem || Data == nullptr || Size <= 0)
	{
		return false;
	}

	if (!ClientSocket)
	{
		return false;
	}

	// Send header and payload separately like the old working implementation
	uint8 Header[O3DSockets::Tcp::FrameHeaderSize];
	O3DSockets::Tcp::WriteFrameHeader(Header, Size);

	int32 HeaderSent = 0;
	if (!ClientSocket->Send(Header, O3DSockets::Tcp::FrameHeaderSize, HeaderSent) || HeaderSent != O3DSockets::Tcp::FrameHeaderSize)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP send header failed."));
		ResetClientSocket();
		return false;
	}

	int32 PayloadSent = 0;
	if (!ClientSocket->Send(Data, Size, PayloadSent) || PayloadSent != Size)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP send payload failed (sent %d/%d)."), PayloadSent, Size);
		ResetClientSocket();
		return false;
	}

	return true;
}

bool FO3DSocketsTcpSender::SendAudioFramedPayload(const uint8* Data, int32 Size)
{
	FScopeLock Lock(&AudioSocketLock);
	if (!AudioClientSocket)
	{
		return false;
	}

	// Send header and payload separately
	uint8 Header[O3DSockets::Tcp::FrameHeaderSize];
	O3DSockets::Tcp::WriteFrameHeader(Header, Size);

	int32 HeaderSent = 0;
	if (!AudioClientSocket->Send(Header, O3DSockets::Tcp::FrameHeaderSize, HeaderSent) || HeaderSent != O3DSockets::Tcp::FrameHeaderSize)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP audio send header failed."));
		ResetAudioClientSocket();
		return false;
	}

	int32 PayloadSent = 0;
	if (!AudioClientSocket->Send(Data, Size, PayloadSent) || PayloadSent != Size)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP audio send payload failed."));
		ResetAudioClientSocket();
		return false;
	}

	return true;
}

bool FO3DSocketsTcpSender::SendAudioFrame(const FString& StreamLabel, const uint8* PCM16Data, int32 NumBytes, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	if (!bAudioEnabled || PCM16Data == nullptr || NumBytes <= 0)
	{
		return false;
	}

	AcceptAudioClient();

	O3DS::FAudioFrameMeta Meta;
	Meta.SourceGuid = AudioSourceGuid;
	Meta.StreamLabel = StreamLabel.IsEmpty() ? (ActiveAudioConfig.StreamLabel.IsEmpty() ? (ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId) : ActiveAudioConfig.StreamLabel) : StreamLabel;
	Meta.SubjectName = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	Meta.NumChannels = NumChannels > 0 ? NumChannels : FMath::Max(ActiveAudioConfig.NumChannels, 1);
	Meta.SampleRate = SampleRate > 0 ? SampleRate : FMath::Max(ActiveAudioConfig.SampleRate, 1);
	Meta.TimestampSec = TimestampSec;

	TArray<uint8> Payload;
	if (!O3DSockets::Tcp::SerializeAudioFramePayload(Meta, PCM16Data, NumBytes, Payload))
	{
		return false;
	}

	if (!SendAudioFramedPayload(Payload.GetData(), Payload.Num()))
	{
		return false;
	}

	Stats.BytesSent += Payload.Num();
	return true;
}

TSharedPtr<FInternetAddr> FO3DSocketsTcpSender::CreateBindAddress(const FString& Host, int32 Port, bool& bOutValid) const
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
	if (HostToUse.IsEmpty() || HostToUse == TEXT("*"))
	{
		HostToUse = TEXT("0.0.0.0");
	}
	else if (HostToUse.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
	{
		HostToUse = TEXT("127.0.0.1");
	}

	Addr->SetPort(Port);
	Addr->SetIp(*HostToUse, bOutValid);
	return Addr;
}

