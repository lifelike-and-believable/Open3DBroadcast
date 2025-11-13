#include "SocketsTcpReceiver.h"
#include "SocketsTcpAudio.h"
#include "SocketsTcpTransport.h"
#include "O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"
#include "O3DAudioSerialization.h"
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

	// Read connection timeout setting (defaults to 5.0 seconds)
	ConnectionTimeoutSeconds = FMath::Max(0.5, O3DSockets::GetIntOption(Config, O3DSockets::TimeoutOptionKey, 5));

	return true;
}

void FO3DSocketsTcpReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	Consumer = InConsumer;
}

bool FO3DSocketsTcpReceiver::Start()
{
	DisconnectSocket();
	return ConnectToServer();
}

void FO3DSocketsTcpReceiver::Stop()
{
	DisconnectSocket();
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
				LastDataReceiveTime = FPlatformTime::Seconds();

				// Process received payload through unified demultiplexer
				if (ProcessReceivedPayload(Frame.GetData(), Frame.Num()))
				{
					++FramesProcessed;
				}
			}
		}
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
	LastDataReceiveTime = FPlatformTime::Seconds();
	ConnectBackoffAttempt = 0;

	UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver connecting to %s:%d"), *RemoteHost, RemotePort);
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
		else
		{
			// Check for timeout - if no data received for too long, assume stale connection
			const double Now = FPlatformTime::Seconds();
			if ((Now - LastDataReceiveTime) > ConnectionTimeoutSeconds)
			{
				UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP connection timeout (no data for %.1fs), forcing reconnect"), Now - LastDataReceiveTime);
				DisconnectSocket();
			}
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

bool FO3DSocketsTcpReceiver::ProcessReceivedPayload(const uint8* Data, int32 Size)
{
	if (!Data || Size <= 0)
	{
		return false;
	}

	// Try to parse as unified message
	O3DS::FUnifiedHeader Header;
	const uint8* PayloadPtr = nullptr;
	int32 PayloadSize = 0;

	if (O3DS::ParseUnifiedMessage(Data, Size, Header, PayloadPtr, PayloadSize))
	{
		// Unified message - route by kind
		if (Header.GetKind() == O3DS::EUnifiedKind::Audio)
		{
			return ProcessAudioPayload(Header.GetCodec(), PayloadPtr, PayloadSize);
		}
		else if (Header.GetKind() == O3DS::EUnifiedKind::Mocap)
		{
			// Route to frame consumer
			if (TSharedPtr<ISerializedFrameConsumer> ConsumerPinned = Consumer.Pin())
			{
				TArray<uint8> PayloadCopy;
				PayloadCopy.SetNumUninitialized(PayloadSize);
				FMemory::Memcpy(PayloadCopy.GetData(), PayloadPtr, PayloadSize);
				ConsumerPinned->SubmitFrame(StreamId, PayloadCopy, FPlatformTime::Seconds());
			}
			Stats.FramesReceived++;
			Stats.BytesReceived += Size;
			return true;
		}
	}
	else
	{
		// Backward compatibility: treat non-unified messages as mocap data
		if (TSharedPtr<ISerializedFrameConsumer> ConsumerPinned = Consumer.Pin())
		{
			TArray<uint8> PayloadCopy;
			PayloadCopy.SetNumUninitialized(Size);
			FMemory::Memcpy(PayloadCopy.GetData(), Data, Size);
			ConsumerPinned->SubmitFrame(StreamId, PayloadCopy, FPlatformTime::Seconds());
		}
		Stats.FramesReceived++;
		Stats.BytesReceived += Size;
		return true;
	}

	return false;
}

bool FO3DSocketsTcpReceiver::ProcessAudioPayload(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize)
{
	if (!Payload || PayloadSize <= 0)
	{
		return false;
	}

	O3DAudio::FEncodedAudioFrame EncodedAudio;
	if (!O3DAudio::DeserializeEncodedAudioFrame(Codec, Payload, PayloadSize, EncodedAudio))
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Failed to deserialize TCP audio frame (payloadSize=%d codec=%d)."), PayloadSize, static_cast<int32>(Codec));
		return false;
	}

	if (TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> SinkPinned = AudioSink.Pin())
	{
		if (Codec == O3DS::EUnifiedCodec::PCM16)
		{
			SinkPinned->SubmitPcm16(EncodedAudio.Meta, EncodedAudio.Payload.GetData(), EncodedAudio.Payload.Num());
		}
		else
		{
			if (!AudioDecoder.Decode(Codec, EncodedAudio.Meta, EncodedAudio.Payload.GetData(), EncodedAudio.Payload.Num(), DecodedPcmScratch))
			{
				UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Failed to decode TCP audio frame (codec=%d)."), static_cast<int32>(Codec));
				return false;
			}

			SinkPinned->SubmitPcm16(EncodedAudio.Meta,
				reinterpret_cast<const uint8*>(DecodedPcmScratch.GetData()),
				DecodedPcmScratch.Num() * sizeof(int16));
		}
	}

	return true;
}
