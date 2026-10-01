// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsUdpReceiver.h"

#include "Transport/O3DTransportTypes.h"
#include "O3DAudioSerialization.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DSerializedFrameConsumer.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/udp_fragment.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogSocketsUdpReceiver, Log, All);

namespace
{
	struct FReceiverConstants
	{
		static constexpr int32 FragmentHeaderSize = static_cast<int32>(kUdpFragmentHeaderSize);
		// Largest possible UDP payload over IPv4; the receive buffer is always
		// this big so a sender with a larger udp.maxdatagram is never truncated.
		static constexpr int32 MaxUdpDatagramBytes = static_cast<int32>(kUdpMaxDatagramSize);
		// Default and hard upper limit for udp.maxframe (largest reassembled message).
		static constexpr int32 DefaultMaxFrameBytes = static_cast<int32>(kUdpDefaultMaxMessageSize);
		static constexpr int32 MaxFrameBytesLimit = 50 * 1024 * 1024;
		// Work bounds per Poll() so a flood cannot hold the game thread (TRB-18).
		// Anything left over stays in the socket buffer for the next Poll().
		static constexpr int32 MaxDatagramsPerPoll = 1024;
		static constexpr int64 MaxBytesPerPoll = 8 * 1024 * 1024;
		// In-flight reassembly limits (see UdpReassemblyConfig).
		static constexpr int32 MaxInFlightMessages = static_cast<int32>(kUdpDefaultMaxInFlight);
		// Longer than the core default: time is sampled when Poll() drains the
		// socket on the game thread, so a frame hitch must not expire a message
		// whose fragments straddle two polls. Stale ids are also dropped as soon
		// as a newer message from the same sender completes.
		static constexpr int32 FragmentTimeoutMs = 500;
		static constexpr int32 TotalReassemblyFrames = 4; // total budget = N x max frame size
	};

	uint64 UdpReceiverNowMilliseconds()
	{
		return static_cast<uint64>(FPlatformTime::Seconds() * 1000.0);
	}

	UdpReassemblyConfig MakeUdpReassemblyConfig(int32 MaxFrameBytes)
	{
		UdpReassemblyConfig ReassemblyConfig;
		ReassemblyConfig.maxMessageSize = static_cast<size_t>(MaxFrameBytes);
		ReassemblyConfig.maxInFlightMessages = static_cast<size_t>(FReceiverConstants::MaxInFlightMessages);
		ReassemblyConfig.maxTotalBytes = static_cast<size_t>(FReceiverConstants::TotalReassemblyFrames) * static_cast<size_t>(MaxFrameBytes);
		ReassemblyConfig.messageTimeoutMs = static_cast<uint64_t>(FReceiverConstants::FragmentTimeoutMs);
		return ReassemblyConfig;
	}
}

struct FO3DSocketsUdpReceiver::FFragmentState
{
	explicit FFragmentState(const UdpReassemblyConfig& InConfig)
		: Mapper(InConfig)
	{
	}

	UdpMapper Mapper;
};

FO3DSocketsUdpReceiver::FO3DSocketsUdpReceiver()
{
	MaxFrameBytes = FReceiverConstants::DefaultMaxFrameBytes;
	FragmentState = MakeUnique<FFragmentState>(MakeUdpReassemblyConfig(MaxFrameBytes));
}

FO3DSocketsUdpReceiver::~FO3DSocketsUdpReceiver()
{
	Stop();
}

FO3DTransportResult FO3DSocketsUdpReceiver::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	Stats.Reset();
	BindHost.Reset();
	BindPort = 0;
	StreamId = ActiveConfig.StreamId;
	ActiveAudioConfig = Config.Audio;
	bAllowBroadcast = O3DSockets::GetBoolOption(Config, O3DSockets::BroadcastOptionKey, false);
	MaxDatagramBytes = FMath::Clamp(O3DSockets::GetIntOption(Config, O3DSockets::MaxDatagramOptionKey, 64000), 512, 65507);
	MtuBytes = FMath::Clamp(O3DSockets::GetIntOption(Config, O3DSockets::MtuOptionKey, 1200), 256, MaxDatagramBytes);
	MaxFrameBytes = FMath::Clamp(O3DSockets::GetIntOption(Config, O3DSockets::MaxFrameOptionKey, FReceiverConstants::DefaultMaxFrameBytes),
		FReceiverConstants::MaxUdpDatagramBytes, FReceiverConstants::MaxFrameBytesLimit);

	FragmentState = MakeUnique<FFragmentState>(MakeUdpReassemblyConfig(MaxFrameBytes));

	if (!O3DSockets::ParseHostPort(Config, BindHost, BindPort, TEXT("udp")))
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP receiver requires udp://host:port URI or explicit host/port options."));
		BindPort = 0;
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("UDP receiver requires a udp://host:port URI or explicit host/port options."));
	}

	if (BindPort <= 0)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP receiver requires a valid port (got %d)."), BindPort);
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("UDP receiver requires a valid port (got %d)."), BindPort));
	}

	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(BindHost, BindPort);
		ActiveConfig.StreamId = StreamId;
	}

	// Note: Audio stream label is now automatically derived from StreamId

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP receiver could not access socket subsystem."));
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("UDP receiver could not access the socket subsystem."));
	}

	ReceiveBuffer.Reset();
	return FO3DTransportResult::Ok();
}

