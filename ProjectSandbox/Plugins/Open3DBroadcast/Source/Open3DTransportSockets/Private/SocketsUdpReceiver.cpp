#include "SocketsUdpReceiver.h"

#include "O3DTransportTypes.h"
#include "O3DAudioSerialization.h"
#include "SerializedFrameConsumerRegistry.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

#include "o3ds/udp_fragment.h"

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogSocketsUdpReceiver, Log, All);

namespace
{
	struct FReceiverConstants
	{
		static constexpr int32 FragmentHeaderSize = 16;
		static constexpr int32 MaxPayloadSizeBytes = 50 * 1024 * 1024; // 50 MiB safety cap
	};
}

struct FO3DSocketsUdpReceiver::FFragmentState
{
	UdpMapper Mapper;
};

FO3DSocketsUdpReceiver::FO3DSocketsUdpReceiver()
{
	FragmentState = MakeUnique<FFragmentState>();
	AudioFragmentState = MakeUnique<FFragmentState>();
}

FO3DSocketsUdpReceiver::~FO3DSocketsUdpReceiver()
{
	Stop();
}

bool FO3DSocketsUdpReceiver::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	Stats.Reset();
	BindHost.Reset();
	BindPort = 0;
	StreamId = ActiveConfig.StreamId;
	ActiveAudioConfig = Config.Audio;
	bAudioEnabled = ActiveAudioConfig.bEnableAudio;
	AudioBindHost.Reset();
	AudioBindPort = 0;
	bAllowBroadcast = O3DSockets::GetBoolOption(Config, O3DSockets::BroadcastOptionKey, false);
	MaxDatagramBytes = FMath::Clamp(O3DSockets::GetIntOption(Config, O3DSockets::MaxDatagramOptionKey, 64000), 512, 65507);
	MtuBytes = FMath::Clamp(O3DSockets::GetIntOption(Config, O3DSockets::MtuOptionKey, 1200), 256, MaxDatagramBytes);

	FragmentState = MakeUnique<FFragmentState>();
	AudioFragmentState = MakeUnique<FFragmentState>();

	if (!O3DSockets::ParseHostPort(Config, BindHost, BindPort, TEXT("udp")))
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP receiver requires udp://host:port URI or explicit host/port options."));
		return false;
	}

	if (BindPort <= 0)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP receiver requires a valid port (got %d)."), BindPort);
		return false;
	}

	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(BindHost, BindPort);
		ActiveConfig.StreamId = StreamId;
	}

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

	if (bAudioEnabled)
	{
		if (AudioBindPort <= 0)
		{
			UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Audio support requested but no valid audio port specified."));
			bAudioEnabled = false;
		}
	}

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP receiver could not access socket subsystem."));
		return false;
	}

	ReceiveBuffer.Reset();
	AudioReceiveBuffer.Reset();
	LastAudioDropLogTimeSeconds = 0.0;
	return true;
}

void FO3DSocketsUdpReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	Consumer = InConsumer;
}

bool FO3DSocketsUdpReceiver::Start()
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
			UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to initialise UDP audio receiver socket; disabling audio channel."));
			bAudioEnabled = false;
			DestroyAudioSocket();
			bAudioStarted = true;
		}
	}

	return bDataStarted && bAudioStarted;
}

void FO3DSocketsUdpReceiver::Stop()
{
	DestroySocket();
	DestroyAudioSocket();
	SocketSubsystem = nullptr;
	ReceiveBuffer.Reset();
	AudioReceiveBuffer.Reset();
	FragmentState = MakeUnique<FFragmentState>();
	AudioFragmentState = MakeUnique<FFragmentState>();
}

