#include "SocketsTcpSenderNew.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogSocketsTcpSenderNew, Log, All);

class FSocketsTcpSenderAudioSinkNew final : public IO3DSenderAudioSink
{
public:
	FSocketsTcpSenderAudioSinkNew(FO3DSocketsTcpSenderNew& InOwner, FO3DTransportAudioConfig InConfig)
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
	FO3DSocketsTcpSenderNew& Owner;
	FO3DTransportAudioConfig AudioConfig;
};

FO3DSocketsTcpSenderNew::FO3DSocketsTcpSenderNew() = default;

FO3DSocketsTcpSenderNew::~FO3DSocketsTcpSenderNew()
{
	Stop();
}

bool FO3DSocketsTcpSenderNew::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	Stats.Reset();
	StreamId = ActiveConfig.StreamId;

	ActiveAudioConfig = Config.Audio;
	bAudioEnabled = ActiveAudioConfig.bEnableAudio;
	AudioSourceGuid = FGuid::NewGuid();

	// Parse bind address from config
	if (!O3DSockets::ParseHostPort(Config, BindHost, BindPort, TEXT("tcp")))
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("TCP sender requires tcp://host:port URI or explicit host/port options."));
		return false;
	}

	if (BindPort <= 0)
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("TCP sender requires a valid port (got %d)."), BindPort);
		return false;
	}

	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(BindHost, BindPort);
		ActiveConfig.StreamId = StreamId;
	}

	// Audio port defaults to data port + 1
	AudioBindHost = O3DSockets::GetOptionValue(Config, O3DSockets::AudioBindOptionKey);
	if (AudioBindHost.IsEmpty())
	{
		AudioBindHost = BindHost;
	}

	AudioBindPort = O3DSockets::GetIntOption(Config, O3DSockets::AudioPortOptionKey, 0);
	if (AudioBindPort <= 0 && BindPort > 0)
	{
		AudioBindPort = BindPort + 1;
	}

	if (ActiveAudioConfig.StreamLabel.IsEmpty())
	{
		ActiveAudioConfig.StreamLabel = ActiveConfig.StreamId;
	}

	if (bAudioEnabled && AudioBindPort <= 0)
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Audio requested but no valid audio port."));
		bAudioEnabled = false;
	}

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("TCP sender could not access socket subsystem."));
		return false;
	}

	return true;
}

bool FO3DSocketsTcpSenderNew::Start()
{
	DestroySocket();
	DestroyAudioSocket();

	const bool bDataStarted = CreateListenSocket();
	bool bAudioStarted = true;
	if (bAudioEnabled)
	{
		bAudioStarted = CreateAudioListenSocket();
		if (!bAudioStarted)
		{
			UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Failed to create TCP audio listen socket; disabling audio."));
			bAudioEnabled = false;
			DestroyAudioSocket();
			bAudioStarted = true;
		}
	}

	return bDataStarted && bAudioStarted;
}

void FO3DSocketsTcpSenderNew::Stop()
{
	DestroySocket();
	DestroyAudioSocket();
	SocketSubsystem = nullptr;
}

bool FO3DSocketsTcpSenderNew::Send(const O3DS::SubjectList& List)
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
		UE_LOG(LogSocketsTcpSenderNew, Verbose, TEXT("TCP sender failed to serialize SubjectList."));
		Stats.DroppedFrames++;
		return false;
	}

	if (!SendFramed(ClientSocket, reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten))
	{
		// Send failed - drop client and wait for reconnect
		UE_LOG(LogSocketsTcpSenderNew, Log, TEXT("TCP send failed, dropping client."));
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

void FO3DSocketsTcpSenderNew::Tick(float /*DeltaSeconds*/)
{
	TickAcceptClient();
	
	if (bAudioEnabled)
	{
		TickAcceptAudioClient();
	}
}

FO3DTransportStats FO3DSocketsTcpSenderNew::GetStats() const
{
	return Stats;
}

bool FO3DSocketsTcpSenderNew::SupportsAudio() const
{
	return true;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DSocketsTcpSenderNew::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
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
	bAudioEnabled = true;

	if (SocketSubsystem && ListenSocket && bAudioEnabled && !AudioListenSocket)
	{
		if (!CreateAudioListenSocket())
		{
			UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Failed to create audio listen socket when enabling audio."));
			bAudioEnabled = false;
			return nullptr;
		}
	}

	return MakeShared<FSocketsTcpSenderAudioSinkNew>(*this, ActiveAudioConfig);
}

bool FO3DSocketsTcpSenderNew::CreateListenSocket()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	bool bValid = false;
	TSharedPtr<FInternetAddr> BindAddr = CreateBindAddress(BindHost, BindPort, bValid);
	if (!bValid || !BindAddr.IsValid())
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Invalid bind address %s:%d"), *BindHost, BindPort);
		return false;
	}

	ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_LISTEN"), BindAddr->GetProtocolType());
	if (!ListenSocket)
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Failed to create listen socket."));
		return false;
	}

	ListenSocket->SetReuseAddr(true);
	ListenSocket->SetNonBlocking(true);

	if (!ListenSocket->Bind(*BindAddr))
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Bind failed on %s"), *BindAddr->ToString(true));
		DestroySocket();
		return false;
	}

	if (!ListenSocket->Listen(8))
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Listen failed on %s"), *BindAddr->ToString(true));
		DestroySocket();
		return false;
	}

	UE_LOG(LogSocketsTcpSenderNew, Log, TEXT("TCP sender listening on %s"), *BindAddr->ToString(true));
	return true;
}

