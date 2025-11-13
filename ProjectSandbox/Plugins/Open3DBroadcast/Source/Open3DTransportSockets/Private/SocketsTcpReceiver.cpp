#include "SocketsTcpReceiver.h"
#include "SocketsTcpAudio.h"
#include "SocketsTcpTransport.h"
#include "O3DTransportTypes.h"
#include "SerializedFrameConsumerRegistry.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogSocketsTcpReceiver, Log, All);

namespace
{
	constexpr double InitialBackoffSeconds = 0.5;
	constexpr double MaxBackoffSeconds = 5.0;
	constexpr int32 MaxPayloadSizeBytes = 50 * 1024 * 1024; // 50 MiB safety cap
}

FO3DSocketsTcpReceiver::FO3DSocketsTcpReceiver() = default;

FO3DSocketsTcpReceiver::~FO3DSocketsTcpReceiver()
{
	Stop();
}

bool FO3DSocketsTcpReceiver::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	Stats.Reset();
	StreamId = ActiveConfig.StreamId;

	ActiveAudioConfig = Config.Audio;
	bAudioEnabled = ActiveAudioConfig.bEnableAudio;

	if (!O3DSockets::ParseHostPort(Config, RemoteHost, RemotePort, TEXT("tcp")))
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver requires tcp://host:port URI or explicit host/port options."));
		return false;
	}

	if (RemotePort <= 0)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver requires a valid port (got %d)."), RemotePort);
		return false;
	}

	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(RemoteHost, RemotePort);
		ActiveConfig.StreamId = StreamId;
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

	if (bAudioEnabled && AudioRemotePort <= 0)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Audio requested but no valid audio port."));
		bAudioEnabled = false;
	}

	if (ActiveAudioConfig.StreamLabel.IsEmpty())
	{
		ActiveAudioConfig.StreamLabel = ActiveConfig.StreamId;
	}

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver could not access socket subsystem."));
		return false;
	}

	return true;
}

void FO3DSocketsTcpReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	Consumer = InConsumer;
}

bool FO3DSocketsTcpReceiver::Start()
{
	DisconnectSocket();
	DisconnectAudioSocket();

	const bool bStarted = ConnectToServer();
	const bool bAudioStarted = !bAudioEnabled || ConnectAudioToServer();
	return bStarted && bAudioStarted;
}

void FO3DSocketsTcpReceiver::Stop()
{
	DisconnectSocket();
	DisconnectAudioSocket();
	SocketSubsystem = nullptr;
}

int32 FO3DSocketsTcpReceiver::Poll()
{
	if (!SocketSubsystem)
	{
		return 0;
	}

	TickConnection();
	
	int32 FramesProcessed = 0;

	if (Socket && State == EState::Connected)
	{
		TArray<uint8> Frame;
		while (ReadFramed(Socket, State, ReceiveBuffer, BytesBuffered, ExpectedPayloadSize, Frame))
		{
			if (Frame.Num() > 0)
			{
				if (TSharedPtr<ISerializedFrameConsumer> ConsumerPinned = Consumer.Pin())
				{
					ConsumerPinned->SubmitFrame(StreamId, Frame, FPlatformTime::Seconds());
				}

				Stats.FramesReceived++;
				Stats.BytesReceived += Frame.Num();
				++FramesProcessed;
			}
		}
	}

	if (bAudioEnabled)
	{
		TickAudioConnection();
		PollAudioChannel(FramesProcessed);
	}

	return FramesProcessed;
}

FO3DTransportStats FO3DSocketsTcpReceiver::GetStats() const
{
	return Stats;
}

bool FO3DSocketsTcpReceiver::SupportsAudio() const
{
	return true;
}

void FO3DSocketsTcpReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
	AudioSink = Sink;
	if (!Sink.IsValid())
	{
		DisconnectAudioSocket();
		bAudioEnabled = false;
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
		EffectiveConfig.StreamLabel = ActiveConfig.StreamId;
	}

	ActiveAudioConfig = EffectiveConfig;
	bAudioEnabled = true;

	if (SocketSubsystem && !AudioSocket)
	{
		ConnectAudioToServer();
	}
}

bool FO3DSocketsTcpReceiver::ConnectToServer()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	DisconnectSocket();

	Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_CLIENT"), false);
	if (!Socket)
	{
		return false;
	}

	Socket->SetNonBlocking(true);

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bValid = false;
	Addr->SetIp(*RemoteHost, bValid);
	if (!bValid)
	{
		FIPv4Address IPv4;
		if (FIPv4Address::Parse(RemoteHost, IPv4))
		{
			Addr->SetIp(IPv4.Value);
			bValid = true;
		}
	}

	if (!bValid)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Invalid TCP host '%s'"), *RemoteHost);
		DisconnectSocket();
		return false;
	}

	Addr->SetPort(RemotePort);

	State = EState::Connecting;
	BytesBuffered = 0;
	ExpectedPayloadSize = 0;

	Socket->Connect(*Addr);
	LastConnectAttempt = FPlatformTime::Seconds();
	ConnectBackoffAttempt = 0;

	UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver connecting to %s:%d"), *RemoteHost, RemotePort);
	return true;
}