int32 FO3DSocketsUdpReceiver::Poll()
{
	if (!Socket || !SocketSubsystem)
	{
		return 0;
	}

	int32 FramesProcessed = 0;

	if (ReceiveBuffer.Num() < MaxDatagramBytes + FReceiverConstants::FragmentHeaderSize)
	{
		ReceiveBuffer.SetNum(MaxDatagramBytes + FReceiverConstants::FragmentHeaderSize);
	}

	while (true)
	{
		TSharedRef<FInternetAddr> SenderAddr = SocketSubsystem->CreateInternetAddr();
		int32 BytesRead = 0;
		if (!Socket->RecvFrom(ReceiveBuffer.GetData(), ReceiveBuffer.Num(), BytesRead, *SenderAddr))
		{
			const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
			if (Error == SE_EWOULDBLOCK || Error == SE_NO_ERROR)
			{
				break;
			}

			UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP recv failed (error=%d)."), static_cast<int32>(Error));
			break;
		}

		if (BytesRead <= 0)
		{
			break;
		}

		TArray<uint8> Frame;
		if (!ProcessDatagram(ReceiveBuffer.GetData(), BytesRead, Frame, FragmentState))
		{
			continue;
		}

		if (Frame.Num() == 0)
		{
			// Fragment buffered, waiting for completion.
			continue;
		}

		if (Frame.Num() > FReceiverConstants::MaxPayloadSizeBytes)
		{
			UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP payload exceeds safety cap (%d bytes). Dropping."), Frame.Num());
			continue;
		}

		if (TSharedPtr<ISerializedFrameConsumer> ConsumerPinned = Consumer.Pin())
		{
			ConsumerPinned->SubmitFrame(StreamId, Frame, FPlatformTime::Seconds());
		}

		Stats.FramesReceived++;
		Stats.BytesReceived += Frame.Num();
		++FramesProcessed;
	}

	PollAudioChannel(FramesProcessed);

	return FramesProcessed;
}

FO3DTransportStats FO3DSocketsUdpReceiver::GetStats() const
{
	return Stats;
}

bool FO3DSocketsUdpReceiver::SupportsAudio() const
{
	return true;
}

void FO3DSocketsUdpReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
	AudioSink = Sink;
	if (!Sink.IsValid())
	{
		DestroyAudioSocket();
		bAudioEnabled = false;
		AudioReceiveBuffer.Reset();
		AudioFragmentState = MakeUnique<FFragmentState>();
		return;
	}

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

	if (AudioBindPort <= 0)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("SetAudioSink failed: no valid audio port configured."));
		bAudioEnabled = false;
		DestroyAudioSocket();
		return;
	}

	bAudioEnabled = true;

	if (SocketSubsystem && !AudioSocket)
	{
		if (!CreateAudioSocket())
		{
			UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to create UDP audio socket after sink configuration; disabling audio channel."));
			bAudioEnabled = false;
			DestroyAudioSocket();
		}
	}
}
bool FO3DSocketsUdpReceiver::CreateSocket()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	DestroySocket();

	Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UDP_RECEIVER"), FNetworkProtocolTypes::IPv4);
	if (!Socket)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to create UDP socket."));
		return false;
	}

	Socket->SetReuseAddr(true);
	Socket->SetNonBlocking(true);

	if (bAllowBroadcast)
	{
		Socket->SetBroadcast(true);
	}

	FString EffectiveHost = BindHost;
	if (EffectiveHost.IsEmpty() || EffectiveHost == TEXT("*"))
	{
		EffectiveHost = TEXT("0.0.0.0");
	}
	else if (EffectiveHost.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
	{
		EffectiveHost = TEXT("127.0.0.1");
	}

	TSharedRef<FInternetAddr> BindAddr = SocketSubsystem->CreateInternetAddr();
	bool bIsValid = false;
	BindAddr->SetIp(*EffectiveHost, bIsValid);

	if (!bIsValid)
	{
		FIPv4Address IPv4;
		if (FIPv4Address::Parse(EffectiveHost, IPv4))
		{
			BindAddr->SetIp(IPv4.Value);
			bIsValid = true;
		}
	}

	if (!bIsValid)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Invalid UDP bind host '%s'."), *BindHost);
		DestroySocket();
		return false;
	}

	BindAddr->SetPort(BindPort);

	if (!Socket->Bind(*BindAddr))
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to bind UDP socket to %s."), *BindAddr->ToString(true));
		DestroySocket();
		return false;
	}

	int32 RequestedSize = 2 * 1024 * 1024;
	int32 AppliedSize = 0;
	Socket->SetReceiveBufferSize(RequestedSize, AppliedSize);

	ReceiveBuffer.SetNum(MaxDatagramBytes + FReceiverConstants::FragmentHeaderSize);

	UE_LOG(LogSocketsUdpReceiver, Log, TEXT("UDP receiver listening on %s:%d (broadcast=%d, recvBuf=%d)."),
		*BindAddr->ToString(false), BindAddr->GetPort(), bAllowBroadcast ? 1 : 0, AppliedSize);

	return true;
}

