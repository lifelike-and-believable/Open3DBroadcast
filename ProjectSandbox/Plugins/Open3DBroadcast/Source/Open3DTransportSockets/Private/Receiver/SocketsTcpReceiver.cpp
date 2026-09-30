// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsTcpReceiver.h"
#include "../Shared/SocketsTcpAudio.h"
#include "../Shared/SocketsTcpTransport.h"
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
	/** Bytes requested from the socket per Recv call. */
	constexpr int32 RecvChunkBytes = 64 * 1024;
	/** Work bounds per Poll() so a burst cannot hold the game thread (TRB-18); the rest stays in the socket buffer. */
	constexpr int32 MaxFramesPerPoll = 256;
	constexpr int64 MaxBytesPerPoll = 8 * 1024 * 1024;
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

	// Note: Audio stream label is now automatically derived from StreamId

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver could not access socket subsystem."));
		return false;
	}

	namespace Tcp = O3DSockets::Tcp;

	// Idle timeout: no data (frames or keepalives) for this long forces a reconnect.
	ConnectionTimeoutSeconds = FMath::Max(0.5, static_cast<double>(O3DSockets::GetIntOption(Config, O3DSockets::TimeoutOptionKey, 5)));
	// TRB-5: a connect that has not completed after this long is abandoned and retried.
	ConnectTimeoutSeconds = FMath::Max(0.5, static_cast<double>(O3DSockets::GetIntOption(Config, Tcp::ConnectTimeoutOptionKey, Tcp::DefaultConnectTimeoutSeconds)));
	// TRB-4: exponential reconnect backoff.
	InitialBackoffSeconds = FMath::Clamp(O3DSockets::GetIntOption(Config, Tcp::BackoffOptionKey, Tcp::DefaultBackoffMs), 10, 60000) / 1000.0;
	MaxBackoffSeconds = FMath::Max(InitialBackoffSeconds, FMath::Clamp(O3DSockets::GetIntOption(Config, Tcp::MaxBackoffOptionKey, Tcp::DefaultMaxBackoffMs), 10, 600000) / 1000.0);
	// TRB-9: largest accepted frame.
	const int32 MaxFrameBytes = FMath::Clamp(O3DSockets::GetIntOption(Config, Tcp::MaxFrameOptionKey, Tcp::DefaultMaxFrameBytes), Tcp::MinFrameBytes, Tcp::MaxFrameBytesLimit);
	Parser.setMaxPayloadBytes(static_cast<size_t>(MaxFrameBytes));

	return true;
}

void FO3DSocketsTcpReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	Consumer = InConsumer;
}

bool FO3DSocketsTcpReceiver::Start()
{
	DisconnectSocket();

	// TRB-13: Stop() keeps SocketSubsystem, but fetch it again in case Initialize() was skipped.
	if (!SocketSubsystem)
	{
		SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}
	if (!SocketSubsystem || RemotePort <= 0)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver cannot start: not initialized."));
		return false;
	}

	ConnectBackoffAttempt = 0;
	ConnectCount = 0;
	bRunning = ConnectToServer();
	return bRunning;
}

void FO3DSocketsTcpReceiver::Stop()
{
	bRunning = false;
	DisconnectSocket();
}

int32 FO3DSocketsTcpReceiver::Poll()
{
	if (!bRunning || !SocketSubsystem)
	{
		return 0;
	}

	TickConnection();

	int32 FramesProcessed = 0;
	int64 BytesRead = 0;

	if (Socket && State == EState::Connected)
	{
		if (!ReadAvailable(FramesProcessed, BytesRead))
		{
			UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP connection to %s:%d closed by peer, will reconnect"), *RemoteHost, RemotePort);
			ReportParserStats();
			DisconnectSocket();
		}
		else
		{
			ReportParserStats();
		}
	}

	return FramesProcessed;
}