bool FO3DSocketsTcpReceiver::ConnectAudioToServer()
{
	if (!SocketSubsystem || !bAudioEnabled || AudioRemotePort <= 0)
	{
		return false;
	}

	DisconnectAudioSocket();

	AudioSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_AUDIO_CLIENT"), false);
	if (!AudioSocket)
	{
		return false;
	}

	AudioSocket->SetNonBlocking(true);

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bValid = false;
	Addr->SetIp(*AudioRemoteHost, bValid);
	if (!bValid)
	{
		FIPv4Address IPv4;
		if (FIPv4Address::Parse(AudioRemoteHost, IPv4))
		{
			Addr->SetIp(IPv4.Value);
			bValid = true;
		}
	}

	if (!bValid)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Invalid TCP audio host '%s'"), *AudioRemoteHost);
		DisconnectAudioSocket();
		return false;
	}

	Addr->SetPort(AudioRemotePort);

	AudioState = EState::Connecting;
	AudioBytesBuffered = 0;
	AudioExpectedPayloadSize = 0;

	AudioSocket->Connect(*Addr);
	LastAudioConnectAttempt = FPlatformTime::Seconds();
	AudioConnectBackoffAttempt = 0;

	UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver audio connecting to %s:%d"), *AudioRemoteHost, AudioRemotePort);
	return true;
}

void FO3DSocketsTcpReceiver::DisconnectSocket()
{
	if (Socket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(Socket);
	}
	Socket = nullptr;
	State = EState::Disconnected;
	BytesBuffered = 0;
	ExpectedPayloadSize = 0;
	ReceiveBuffer.Reset();
}

void FO3DSocketsTcpReceiver::DisconnectAudioSocket()
{
	if (AudioSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(AudioSocket);
	}
	AudioSocket = nullptr;
	AudioState = EState::Disconnected;
	AudioBytesBuffered = 0;
	AudioExpectedPayloadSize = 0;
	AudioReceiveBuffer.Reset();
}

void FO3DSocketsTcpReceiver::TickConnection()
{
	if (!Socket)
	{
		// Need to reconnect
		const double Now = FPlatformTime::Seconds();
		const double BackoffSeconds = FMath::Min(MaxBackoffSeconds, InitialBackoffSeconds * FMath::Pow(2.0, static_cast<double>(FMath::Clamp(ConnectBackoffAttempt, 0, 8))));
		if ((Now - LastConnectAttempt) >= BackoffSeconds)
		{
			LastConnectAttempt = Now;
			++ConnectBackoffAttempt;
			ConnectToServer();
		}
		return;
	}

	if (State == EState::Connecting)
	{
		const ESocketConnectionState ConnState = Socket->GetConnectionState();
		if (ConnState == SCS_Connected)
		{
			State = EState::Connected;
			ConnectBackoffAttempt = 0;
			UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver connected to %s:%d"), *RemoteHost, RemotePort);
		}
		else if (ConnState == SCS_ConnectionError)
		{
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP connection error, will retry"));
			DisconnectSocket();
		}
	}
	else if (State == EState::Connected)
	{
		const ESocketConnectionState ConnState = Socket->GetConnectionState();
		if (ConnState != SCS_Connected)
		{
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP connection lost, will reconnect"));
			DisconnectSocket();
		}
	}
}

void FO3DSocketsTcpReceiver::TickAudioConnection()
{
	if (!AudioSocket && bAudioEnabled)
	{
		// Need to reconnect
		const double Now = FPlatformTime::Seconds();
		const double BackoffSeconds = FMath::Min(MaxBackoffSeconds, InitialBackoffSeconds * FMath::Pow(2.0, static_cast<double>(FMath::Clamp(AudioConnectBackoffAttempt, 0, 8))));
		if ((Now - LastAudioConnectAttempt) >= BackoffSeconds)
		{
			LastAudioConnectAttempt = Now;
			++AudioConnectBackoffAttempt;
			ConnectAudioToServer();
		}
		return;
	}

	if (!AudioSocket)
	{
		return;
	}

	if (AudioState == EState::Connecting)
	{
		const ESocketConnectionState ConnState = AudioSocket->GetConnectionState();
		if (ConnState == SCS_Connected)
		{
			AudioState = EState::Connected;
			AudioConnectBackoffAttempt = 0;
			UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver audio connected to %s:%d"), *AudioRemoteHost, AudioRemotePort);
		}
		else if (ConnState == SCS_ConnectionError)
		{
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP audio connection error, will retry"));
			DisconnectAudioSocket();
		}
	}
	else if (AudioState == EState::Connected)
	{
		const ESocketConnectionState ConnState = AudioSocket->GetConnectionState();
		if (ConnState != SCS_Connected)
		{
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP audio connection lost, will reconnect"));
			DisconnectAudioSocket();
		}
	}
}