void FO3DSocketsUdpReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	Consumer = InConsumer;
}

FO3DTransportResult FO3DSocketsUdpReceiver::Start()
{
	DestroySocket();
	if (BindPort <= 0)
	{
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("UDP receiver Start() before a successful Initialize()."));
	}
	if (!Consumer.IsValid())
	{
		return FO3DTransportResult::Error(EO3DTransportError::NoConsumer, TEXT("UDP receiver Start() without a frame consumer (SetConsumer)."));
	}
	// Stop() drops SocketSubsystem; fetch it again so Start() after Stop() works without Initialize().
	if (!SocketSubsystem)
	{
		SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}

	const FO3DTransportResult Result = CreateSocket();
	if (!Result.IsOk())
	{
		ConnectionState.End(EO3DConnectionState::Failed, Result);
		return Result;
	}
	// Bound: datagrams from any sender are received from now on; UDP has no connection.
	ConnectionState.Begin(EO3DConnectionState::Connected);
	return Result;
}

void FO3DSocketsUdpReceiver::Stop()
{
	ControlSink.Reset();
	DestroySocket();
	SocketSubsystem = nullptr;
	ReceiveBuffer.Reset();
	RecvAddr.Reset();
	FragmentState = MakeUnique<FFragmentState>(MakeUdpReassemblyConfig(MaxFrameBytes));
	ConnectionState.End(EO3DConnectionState::Idle);
}

int32 FO3DSocketsUdpReceiver::Poll()
{
	if (!Socket || !SocketSubsystem || !RecvAddr.IsValid())
	{
		return 0;
	}

	int32 FramesProcessed = 0;

	if (ReceiveBuffer.Num() < FReceiverConstants::MaxUdpDatagramBytes)
	{
		ReceiveBuffer.SetNum(FReceiverConstants::MaxUdpDatagramBytes);
	}

	int64 BytesThisPoll = 0;
	for (int32 DatagramsThisPoll = 0;
		DatagramsThisPoll < FReceiverConstants::MaxDatagramsPerPoll && BytesThisPoll < FReceiverConstants::MaxBytesPerPoll;
		++DatagramsThisPoll)
	{
		int32 BytesRead = 0;
		if (!Socket->RecvFrom(ReceiveBuffer.GetData(), ReceiveBuffer.Num(), BytesRead, *RecvAddr))
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

		BytesThisPoll += BytesRead;

		TArray<uint8>& Frame = FrameScratch;
		if (!ProcessDatagram(ReceiveBuffer.GetData(), BytesRead, Frame, FragmentState))
		{
			continue;
		}

		if (Frame.Num() == 0)
		{
			// Fragment buffered, waiting for completion.
			continue;
		}

		if (Frame.Num() > MaxFrameBytes)
		{
			UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP payload exceeds safety cap (%d bytes). Dropping."), Frame.Num());
			continue;
		}

		// Process received payload through unified demultiplexer
		if (ProcessReceivedPayload(Frame.GetData(), Frame.Num()))
		{
			++FramesProcessed;
		}
	}

	return FramesProcessed;
}

FO3DTransportStats FO3DSocketsUdpReceiver::GetStats() const
{
	FO3DTransportStats Copy = Stats;
	Copy.State = ConnectionState.Get();
	return Copy;
}

void FO3DSocketsUdpReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
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

FO3DTransportResult FO3DSocketsUdpReceiver::CreateSocket()
{
	if (!SocketSubsystem)
	{
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("UDP receiver has no socket subsystem."));
	}

	DestroySocket();

	Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UDP_RECEIVER"), FNetworkProtocolTypes::IPv4);
	if (!Socket)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to create UDP socket."));
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("Failed to create the UDP socket."));
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
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("Invalid UDP bind host '%s'."), *BindHost));
	}

	BindAddr->SetPort(BindPort);

	if (!Socket->Bind(*BindAddr))
	{
		const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to bind UDP socket to %s."), *BindAddr->ToString(true));
		const FString Message = FString::Printf(TEXT("Failed to bind the UDP socket to %s (socket error %d)."), *BindAddr->ToString(true), static_cast<int32>(Error));
		DestroySocket();
		return FO3DTransportResult::Error(Error == SE_EADDRINUSE ? EO3DTransportError::AddressInUse : EO3DTransportError::ConnectFailed, Message);
	}

	int32 RequestedSize = 2 * 1024 * 1024;
	int32 AppliedSize = 0;
	Socket->SetReceiveBufferSize(RequestedSize, AppliedSize);

	ReceiveBuffer.SetNum(FReceiverConstants::MaxUdpDatagramBytes);
	TSharedPtr<FInternetAddr> SenderAddrScratch = SocketSubsystem->CreateInternetAddr();
	RecvAddr = SenderAddrScratch;

	UE_LOG(LogSocketsUdpReceiver, Log, TEXT("UDP receiver listening on %s:%d (broadcast=%d, recvBuf=%d)."),
		*BindAddr->ToString(false), BindAddr->GetPort(), bAllowBroadcast ? 1 : 0, AppliedSize);

	return FO3DTransportResult::Ok();
}

