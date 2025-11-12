#include "SocketsTcpReceiver.h"
#include "SocketsTcpAudio.h"
#include "SocketsTcpTransport.h"

#include "O3DTransportTypes.h"
#include "SerializedFrameConsumerRegistry.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "Math/UnrealMathUtility.h"

DEFINE_LOG_CATEGORY_STATIC(LogSocketsTcpReceiver, Log, All);

namespace
{
	constexpr double InitialBackoffSeconds = 0.1;
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
	BackoffAttempt = 0;
	LastConnectAttempt = 0.0;
	bAnnouncedConnected = false;
	AudioBackoffAttempt = 0;
	LastAudioConnectAttempt = 0.0;
	bAudioAnnouncedConnected = false;
	ActiveAudioConfig = Config.Audio;

	if (!O3DSockets::ParseHostPort(Config, RemoteHost, RemotePort, TEXT("tcp")))
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver requires tcp://host:port URI or explicit host/port options."));
		return false;
	}

	StreamId = ActiveConfig.StreamId;
	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(RemoteHost, RemotePort);
		ActiveConfig.StreamId = StreamId;
	}

	AudioHost = O3DSockets::GetOptionValue(Config, O3DSockets::AudioHostOptionKey);
	if (AudioHost.IsEmpty())
	{
		AudioHost = RemoteHost;
	}

	AudioPort = O3DSockets::GetIntOption(Config, O3DSockets::AudioPortOptionKey, 0);
	if (AudioPort <= 0 && RemotePort > 0)
	{
		AudioPort = RemotePort + 1;
	}

	bAudioEnabled = ActiveAudioConfig.bEnableAudio;
	if (bAudioEnabled)
	{
		if (AudioPort <= 0)
		{
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Audio support requested but no valid audio port specified."));
			bAudioEnabled = false;
		}
		if (ActiveAudioConfig.StreamLabel.IsEmpty())
		{
			ActiveAudioConfig.StreamLabel = ActiveConfig.StreamId;
		}
	}

	ResetAudioState();

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	return SocketSubsystem != nullptr;
}

void FO3DSocketsTcpReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	Consumer = InConsumer;
}

bool FO3DSocketsTcpReceiver::Start()
{
	ResetState();
	ResetAudioState();

	const bool bStarted = BeginConnect();
	const bool bAudioStarted = !ShouldReceiveAudio() || BeginAudioConnect();
	return bStarted && bAudioStarted;
}

void FO3DSocketsTcpReceiver::Stop()
{
	DestroySocket();
	DestroyAudioSocket();
	SocketSubsystem = nullptr;
	ResetState();
	ResetAudioState();
}

int32 FO3DSocketsTcpReceiver::Poll()
{
	if (!SocketSubsystem)
	{
		return 0;
	}

	int32 FramesProcessed = 0;

	if (!EnsureSocket() || !Socket || Socket->GetConnectionState() != SCS_Connected)
	{
		return 0;
	}

	while (true)
	{
		if (State == EState::Sync)
		{
			BytesBuffered = 0;
			if (!ReadBytes(1))
			{
				break;
			}

			if (Buffer[0] != O3DSockets::Tcp::FrameMagic[0])
			{
				continue;
			}

			bool bMatches = true;
			for (int32 Index = 1; Index < O3DSockets::Tcp::FrameMagicSize; ++Index)
			{
				if (!ReadBytes(Index + 1))
				{
					bMatches = false;
					break;
				}

				if (Buffer[Index] != O3DSockets::Tcp::FrameMagic[Index])
				{
					bMatches = false;
					break;
				}
			}

			if (!bMatches)
			{
				BytesBuffered = 0;
				continue;
			}

			State = EState::Header;
		}

		if (State == EState::Header)
		{
			if (!ReadBytes(O3DSockets::Tcp::FrameHeaderSize))
			{
				break;
			}

			ExpectedPayloadSize = static_cast<int32>(O3DSockets::Tcp::DecodePayloadSize(Buffer.GetData()));
			if (ExpectedPayloadSize <= 0 || ExpectedPayloadSize > MaxPayloadSizeBytes)
			{
				UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Malformed TCP frame size %d"), ExpectedPayloadSize);
				HandleSocketError();
				break;
			}

			State = EState::Data;
		}

		if (State == EState::Data)
		{
			const int32 TotalRequired = O3DSockets::Tcp::FrameHeaderSize + ExpectedPayloadSize;
			if (!ReadBytes(TotalRequired))
			{
				break;
			}

			TArray<uint8> Frame;
			if (!ProcessFrame(ExpectedPayloadSize, Frame))
			{
				HandleSocketError();
				break;
			}

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

			State = EState::Sync;
			BytesBuffered = 0;
		}
	}

	PollAudioChannel(FramesProcessed);

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
		DestroyAudioSocket();
		bAudioEnabled = false;
		ResetAudioState();
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

	if (SocketSubsystem && ShouldReceiveAudio() && !AudioSocket)
	{
		BeginAudioConnect();
	}
}