bool FO3DSocketsTcpReceiver::ReadFramed(FSocket* InSocket, EState& InState, TArray<uint8>& Buffer, int32& InOutBytesBuffered, int32& InOutExpectedPayloadSize, TArray<uint8>& OutFrame)
{
	if (!InSocket || InState != EState::Connected)
	{
		return false;
	}

	// Ensure buffer has space
	const int32 HeaderSize = O3DSockets::Tcp::FrameHeaderSize;
	const int32 MinBufferSize = FMath::Max(HeaderSize, InOutExpectedPayloadSize + HeaderSize);
	if (Buffer.Num() < MinBufferSize)
	{
		Buffer.SetNum(MinBufferSize);
	}

	// Try to read more data
	int32 BytesRead = 0;
	const int32 SpaceAvailable = Buffer.Num() - InOutBytesBuffered;
	if (SpaceAvailable > 0)
	{
		if (!InSocket->Recv(Buffer.GetData() + InOutBytesBuffered, SpaceAvailable, BytesRead))
		{
			const ESocketErrors Error = SocketSubsystem ? SocketSubsystem->GetLastErrorCode() : SE_NO_ERROR;
			if (Error != SE_EWOULDBLOCK && Error != SE_NO_ERROR)
			{
				return false; // Connection error
			}
			BytesRead = 0;
		}

		if (BytesRead == 0)
		{
			return false; // No data available or connection closed
		}

		InOutBytesBuffered += BytesRead;
	}

	// Parse header if we have enough data
	if (InOutExpectedPayloadSize == 0 && InOutBytesBuffered >= HeaderSize)
	{
		// Check magic
		if (!O3DSockets::Tcp::MatchesMagic(Buffer.GetData()))
		{
			// Resync - look for magic
			InOutBytesBuffered = 0;
			return false;
		}

		InOutExpectedPayloadSize = static_cast<int32>(O3DSockets::Tcp::DecodePayloadSize(Buffer.GetData()));
		if (InOutExpectedPayloadSize <= 0 || InOutExpectedPayloadSize > MaxPayloadSizeBytes)
		{
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Invalid frame size %d"), InOutExpectedPayloadSize);
			InOutBytesBuffered = 0;
			InOutExpectedPayloadSize = 0;
			return false;
		}

		// Ensure buffer can hold header + payload
		if (Buffer.Num() < HeaderSize + InOutExpectedPayloadSize)
		{
			Buffer.SetNum(HeaderSize + InOutExpectedPayloadSize);
		}
	}

	// Check if we have complete frame
	if (InOutExpectedPayloadSize > 0 && InOutBytesBuffered >= HeaderSize + InOutExpectedPayloadSize)
	{
		// Extract payload
		OutFrame.Reset(InOutExpectedPayloadSize);
		OutFrame.SetNumUninitialized(InOutExpectedPayloadSize);
		FMemory::Memcpy(OutFrame.GetData(), Buffer.GetData() + HeaderSize, InOutExpectedPayloadSize);

		// Reset for next frame
		InOutBytesBuffered = 0;
		InOutExpectedPayloadSize = 0;
		return true;
	}

	return false;
}

void FO3DSocketsTcpReceiver::PollAudioChannel(int32& OutFramesProcessed)
{
	if (!AudioSocket || AudioState != EState::Connected || !AudioSink.Pin().IsValid())
	{
		return;
	}

	TArray<uint8> Frame;
	while (ReadFramed(AudioSocket, AudioState, AudioReceiveBuffer, AudioBytesBuffered, AudioExpectedPayloadSize, Frame))
	{
		if (Frame.Num() > 0)
		{
			ProcessAudioPayload(Frame);
			++OutFramesProcessed;
		}
	}
}

bool FO3DSocketsTcpReceiver::ProcessAudioPayload(const TArray<uint8>& Payload)
{
	if (Payload.Num() == 0)
	{
		return false;
	}

	O3DSockets::Tcp::FAudioFrame AudioFrame;
	if (!O3DSockets::Tcp::DeserializeAudioFramePayload(Payload.GetData(), Payload.Num(), AudioFrame))
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Failed to deserialize audio frame"));
		return false;
	}

	if (TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> SinkPinned = AudioSink.Pin())
	{
		SinkPinned->SubmitPcm16(AudioFrame.Meta, AudioFrame.PCM16.GetData(), AudioFrame.PCM16.Num());
	}

	Stats.BytesReceived += Payload.Num();
	return true;
}