void FO3DSocketsUdpReceiver::DestroySocket()
{
	if (Socket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(Socket);
	}
	Socket = nullptr;
}

bool FO3DSocketsUdpReceiver::ProcessDatagram(const uint8* Data, int32 Bytes, TArray<uint8>& OutFrame, TUniquePtr<FFragmentState>& InState)
{
	OutFrame.Reset();

	if (Data == nullptr || Bytes <= 0)
	{
		return false;
	}

	// Policy check only: the receive buffer itself is always large enough
	// for any UDP datagram, so nothing is silently truncated (TRB-23).
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
		InState = MakeUnique<FFragmentState>(MakeUdpReassemblyConfig(MaxFrameBytes));
	}

	// Reassembly is keyed on (sender, message id) so two senders reusing the
	// same ids cannot corrupt each other's messages.
	uint64 SourceKey = 0;
	if (RecvAddr.IsValid())
	{
		SourceKey = (static_cast<uint64>(GetTypeHash(RecvAddr->ToString(false))) << 32)
			| static_cast<uint64>(static_cast<uint32>(RecvAddr->GetPort()));
	}

	if (!InState->Mapper.addFragment(SourceKey, reinterpret_cast<const char*>(Data), static_cast<size_t>(Bytes), UdpReceiverNowMilliseconds()))
	{
		UE_LOG(LogSocketsUdpReceiver, Verbose, TEXT("Discarded malformed UDP fragment (size=%d)."), Bytes);
		return false;
	}

	std::vector<char>& Combined = CombinedScratch;
	if (InState->Mapper.getFrame(Combined))
	{
		if (Combined.size() > static_cast<size_t>(MaxFrameBytes))
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

	UdpFragmentHeader Header;
	if (!readUdpFragmentHeader(reinterpret_cast<const char*>(Data), static_cast<size_t>(Bytes), Header))
	{
		return false;
	}
	const uint32 Sequence = Header.seq;
	const uint32 TotalSize = Header.totalSize;
	const uint32 FragmentSize = Header.fragSize;

	if (FragmentSize == 0 || FragmentSize > static_cast<uint32>(MaxDatagramBytes))
	{
		return false;
	}

	if (TotalSize == 0 || TotalSize > static_cast<uint32>(MaxFrameBytes))
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

bool FO3DSocketsUdpReceiver::ProcessReceivedPayload(const uint8* Data, int32 Size)
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
		if (Header.GetKind() == O3DS::EUnifiedKind::Control)
		{
			// ADR 0011: to the control sink if well-formed, otherwise dropped; never a frame.
			O3DTransport::DeliverControlEnvelope(ControlSink, Data, Size, StreamId);
			return false;
		}
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

bool FO3DSocketsUdpReceiver::ProcessAudioPayload(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize)
{
	if (!Payload || PayloadSize <= 0)
	{
		return false;
	}

	O3DAudio::FEncodedAudioFrame EncodedFrame;
	if (!O3DAudio::DeserializeEncodedAudioFrame(Codec, Payload, PayloadSize, EncodedFrame))
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to deserialize UDP audio frame (payloadSize=%d codec=%d)."), PayloadSize, static_cast<int32>(Codec));
		return false;
	}

	if (TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> SinkPinned = AudioSink.Pin())
	{
		if (Codec == O3DS::EUnifiedCodec::PCM16)
		{
			SinkPinned->SubmitPcm16(EncodedFrame.Meta, EncodedFrame.Payload.GetData(), EncodedFrame.Payload.Num());
		}
		else
		{
			if (!AudioDecoder.Decode(Codec, EncodedFrame.Meta, EncodedFrame.Payload.GetData(), EncodedFrame.Payload.Num(), DecodedPcmScratch))
			{
				UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to decode UDP audio frame (codec=%d)."), static_cast<int32>(Codec));
				return false;
			}

			SinkPinned->SubmitPcm16(EncodedFrame.Meta,
				reinterpret_cast<const uint8*>(DecodedPcmScratch.GetData()),
				DecodedPcmScratch.Num() * sizeof(int16));
		}
	}

	return true;
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