bool FO3DSocketsTcpSenderNew::CreateAudioListenSocket()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	bool bValid = false;
	TSharedPtr<FInternetAddr> BindAddr = CreateBindAddress(AudioBindHost, AudioBindPort, bValid);
	if (!bValid || !BindAddr.IsValid())
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Invalid audio bind address %s:%d"), *AudioBindHost, AudioBindPort);
		return false;
	}

	AudioListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_AUDIO_LISTEN"), BindAddr->GetProtocolType());
	if (!AudioListenSocket)
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Failed to create audio listen socket."));
		return false;
	}

	AudioListenSocket->SetReuseAddr(true);
	AudioListenSocket->SetNonBlocking(true);

	if (!AudioListenSocket->Bind(*BindAddr))
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Audio bind failed on %s"), *BindAddr->ToString(true));
		DestroyAudioSocket();
		return false;
	}

	if (!AudioListenSocket->Listen(8))
	{
		UE_LOG(LogSocketsTcpSenderNew, Warning, TEXT("Audio listen failed on %s"), *BindAddr->ToString(true));
		DestroyAudioSocket();
		return false;
	}

	UE_LOG(LogSocketsTcpSenderNew, Log, TEXT("TCP sender listening for audio on %s"), *BindAddr->ToString(true));
	return true;
}

void FO3DSocketsTcpSenderNew::DestroySocket()
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

void FO3DSocketsTcpSenderNew::DestroyAudioSocket()
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

void FO3DSocketsTcpSenderNew::TickAcceptClient()
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
		UE_LOG(LogSocketsTcpSenderNew, Log, TEXT("TCP sender accepted client %s"), *PeerAddr->ToString(true));
	}
}

void FO3DSocketsTcpSenderNew::TickAcceptAudioClient()
{
	if (!AudioListenSocket)
	{
		return;
	}

	FScopeLock Lock(&AudioSocketLock);
	if (AudioClientSocket)
	{
		return; // Already have a client
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastAudioAcceptPollTime < 0.01)
	{
		return;
	}
	LastAudioAcceptPollTime = Now;

	TSharedRef<FInternetAddr> PeerAddr = SocketSubsystem->CreateInternetAddr();
	FSocket* Accepted = AudioListenSocket->Accept(*PeerAddr, TEXT("O3DS_TCP_AUDIO_CLIENT"));
	if (Accepted)
	{
		Accepted->SetNonBlocking(true);
		AudioClientSocket = Accepted;
		UE_LOG(LogSocketsTcpSenderNew, Log, TEXT("TCP sender accepted audio client %s"), *PeerAddr->ToString(true));
	}
}

bool FO3DSocketsTcpSenderNew::SendFramed(FSocket* InSocket, const uint8* Data, int32 Size)
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

bool FO3DSocketsTcpSenderNew::SendAudioFrame(const FString& StreamLabel, const uint8* PCM16Data, int32 NumBytes, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	if (!bAudioEnabled || !PCM16Data || NumBytes <= 0)
	{
		return false;
	}

	FScopeLock Lock(&AudioSocketLock);
	if (!AudioClientSocket)
	{
		return false; // No client connected
	}

	O3DS::FAudioFrameMeta Meta;
	Meta.SourceGuid = AudioSourceGuid;
	Meta.StreamLabel = StreamLabel.IsEmpty() ? ActiveAudioConfig.StreamLabel : StreamLabel;
	Meta.SubjectName = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	Meta.NumChannels = NumChannels > 0 ? NumChannels : FMath::Max(ActiveAudioConfig.NumChannels, 1);
	Meta.SampleRate = SampleRate > 0 ? SampleRate : FMath::Max(ActiveAudioConfig.SampleRate, 1);
	Meta.TimestampSec = TimestampSec;

	TArray<uint8> Payload;
	if (!O3DSockets::Tcp::SerializeAudioFramePayload(Meta, PCM16Data, NumBytes, Payload))
	{
		return false;
	}

	if (!SendFramed(AudioClientSocket, Payload.GetData(), Payload.Num()))
	{
		// Send failed - drop audio client
		if (AudioClientSocket && SocketSubsystem)
		{
			SocketSubsystem->DestroySocket(AudioClientSocket);
		}
		AudioClientSocket = nullptr;
		return false;
	}

	Stats.BytesSent += Payload.Num();
	return true;
}

TSharedPtr<FInternetAddr> FO3DSocketsTcpSenderNew::CreateBindAddress(const FString& Host, int32 Port, bool& bOutValid)
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
