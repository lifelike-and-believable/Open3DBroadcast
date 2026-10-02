// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsUdpReceiver.h"

#include "Transport/O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
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
	BindEndpoint = FO3DHostPort();
	BindPort = 0;
	StreamId = ActiveConfig.StreamId;
	ActiveAudioConfig = Config.Audio;
	const TMap<FString, FString>& Options = Config.AdvancedParams;
	bAllowBroadcast = O3DTransportOptions::GetBool(Options, O3DSockets::BroadcastOptionKey, false);
	MaxDatagramBytes = O3DTransportOptions::GetInt(Options, O3DSockets::MaxDatagramOptionKey, 64000, 512, 65507);
	MtuBytes = FMath::Clamp(O3DTransportOptions::GetInt(Options, O3DSockets::MtuOptionKey, 1200), 256, MaxDatagramBytes);
	MaxFrameBytes = O3DTransportOptions::GetInt(Options, O3DSockets::MaxFrameOptionKey, FReceiverConstants::DefaultMaxFrameBytes,
		FReceiverConstants::MaxUdpDatagramBytes, FReceiverConstants::MaxFrameBytesLimit);

	FragmentState = MakeUnique<FFragmentState>(MakeUdpReassemblyConfig(MaxFrameBytes));

	FO3DHostPort Parsed;
	if (!O3DSockets::ParseEndpoint(Config, TEXT("udp"), Parsed))
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP receiver requires udp://host:port URI or explicit host/port options."));
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("UDP receiver requires a udp://host:port URI or explicit host/port options."));
	}
	BindEndpoint = Parsed;
	BindPort = Parsed.Port;

	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(Parsed.Host, Parsed.Port);
		ActiveConfig.StreamId = StreamId;
	}

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("UDP receiver could not access socket subsystem."));
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("UDP receiver could not access the socket subsystem."));
	}

	FO3DReceiveDemuxSettings DemuxSettings = Demux.GetSettings();
	DemuxSettings.StreamId = StreamId;
	Demux.SetSettings(DemuxSettings);
	Demux.ResetStats();

	ReceiveBuffer.Reset();
	return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DSocketsUdpReceiver::Start()
{
	DestroySocket();
	if (BindPort <= 0)
	{
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("UDP receiver Start() before a successful Initialize()."));
	}
	if (!Demux.HasConsumer())
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
	DestroySocket();
	Demux.ReleaseSinks();
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

		// One classification for every message (ADR 0007 item 7): mocap to the consumer, audio to
		// the audio sink, control to the control sink; malformed input is counted and dropped.
		switch (Demux.ProcessMessage(Frame.GetData(), Frame.Num(), FPlatformTime::Seconds()))
		{
		case EO3DDemuxResult::Mocap:
			Stats.FramesReceived++;
			Stats.BytesReceived += Frame.Num();
			++FramesProcessed;
			break;
		case EO3DDemuxResult::Audio:
			++FramesProcessed;
			break;
		case EO3DDemuxResult::AudioRejected:
		case EO3DDemuxResult::Malformed:
		case EO3DDemuxResult::Oversize:
			Stats.ReceiveErrors++;
			break;
		default:
			break; // control (not a frame), keepalives, unknown kinds
		}
		if (!Socket)
		{
			break; // A consumer stopped this receiver from inside SubmitFrame.
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
	Demux.SetAudioSink(Sink);
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

	// "" and "*" bind every interface, "localhost" is 127.0.0.1 (as before WP-A1 PR 4c); anything
	// else must be an IP literal, so nothing resolves a name on the game thread (TRB-26).
	FO3DHostPort BindTarget = BindEndpoint;
	if (BindTarget.Host.IsEmpty() || BindTarget.Host == TEXT("*"))
	{
		BindTarget.Host = TEXT("0.0.0.0");
		BindTarget.bIPv6 = false;
	}
	else if (BindTarget.Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
	{
		BindTarget.Host = TEXT("127.0.0.1");
		BindTarget.bIPv6 = false;
	}
	TSharedPtr<FInternetAddr> BindAddrPtr;
	if (!O3DTransportOptions::IsIpLiteral(BindTarget) || !O3DTransportOptions::ResolveHostPort(BindTarget, BindAddrPtr) || !BindAddrPtr.IsValid())
	{
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Invalid UDP bind host '%s'."), *BindEndpoint.Host);
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("Invalid UDP bind host '%s'."), *BindEndpoint.Host));
	}
	FInternetAddr& BindAddr = *BindAddrPtr;

	// The socket's protocol follows the bind address, so an IPv6 bind works.
	Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UDP_RECEIVER"), BindAddr.GetProtocolType());
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

	if (!Socket->Bind(BindAddr))
	{
		const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
		UE_LOG(LogSocketsUdpReceiver, Warning, TEXT("Failed to bind UDP socket to %s."), *BindAddr.ToString(true));
		const FString Message = FString::Printf(TEXT("Failed to bind the UDP socket to %s (socket error %d)."), *BindAddr.ToString(true), static_cast<int32>(Error));
		DestroySocket();
		return FO3DTransportResult::Error(Error == SE_EADDRINUSE ? EO3DTransportError::AddressInUse : EO3DTransportError::ConnectFailed, Message);
	}

	int32 RequestedSize = 2 * 1024 * 1024;
	int32 AppliedSize = 0;
	Socket->SetReceiveBufferSize(RequestedSize, AppliedSize);

	ReceiveBuffer.SetNum(FReceiverConstants::MaxUdpDatagramBytes);
	// Same protocol as the socket, so RecvFrom can fill it for an IPv6 bind too.
	TSharedPtr<FInternetAddr> SenderAddrScratch = SocketSubsystem->CreateInternetAddr(BindAddr.GetProtocolType());
	RecvAddr = SenderAddrScratch;

	UE_LOG(LogSocketsUdpReceiver, Log, TEXT("UDP receiver listening on %s:%d (broadcast=%d, recvBuf=%d)."),
		*BindAddr.ToString(false), BindAddr.GetPort(), bAllowBroadcast ? 1 : 0, AppliedSize);

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

#endif // O3D_WITH_TRANSPORT_SOCKETS