bool FO3DSocketsUdpReceiver::CreateAudioSocket()
{
	if (!SocketSubsystem || AudioBindPort <= 0)
	{
		return false;
	}

	DestroyAudioSocket();

	AudioSocket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UDP_AUDIO_RECEIVER"), FNetworkProtocolTypes::IPv4);
	if (!AudioSocket)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to create UDP audio socket."));
		return false;
	}

	AudioSocket->SetReuseAddr(true);
	AudioSocket->SetNonBlocking(true);

	if (bAllowBroadcast)
	{
		AudioSocket->SetBroadcast(true);
	}

	FString EffectiveHost = AudioBindHost;
	if (EffectiveHost.IsEmpty() || EffectiveHost == TEXT("*"))
	{
		EffectiveHost = TEXT("0.0.0.0");
	}
	else if (EffectiveHost.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
	{
		EffectiveHost = TEXT("127.0.0.1");
	}

	TSharedRef<FInternetAddr> BindAddr = SocketSubsystem->CreateInternetAddr();
	bool bIsValid = false;
	BindAddr->SetIp(*EffectiveHost, bIsValid);

	if (!bIsValid)
	{
		FIPv4Address IPv4;
		if (FIPv4Address::Parse(EffectiveHost, IPv4))
		{
			BindAddr->SetIp(IPv4.Value);
			bIsValid = true;
		}
	}

	if (!bIsValid)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Invalid UDP audio bind host '%s'."), *AudioBindHost);
		DestroyAudioSocket();
		return false;
	}

	BindAddr->SetPort(AudioBindPort);

	if (!AudioSocket->Bind(*BindAddr))
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to bind UDP audio socket to %s."), *BindAddr->ToString(true));
		DestroyAudioSocket();
		return false;
	}

	int32 RequestedSize = 2 * 1024 * 1024;
	int32 AppliedSize = 0;
	AudioSocket->SetReceiveBufferSize(RequestedSize, AppliedSize);

	AudioReceiveBuffer.SetNum(MaxDatagramBytes + FReceiverConstants::FragmentHeaderSize);

	UE_LOG(LogSocketsUdpReceiver, Log, TEXT("UDP audio receiver listening on %s:%d (broadcast=%d, recvBuf=%d)."),
		*BindAddr->ToString(false), BindAddr->GetPort(), bAllowBroadcast ? 1 : 0, AppliedSize);

	return true;
}

void FO3DSocketsUdpReceiver::DestroySocket()
{
	if (Socket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(Socket);
	}
	Socket = nullptr;
}

void FO3DSocketsUdpReceiver::DestroyAudioSocket()
{
	if (AudioSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(AudioSocket);
	}
	AudioSocket = nullptr;
}

bool FO3DSocketsUdpReceiver::ProcessDatagram(const uint8* Data, int32 Bytes, TArray<uint8>& OutFrame, TUniquePtr<FFragmentState>& InState)
{
	OutFrame.Reset();

	if (Data == nullptr || Bytes <= 0)
	{
		return false;
	}

	if (Bytes > MaxDatagramBytes + FReceiverConstants::FragmentHeaderSize)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP datagram too large (%d bytes)."), Bytes);
		return false;
	}

	if (IsFragmentPacket(Data, Bytes))
	{
		return HandleFragment(Data, Bytes, OutFrame, InState);
	}

	OutFrame.SetNum(Bytes);
	FMemory::Memcpy(OutFrame.GetData(), Data, Bytes);
	return true;
}

bool FO3DSocketsUdpReceiver::HandleFragment(const uint8* Data, int32 Bytes, TArray<uint8>& OutFrame, TUniquePtr<FFragmentState>& InState)
{
	if (!InState)
	{
		InState = MakeUnique<FFragmentState>();
	}

	if (!InState->Mapper.addFragment(reinterpret_cast<const char*>(Data), static_cast<size_t>(Bytes)))
	{
		UE_LOG(LogSocketsUdpReceiver, Verbose, TEXT("Discarded malformed UDP fragment (size=%d)."), Bytes);
		return false;
	}

	std::vector<char> Combined;
	if (InState->Mapper.getFrame(Combined))
	{
		if (Combined.size() > static_cast<size_t>(FReceiverConstants::MaxPayloadSizeBytes))
		{
			UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Reassembled UDP payload exceeds safety cap (%llu bytes). Dropping."), static_cast<unsigned long long>(Combined.size()));
			return false;
		}

		OutFrame.SetNum(static_cast<int32>(Combined.size()));
		if (OutFrame.Num() > 0)
		{
			FMemory::Memcpy(OutFrame.GetData(), Combined.data(), OutFrame.Num());
		}
	}

	return true;
}