bool FO3DSocketsTcpReceiver::EnsureSocket()
{
	if (Socket)
	{
		const ESocketConnectionState ConnState = Socket->GetConnectionState();
		if (ConnState == SCS_Connected)
		{
			if (!bAnnouncedConnected)
			{
				UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver connected to %s:%d."), *RemoteHost, RemotePort);
				bAnnouncedConnected = true;
				BackoffAttempt = 0;
			}
			return true;
		}

		if (ConnState == SCS_ConnectionError || (ConnState == SCS_NotConnected && bAnnouncedConnected))
		{
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP connection error detected; scheduling reconnect."));
			HandleSocketError();
		}
		else
		{
			return Socket != nullptr;
		}
	}

	const double Now = FPlatformTime::Seconds();
	const double BackoffSeconds = FMath::Min(MaxBackoffSeconds, InitialBackoffSeconds * FMath::Pow(2.0, static_cast<double>(FMath::Clamp(BackoffAttempt, 0, 8))));
	if (!Socket && (Now - LastConnectAttempt) >= BackoffSeconds)
	{
		LastConnectAttempt = Now;
		++BackoffAttempt;
		return BeginConnect();
	}

	return Socket != nullptr;
}

void FO3DSocketsTcpReceiver::DestroySocket()
{
	if (Socket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(Socket);
	}
	Socket = nullptr;
}

bool FO3DSocketsTcpReceiver::BeginConnect()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	DestroySocket();

	Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_CLIENT"), false);
	if (!Socket)
	{
		return false;
	}

	Socket->SetNonBlocking(true);

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bIsValid = false;
	Addr->SetIp(*RemoteHost, bIsValid);

	if (!bIsValid)
	{
		FIPv4Address IPv4;
		if (FIPv4Address::Parse(RemoteHost, IPv4))
		{
			Addr->SetIp(IPv4.Value);
			bIsValid = true;
		}
	}

	if (!bIsValid)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Invalid TCP host '%s'."), *RemoteHost);
		HandleSocketError();
		return false;
	}

	Addr->SetPort(RemotePort);

	bAnnouncedConnected = false;
	BytesBuffered = 0;
	State = EState::Sync;

	Socket->Connect(*Addr);
	return true;
}

bool FO3DSocketsTcpReceiver::ReadBytes(int32 TargetBytes)
{
	if (!Socket)
	{
		return false;
	}

	if (BytesBuffered < TargetBytes)
	{
		if (Buffer.Num() < TargetBytes)
		{
			Buffer.SetNum(TargetBytes);
		}

		int32 BytesRead = 0;
		if (!Socket->Recv(Buffer.GetData() + BytesBuffered, TargetBytes - BytesBuffered, BytesRead))
		{
			const ESocketErrors Error = SocketSubsystem ? SocketSubsystem->GetLastErrorCode() : SE_NO_ERROR;
			if (Error == SE_EWOULDBLOCK || Error == SE_NO_ERROR)
			{
				return false;
			}

			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP recv failed (error=%d)."), static_cast<int32>(Error));
			HandleSocketError();
			return false;
		}

		if (BytesRead == 0)
		{
			HandleSocketError();
			return false;
		}

		BytesBuffered += BytesRead;
	}

	return BytesBuffered >= TargetBytes;
}

bool FO3DSocketsTcpReceiver::ProcessFrame(int32 PayloadSize, TArray<uint8>& OutFrame)
{
	if (PayloadSize <= 0)
	{
		return false;
	}

	const int32 HeaderSize = O3DSockets::Tcp::FrameHeaderSize;
	if (BytesBuffered < HeaderSize + PayloadSize)
	{
		return false;
	}

	OutFrame.Reset(PayloadSize);
	OutFrame.SetNumUninitialized(PayloadSize);
	FMemory::Memcpy(OutFrame.GetData(), Buffer.GetData() + HeaderSize, PayloadSize);
	return true;
}