bool FO3DSocketsTcpReceiver::ReadAvailable(int32& InOutFramesProcessed, int64& InOutBytesRead)
{
	// TRB-1: pop every complete frame already buffered before reading more, and read only
	// when nothing complete is left.
	while (InOutFramesProcessed < MaxFramesPerPoll && InOutBytesRead < MaxBytesPerPoll)
	{
		if (!Socket)
		{
			return true; // A consumer stopped this receiver from inside SubmitFrame.
		}

		const uint8_t* Payload = nullptr;
		size_t PayloadSize = 0;
		if (Parser.next(Payload, PayloadSize))
		{
			LastDataReceiveTime = FPlatformTime::Seconds();
			if (!bReceivedOnThisConnection)
			{
				// The connection carries data: only now does the backoff start over (TRB-4).
				bReceivedOnThisConnection = true;
				ConnectBackoffAttempt = 0;
			}
			if (ProcessReceivedPayload(Payload, static_cast<int32>(PayloadSize)))
			{
				++InOutFramesProcessed;
			}
			continue;
		}

		uint8* Destination = Parser.prepareWrite(RecvChunkBytes);
		int32 Read = 0;
		const bool bOk = Socket->Recv(Destination, RecvChunkBytes, Read);
		if (bOk && Read > 0)
		{
			Parser.commitWrite(static_cast<size_t>(Read));
			InOutBytesRead += Read;
			continue;
		}

		if (!bOk)
		{
			const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
			if (Error != SE_EWOULDBLOCK && Error != SE_NO_ERROR)
			{
				return false; // Reset, aborted or another hard error.
			}
		}

		// Nothing read. A stream socket that reports readable but then yields no bytes has
		// reached end of stream (the sender closed or restarted) or failed. The error code is
		// not used here: after an orderly close it can still hold a stale EWOULDBLOCK. The
		// second Recv means a byte arriving between the two calls is not mistaken for a close.
		if (Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
		{
			Read = 0;
			Destination = Parser.prepareWrite(RecvChunkBytes);
			if (Socket->Recv(Destination, RecvChunkBytes, Read) && Read > 0)
			{
				Parser.commitWrite(static_cast<size_t>(Read));
				InOutBytesRead += Read;
				continue;
			}
			return false;
		}
		break;
	}

	return true;
}

void FO3DSocketsTcpReceiver::ReportParserStats()
{
	const O3DS::TcpStreamParserStats& ParserStats = Parser.stats();
	if (ParserStats.rejectedFrames > ReportedRejectedFrames)
	{
		Stats.DroppedFrames += static_cast<int64>(ParserStats.rejectedFrames - ReportedRejectedFrames);
		ReportedRejectedFrames = ParserStats.rejectedFrames;
	}

	if (ParserStats.discardedBytes > ReportedDiscardedBytes)
	{
		const uint64 NewBytes = ParserStats.discardedBytes - ReportedDiscardedBytes;
		ReportedDiscardedBytes = ParserStats.discardedBytes;
		if (!bWarnedResyncThisConnection)
		{
			bWarnedResyncThisConnection = true;
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP stream from %s:%d lost framing: skipped %llu bytes to the next frame header (rejected frames so far: %llu, tcp.maxframe=%llu). Further resyncs on this connection are logged at Verbose."),
				*RemoteHost, RemotePort, static_cast<unsigned long long>(NewBytes), static_cast<unsigned long long>(ParserStats.rejectedFrames), static_cast<unsigned long long>(Parser.maxPayloadBytes()));
		}
		else
		{
			UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP stream resync: skipped %llu bytes"), static_cast<unsigned long long>(NewBytes));
		}
	}
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
	// Note: Audio stream label is now automatically derived from StreamId

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

	// Configure socket buffers for better performance
	int32 ReceiveBufferSize = 2 * 1024 * 1024; // 2MB (match UDP)
	int32 AppliedSize = 0;
	Socket->SetReceiveBufferSize(ReceiveBufferSize, AppliedSize);

	// Disable Nagle's algorithm for low-latency transmission (critical for audio)
	Socket->SetNoDelay(true);

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

	Socket->Connect(*Addr);
	const double Now = FPlatformTime::Seconds();
	LastConnectAttempt = Now;
	ConnectStartTime = Now;
	LastDataReceiveTime = Now;
	// TRB-4: the attempt counter is not reset here. It is reset only once a connection
	// delivers data, so an unreachable or immediately-closing sender backs off to the maximum.

	if (ConnectBackoffAttempt == 0)
	{
		UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver connecting to %s:%d (recvBuf=%d, TCP_NODELAY=true)"), *RemoteHost, RemotePort, AppliedSize);
	}
	else
	{
		UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP receiver reconnect attempt %d to %s:%d"), ConnectBackoffAttempt, *RemoteHost, RemotePort);
	}
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
	// A new connection starts on a frame boundary; never splice bytes from the old one.
	Parser.reset();
	bWarnedResyncThisConnection = false;
	bReceivedOnThisConnection = false;
}

double FO3DSocketsTcpReceiver::GetBackoffSeconds() const
{
	const int32 Exponent = FMath::Clamp(ConnectBackoffAttempt, 0, 16);
	return FMath::Min(MaxBackoffSeconds, InitialBackoffSeconds * FMath::Pow(2.0, static_cast<double>(Exponent)));
}

void FO3DSocketsTcpReceiver::TickConnection()
{
	const double Now = FPlatformTime::Seconds();

	if (!Socket)
	{
		// Reconnect after an exponentially growing delay (TRB-4). The first retry after a
		// dropped connection waits InitialBackoffSeconds.
		if ((Now - LastConnectAttempt) >= GetBackoffSeconds())
		{
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
			LastDataReceiveTime = Now;
			++ConnectCount;
			UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver connected to %s:%d"), *RemoteHost, RemotePort);
		}
		else if (ConnState == SCS_ConnectionError)
		{
			UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP connection error, will retry"));
			DisconnectSocket();
		}
		else if ((Now - ConnectStartTime) > ConnectTimeoutSeconds)
		{
			// TRB-5: a SYN that is never answered (firewall, wrong subnet) must not leave the
			// receiver in Connecting forever.
			UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP connect to %s:%d timed out after %.1fs, will retry"), *RemoteHost, RemotePort, Now - ConnectStartTime);
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
		else if ((Now - LastDataReceiveTime) > ConnectionTimeoutSeconds)
		{
			// No frames and no keepalives: treat the connection as dead. A sender that is only
			// idle sends keepalives (TRB-6).
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP connection timeout (no data for %.1fs), forcing reconnect"), Now - LastDataReceiveTime);
			DisconnectSocket();
		}
	}
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
		if (PayloadSize == 0)
		{
			// Sender keepalive: an envelope with no payload (TRB-6). ReadAvailable() already
			// refreshed the idle timer; nothing else to do.
			return false;
		}

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

#endif // O3D_WITH_TRANSPORT_SOCKETS