bool FO3DSocketsUdpReceiver::IsFragmentPacket(const uint8* Data, int32 Bytes) const
{
	if (Bytes < FReceiverConstants::FragmentHeaderSize)
	{
		return false;
	}

	const uint32* Header = reinterpret_cast<const uint32*>(Data);
	const uint32 Sequence = Header[1];
	const uint32 TotalSize = Header[2];
	const uint32 FragmentSize = Header[3];

	if (FragmentSize == 0 || FragmentSize > static_cast<uint32>(MaxDatagramBytes))
	{
		return false;
	}

	if (TotalSize == 0 || TotalSize > static_cast<uint32>(FReceiverConstants::MaxPayloadSizeBytes))
	{
		return false;
	}

	const uint32 FrameCount = (TotalSize + FragmentSize - 1) / FragmentSize;
	if (FrameCount == 0 || FrameCount > 4096)
	{
		return false;
	}

	if (Sequence >= FrameCount)
	{
		return false;
	}

	const uint32 ExpectedPayload = (Sequence == FrameCount - 1)
		? (TotalSize - (FrameCount - 1) * FragmentSize)
		: FragmentSize;

	const uint32 ActualPayload = static_cast<uint32>(Bytes - FReceiverConstants::FragmentHeaderSize);
	return ActualPayload == ExpectedPayload;
}

void FO3DSocketsUdpReceiver::PollAudioChannel(int32& OutFramesProcessed)
{
	if (!bAudioEnabled || !AudioSocket || !SocketSubsystem)
	{
		return;
	}

	if (AudioReceiveBuffer.Num() < MaxDatagramBytes + FReceiverConstants::FragmentHeaderSize)
	{
		AudioReceiveBuffer.SetNum(MaxDatagramBytes + FReceiverConstants::FragmentHeaderSize);
	}

	while (true)
	{
		TSharedRef<FInternetAddr> SenderAddr = SocketSubsystem->CreateInternetAddr();
		int32 BytesRead = 0;
		if (!AudioSocket->RecvFrom(AudioReceiveBuffer.GetData(), AudioReceiveBuffer.Num(), BytesRead, *SenderAddr))
		{
			const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
			if (Error == SE_EWOULDBLOCK || Error == SE_NO_ERROR)
			{
				break;
			}

			UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP audio recv failed (error=%d)."), static_cast<int32>(Error));
			break;
		}

		if (BytesRead <= 0)
		{
			break;
		}

		TArray<uint8> Payload;
		if (!ProcessDatagram(AudioReceiveBuffer.GetData(), BytesRead, Payload, AudioFragmentState))
		{
			continue;
		}

		if (Payload.Num() == 0)
		{
			continue;
		}

		if (Payload.Num() > FReceiverConstants::MaxPayloadSizeBytes)
		{
			UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP audio payload exceeds safety cap (%d bytes). Dropping."), Payload.Num());
			continue;
		}

		if (ProcessAudioPayload(Payload))
		{
			++OutFramesProcessed;
		}
	}
}

bool FO3DSocketsUdpReceiver::ProcessAudioPayload(const TArray<uint8>& Payload)
{
	if (!bAudioEnabled || Payload.Num() <= 0)
	{
		return false;
	}

	O3DAudio::FPcm16Frame Frame;
	if (!O3DAudio::DeserializePcm16Frame(Payload.GetData(), Payload.Num(), Frame))
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to deserialize UDP audio frame (payloadSize=%d)."), Payload.Num());
		return false;
	}

	Stats.BytesReceived += Payload.Num();

	if (TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> SinkPinned = AudioSink.Pin())
	{
		SinkPinned->SubmitPcm16(Frame.Meta, Frame.PCM16.GetData(), Frame.PCM16.Num());
		return true;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastAudioDropLogTimeSeconds > 1.0)
	{
		UE_LOG(LogSocketsUdpReceiver, Verbose, TEXT("UDP audio frame discarded (no sink)."));
		LastAudioDropLogTimeSeconds = Now;
	}

	return false;
}