void FO3DSocketsTcpReceiver::HandleSocketError()
{
	DestroySocket();
	Stats.DroppedFrames++;
	BytesBuffered = 0;
	ExpectedPayloadSize = 0;
	State = EState::Sync;
	bAnnouncedConnected = false;
	LastConnectAttempt = 0.0;
}

void FO3DSocketsTcpReceiver::ResetState()
{
	Buffer.Reset();
	BytesBuffered = 0;
	ExpectedPayloadSize = 0;
	State = EState::Sync;
}

bool FO3DSocketsTcpReceiver::ShouldReceiveAudio() const
{
	return bAudioEnabled && AudioPort > 0 && AudioSink.Pin().IsValid();
}

void FO3DSocketsTcpReceiver::PollAudioChannel(int32& OutFramesProcessed)
{
	if (!ShouldReceiveAudio())
	{
		return;
	}

	if (!EnsureAudioSocket() || !AudioSocket || AudioSocket->GetConnectionState() != SCS_Connected)
	{
		return;
	}

	while (true)
	{
		if (AudioState == EState::Sync)
		{
			AudioBytesBuffered = 0;
			if (!ReadAudioBytes(1))
			{
				break;
			}

			if (AudioBuffer[0] != O3DSockets::Tcp::FrameMagic[0])
			{
				continue;
			}

			bool bMatches = true;
			for (int32 Index = 1; Index < O3DSockets::Tcp::FrameMagicSize; ++Index)
			{
				if (!ReadAudioBytes(Index + 1))
				{
					bMatches = false;
					break;
				}

				if (AudioBuffer[Index] != O3DSockets::Tcp::FrameMagic[Index])
				{
					bMatches = false;
					break;
				}
			}

			if (!bMatches)
			{
				AudioBytesBuffered = 0;
				continue;
			}

			AudioState = EState::Header;
		}

		if (AudioState == EState::Header)
		{
			if (!ReadAudioBytes(O3DSockets::Tcp::FrameHeaderSize))
			{
				break;
			}

			AudioExpectedPayloadSize = static_cast<int32>(O3DSockets::Tcp::DecodePayloadSize(AudioBuffer.GetData()));
			if (AudioExpectedPayloadSize <= 0 || AudioExpectedPayloadSize > MaxPayloadSizeBytes)
			{
				UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Malformed TCP audio frame size %d"), AudioExpectedPayloadSize);
				HandleAudioSocketError();
				break;
			}

			AudioState = EState::Data;
		}

		if (AudioState == EState::Data)
		{
			const int32 TotalRequired = O3DSockets::Tcp::FrameHeaderSize + AudioExpectedPayloadSize;
			if (!ReadAudioBytes(TotalRequired))
			{
				break;
			}

			if (!ProcessAudioFrame(AudioExpectedPayloadSize))
			{
				HandleAudioSocketError();
				break;
			}

			AudioState = EState::Sync;
			AudioBytesBuffered = 0;
			++OutFramesProcessed;
		}
	}
}

bool FO3DSocketsTcpReceiver::EnsureAudioSocket()
{
	if (!ShouldReceiveAudio())
	{
		DestroyAudioSocket();
		return false;
	}

	if (AudioSocket)
	{
		const ESocketConnectionState ConnState = AudioSocket->GetConnectionState();
		if (ConnState == SCS_Connected)
		{
			if (!bAudioAnnouncedConnected)
			{
				UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver audio connected to %s:%d."), *AudioHost, AudioPort);
				bAudioAnnouncedConnected = true;
				AudioBackoffAttempt = 0;
			}
			return true;
		}

		if (ConnState == SCS_ConnectionError || (ConnState == SCS_NotConnected && bAudioAnnouncedConnected))
		{
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP audio connection error detected; scheduling reconnect."));
			HandleAudioSocketError();
		}
		else
		{
			return AudioSocket != nullptr;
		}
	}

	const double Now = FPlatformTime::Seconds();
	const double BackoffSeconds = FMath::Min(MaxBackoffSeconds, InitialBackoffSeconds * FMath::Pow(2.0, static_cast<double>(FMath::Clamp(AudioBackoffAttempt, 0, 8))));
	if (!AudioSocket && (Now - LastAudioConnectAttempt) >= BackoffSeconds)
	{
		LastAudioConnectAttempt = Now;
		++AudioBackoffAttempt;
		return BeginAudioConnect();
	}

	return AudioSocket != nullptr;
}

bool FO3DSocketsTcpReceiver::BeginAudioConnect()
{
	if (!SocketSubsystem || AudioPort <= 0)
	{
		return false;
	}

	DestroyAudioSocket();

	AudioSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_AUDIO_CLIENT"), false);
	if (!AudioSocket)
	{
		return false;
	}

	AudioSocket->SetNonBlocking(true);

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bIsValid = false;
	Addr->SetIp(*AudioHost, bIsValid);

	if (!bIsValid)
	{
		FIPv4Address IPv4;
		if (FIPv4Address::Parse(AudioHost, IPv4))
		{
			Addr->SetIp(IPv4.Value);
			bIsValid = true;
		}
	}

	if (!bIsValid)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Invalid TCP audio host '%s'."), *AudioHost);
		HandleAudioSocketError();
		return false;
	}

	Addr->SetPort(AudioPort);

	bAudioAnnouncedConnected = false;
	AudioBytesBuffered = 0;
	AudioState = EState::Sync;

	AudioSocket->Connect(*Addr);
	return true;
}

void FO3DSocketsTcpReceiver::DestroyAudioSocket()
{
	if (AudioSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(AudioSocket);
	}
	AudioSocket = nullptr;
}

bool FO3DSocketsTcpReceiver::ReadAudioBytes(int32 TargetBytes)
{
	if (!AudioSocket)
	{
		return false;
	}

	if (AudioBytesBuffered < TargetBytes)
	{
		if (AudioBuffer.Num() < TargetBytes)
		{
			AudioBuffer.SetNum(TargetBytes);
		}

		int32 BytesRead = 0;
		if (!AudioSocket->Recv(AudioBuffer.GetData() + AudioBytesBuffered, TargetBytes - AudioBytesBuffered, BytesRead))
		{
			const ESocketErrors Error = SocketSubsystem ? SocketSubsystem->GetLastErrorCode() : SE_NO_ERROR;
			if (Error == SE_EWOULDBLOCK || Error == SE_NO_ERROR)
			{
				return false;
			}

			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP audio recv failed (error=%d)."), static_cast<int32>(Error));
			HandleAudioSocketError();
			return false;
		}

		if (BytesRead == 0)
		{
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP audio recv returned zero bytes."));
			HandleAudioSocketError();
			return false;
		}

		AudioBytesBuffered += BytesRead;
	}

	return AudioBytesBuffered >= TargetBytes;
}

bool FO3DSocketsTcpReceiver::ProcessAudioFrame(int32 PayloadSize)
{
	if (PayloadSize <= 0)
	{
		return false;
	}

	const int32 HeaderSize = O3DSockets::Tcp::FrameHeaderSize;
	if (AudioBytesBuffered < HeaderSize + PayloadSize)
	{
		return false;
	}

	const uint8* PayloadPtr = AudioBuffer.GetData() + HeaderSize;
	O3DSockets::Tcp::FAudioFrame Frame;
	if (!O3DSockets::Tcp::DeserializeAudioFramePayload(PayloadPtr, PayloadSize, Frame))
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("Failed to deserialize TCP audio frame (payloadSize=%d)."), PayloadSize);
		return false;
	}

	if (TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> SinkPinned = AudioSink.Pin())
	{
		SinkPinned->SubmitPcm16(Frame.Meta, Frame.PCM16.GetData(), Frame.PCM16.Num());
	}

	Stats.BytesReceived += PayloadSize;
	return true;
}

void FO3DSocketsTcpReceiver::HandleAudioSocketError()
{
	UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("HandleAudioSocketError invoked (DroppedFrames=%llu)."), static_cast<unsigned long long>(Stats.DroppedFrames + 1));
	DestroyAudioSocket();
	Stats.DroppedFrames++;
	ResetAudioState();
	bAudioAnnouncedConnected = false;
	LastAudioConnectAttempt = 0.0;
}

void FO3DSocketsTcpReceiver::ResetAudioState()
{
	AudioBuffer.Reset();
	AudioBytesBuffered = 0;
	AudioExpectedPayloadSize = 0;
	AudioState = EState::Sync;
}
