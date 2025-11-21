// Copyright (c) Open3DStream Contributors

#include "Shared/QuicConnection.h"

#include "Open3DTransportQUICLog.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace O3DQuic
{
namespace
{
class FMsQuicApi
{
public:
	static const QUIC_API_TABLE* Get()
	{
		static FMsQuicApi Instance;
		return Instance.Api;
	}

private:
	FMsQuicApi()
	{
		const QUIC_STATUS Status = MsQuicOpen2(&Api);
		if (QUIC_FAILED(Status))
		{
			Api = nullptr;
			UE_LOG(LogOpen3DTransportQUIC, Error, TEXT("MsQuicOpen2 failed (0x%08x)"), Status);
		}
	}

	~FMsQuicApi()
	{
		if (Api)
		{
			MsQuicClose(Api);
			Api = nullptr;
		}
	}

private:
	const QUIC_API_TABLE* Api = nullptr;
};

void BuildAlpnBuffer(const FString& AlpnString, QUIC_BUFFER& OutBuffer, TArray<uint8>& Scratch)
{
	if (!AlpnString.IsEmpty())
	{
		FTCHARToUTF8 Converted(*AlpnString);
		Scratch.SetNum(Converted.Length());
		FMemory::Memcpy(Scratch.GetData(), Converted.Get(), Scratch.Num());
	}
	else
	{
		static const ANSICHAR DefaultAlpnAnsi[] = "moq-00";
		Scratch.SetNum(sizeof(DefaultAlpnAnsi) - 1);
		FMemory::Memcpy(Scratch.GetData(), DefaultAlpnAnsi, Scratch.Num());
	}

	OutBuffer.Buffer = Scratch.GetData();
	OutBuffer.Length = Scratch.Num();
}

bool BuildLocalAddress(const FString& Host, uint16 Port, QUIC_ADDR& OutAddr)
{
	FTCHARToUTF8 HostUtf8(*Host);
	return QuicAddrFromString(HostUtf8.Get(), Port, &OutAddr);
}

FString StatusToString(QUIC_STATUS Status)
{
	return FString::Printf(TEXT("0x%08x"), Status);
}

bool IsValidFrameType(uint8 Value)
{
	return Value == static_cast<uint8>(EQuicFrameType::Control) || Value == static_cast<uint8>(EQuicFrameType::Object);
}
}

FQuicConnection::FQuicConnection() = default;

FQuicConnection::~FQuicConnection()
{
	Shutdown();
}

bool FQuicConnection::InitializeServer(const FQuicSenderOptions& Options, IQuicConnectionListener* InListener, FString& OutError)
{
	FScopeLock Lock(&Guard);

	if (bInitialized)
	{
		OutError = TEXT("QUIC connection already initialized");
		return false;
	}

	if (InListener == nullptr)
	{
		OutError = TEXT("A connection listener is required for MsQuic server mode.");
		return false;
	}

	Api = FMsQuicApi::Get();
	if (!Api)
	{
		OutError = TEXT("MsQuic API table unavailable");
		return false;
	}

	Listener = InListener;
	ActiveOptions = Options;
	SubscribersByConnection.Empty();
	SubscribersById.Empty();
	NextSubscriberId = 1;

	if (!OpenRegistration(OutError))
	{
		Shutdown();
		return false;
	}

	if (!OpenConfiguration(Options, OutError))
	{
		Shutdown();
		return false;
	}

	if (!StartListener(Options, OutError))
	{
		Shutdown();
		return false;
	}

	bInitialized = true;
	return true;
}

void FQuicConnection::Shutdown()
{
	TArray<TSharedPtr<FSubscriberContext>> Disconnected;
	{
		FScopeLock Lock(&Guard);
		SubscribersByConnection.GenerateValueArray(Disconnected);
		SubscribersByConnection.Empty();
		SubscribersById.Empty();
	}

	for (const TSharedPtr<FSubscriberContext>& Subscriber : Disconnected)
	{
		if (Subscriber.IsValid() && Listener)
		{
			Listener->OnQuicSubscriberDisconnected(Subscriber->SubscriberId, TEXT("Server shutting down"));
		}
	}

	FScopeLock Lock(&Guard);
	CloseListener();
	CloseConfiguration();
	CloseRegistration();
	Listener = nullptr;
	Api = nullptr;
	bInitialized = false;
	bWarnedUnreliableFallback = false;
}

bool FQuicConnection::IsInitialized() const
{
	FScopeLock Lock(&Guard);
	return bInitialized;
}

bool FQuicConnection::SendControlMessage(uint32 SubscriberId, const uint8* Data, uint32 Length, FString& OutError)
{
	FScopeLock Lock(&Guard);
	TSharedPtr<FSubscriberContext> Subscriber = GetSubscriberById(SubscriberId);
	return SendFrameInternal(Subscriber, EQuicFrameType::Control, Data, Length, OutError);
}

bool FQuicConnection::SendObjectMessage(uint32 SubscriberId, const uint8* Data, uint32 Length, O3DMoQ::EMoQReliabilityMode Reliability, FString& OutError)
{
	FScopeLock Lock(&Guard);
	TSharedPtr<FSubscriberContext> Subscriber = GetSubscriberById(SubscriberId);
	if (Reliability == O3DMoQ::EMoQReliabilityMode::UnreliableSequenced)
	{
		if (ActiveOptions.bEnableDatagrams)
		{
			return SendDatagramInternal(Subscriber, Data, Length, OutError);
		}
		else if (!bWarnedUnreliableFallback)
		{
			bWarnedUnreliableFallback = true;
			UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("QUIC track '%s' requested unreliable delivery but datagrams are disabled; falling back to reliable streams."), *ActiveOptions.TrackName);
		}
	}
	return SendFrameInternal(Subscriber, EQuicFrameType::Object, Data, Length, OutError);
}

bool FQuicConnection::SendDatagramInternal(const TSharedPtr<FSubscriberContext>& Subscriber, const uint8* Data, uint32 Length, FString& OutError)
{
	if (!Subscriber.IsValid() || !Subscriber->ConnectionHandle)
	{
		OutError = TEXT("Subscriber unavailable");
		return false;
	}

	if (!Api)
	{
		OutError = TEXT("MsQuic API unavailable");
		return false;
	}

	FDatagramSendState* SendState = new FDatagramSendState();
	SendState->Buffer.SetNum(static_cast<int32>(Length) + 1);
	SendState->Buffer[0] = static_cast<uint8>(EQuicFrameType::Object);
	if (Length > 0)
	{
		FMemory::Memcpy(SendState->Buffer.GetData() + 1, Data, Length);
	}

	QUIC_BUFFER Buffer{};
	Buffer.Buffer = SendState->Buffer.GetData();
	Buffer.Length = SendState->Buffer.Num();

	const QUIC_STATUS Status = Api->DatagramSend(Subscriber->ConnectionHandle, &Buffer, 1, QUIC_SEND_FLAG_NONE, SendState);
	if (QUIC_FAILED(Status))
	{
		delete SendState;
		OutError = FString::Printf(TEXT("DatagramSend failed (%s)"), *StatusToString(Status));
		return false;
	}

	return true;
}

int32 FQuicConnection::GetActiveSubscriberCount() const
{
	FScopeLock Lock(&Guard);
	return SubscribersById.Num();
}

bool FQuicConnection::OpenRegistration(FString& OutError)
{
	if (RegistrationHandle)
	{
		return true;
	}

	QUIC_REGISTRATION_CONFIG Config = {};
	Config.AppName = "Open3DTransportQUIC";
	Config.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;

	const QUIC_STATUS Status = Api->RegistrationOpen(&Config, &RegistrationHandle);
	if (QUIC_FAILED(Status))
	{
		OutError = FString::Printf(TEXT("MsQuic RegistrationOpen failed (%s)"), *StatusToString(Status));
		return false;
	}

	return true;
}

bool FQuicConnection::OpenConfiguration(const FQuicSenderOptions& Options, FString& OutError)
{
	if (ConfigurationHandle)
	{
		return true;
	}

	QUIC_SETTINGS Settings;
	FMemory::Memzero(Settings);
	Settings.IsSet.DatagramReceiveEnabled = TRUE;
	Settings.DatagramReceiveEnabled = Options.bEnableDatagrams ? TRUE : FALSE;
	Settings.IsSet.PeerBidiStreamCount = TRUE;
	Settings.PeerBidiStreamCount = 8;
	Settings.IsSet.PeerUnidiStreamCount = TRUE;
	Settings.PeerUnidiStreamCount = 8;
	Settings.IsSet.ServerResumptionLevel = TRUE;
	Settings.ServerResumptionLevel = QUIC_SERVER_NO_RESUME;

	QUIC_BUFFER AlpnBuffer{};
	TArray<uint8> AlpnScratch;
	BuildAlpnBuffer(Options.Alpn, AlpnBuffer, AlpnScratch);

	const QUIC_STATUS Status = Api->ConfigurationOpen(
		RegistrationHandle,
		&AlpnBuffer,
		1,
		&Settings,
		sizeof(Settings),
		nullptr,
		&ConfigurationHandle);
	if (QUIC_FAILED(Status))
	{
		OutError = FString::Printf(TEXT("MsQuic ConfigurationOpen failed (%s)"), *StatusToString(Status));
		return false;
	}

	QUIC_CREDENTIAL_CONFIG CredConfig;
	FMemory::Memzero(CredConfig);
	CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
	CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;

	const QUIC_STATUS CredentialStatus = Api->ConfigurationLoadCredential(ConfigurationHandle, &CredConfig);
	if (QUIC_FAILED(CredentialStatus))
	{
		OutError = FString::Printf(TEXT("MsQuic ConfigurationLoadCredential failed (%s)"), *StatusToString(CredentialStatus));
		return false;
	}

	return true;
}

bool FQuicConnection::StartListener(const FQuicSenderOptions& Options, FString& OutError)
{
	if (ListenerHandle)
	{
		return true;
	}

	const QUIC_STATUS OpenStatus = Api->ListenerOpen(RegistrationHandle, ListenerCallback, this, &ListenerHandle);
	if (QUIC_FAILED(OpenStatus))
	{
		OutError = FString::Printf(TEXT("MsQuic ListenerOpen failed (%s)"), *StatusToString(OpenStatus));
		return false;
	}

	QUIC_ADDR LocalAddr;
	FMemory::Memzero(LocalAddr);
	if (!BuildLocalAddress(Options.Endpoint.Host, Options.Endpoint.Port, LocalAddr))
	{
		Api->ListenerClose(ListenerHandle);
		ListenerHandle = nullptr;
		OutError = FString::Printf(TEXT("Invalid QUIC bind address '%s:%u'"), *Options.Endpoint.Host, Options.Endpoint.Port);
		return false;
	}

	QUIC_BUFFER AlpnBuffer{};
	TArray<uint8> AlpnScratch;
	BuildAlpnBuffer(Options.Alpn, AlpnBuffer, AlpnScratch);

	const QUIC_STATUS StartStatus = Api->ListenerStart(ListenerHandle, &AlpnBuffer, 1, &LocalAddr);
	if (QUIC_FAILED(StartStatus))
	{
		Api->ListenerClose(ListenerHandle);
		ListenerHandle = nullptr;
		OutError = FString::Printf(TEXT("MsQuic ListenerStart failed (%s)"), *StatusToString(StartStatus));
		return false;
	}

	UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("MsQuic listener started on %s:%u (ALPN=%s)"),
		*Options.Endpoint.Host,
		Options.Endpoint.Port,
		Options.Alpn.IsEmpty() ? TEXT("<default>") : *Options.Alpn);

	return true;
}

void FQuicConnection::CloseListener()
{
	if (ListenerHandle)
	{
		if (Api)
		{
			Api->ListenerStop(ListenerHandle);
			Api->ListenerClose(ListenerHandle);
		}
		ListenerHandle = nullptr;
	}
}

void FQuicConnection::CloseConfiguration()
{
	if (ConfigurationHandle)
	{
		if (Api)
		{
			Api->ConfigurationClose(ConfigurationHandle);
		}
		ConfigurationHandle = nullptr;
	}
}

void FQuicConnection::CloseRegistration()
{
	if (RegistrationHandle)
	{
		if (Api)
		{
			Api->RegistrationClose(RegistrationHandle);
		}
		RegistrationHandle = nullptr;
	}
}

TSharedPtr<FQuicConnection::FSubscriberContext> FQuicConnection::RegisterSubscriber(HQUIC ConnectionHandle)
{
	if (!ConnectionHandle)
	{
		return nullptr;
	}

	TSharedPtr<FSubscriberContext> Subscriber = MakeShared<FSubscriberContext>();
	Subscriber->ConnectionHandle = ConnectionHandle;
	Subscriber->SubscriberId = NextSubscriberId++;
	Subscriber->bConnected = false;

	SubscribersByConnection.Add(ConnectionHandle, Subscriber);
	SubscribersById.Add(Subscriber->SubscriberId, Subscriber);
	return Subscriber;
}

TSharedPtr<FQuicConnection::FSubscriberContext> FQuicConnection::GetSubscriberByConnection(HQUIC ConnectionHandle) const
{
	if (const TSharedPtr<FSubscriberContext>* Found = SubscribersByConnection.Find(ConnectionHandle))
	{
		return *Found;
	}
	return nullptr;
}

TSharedPtr<FQuicConnection::FSubscriberContext> FQuicConnection::GetSubscriberById(uint32 SubscriberId) const
{
	if (const TSharedPtr<FSubscriberContext>* Found = SubscribersById.Find(SubscriberId))
	{
		return *Found;
	}
	return nullptr;
}

void FQuicConnection::RemoveSubscriber(HQUIC ConnectionHandle, const FString& Reason)
{
	TSharedPtr<FSubscriberContext> Subscriber;
	{
		if (const TSharedPtr<FSubscriberContext>* Found = SubscribersByConnection.Find(ConnectionHandle))
		{
			Subscriber = *Found;
		}
		SubscribersByConnection.Remove(ConnectionHandle);
		if (Subscriber.IsValid())
		{
			SubscribersById.Remove(Subscriber->SubscriberId);
		}
	}

	if (Subscriber.IsValid() && Listener)
	{
		Listener->OnQuicSubscriberDisconnected(Subscriber->SubscriberId, Reason);
	}
}

bool FQuicConnection::SendFrameInternal(const TSharedPtr<FSubscriberContext>& Subscriber, EQuicFrameType FrameType, const uint8* Data, uint32 Length, FString& OutError)
{
	if (!Subscriber.IsValid() || !Subscriber->ConnectionHandle)
	{
		OutError = TEXT("Subscriber unavailable");
		return false;
	}

	if (!Api)
	{
		OutError = TEXT("MsQuic API unavailable");
		return false;
	}

	FStreamContext* StreamContext = new FStreamContext();
	StreamContext->Owner = this;
	StreamContext->Subscriber = Subscriber;
	StreamContext->FrameType = FrameType;
	StreamContext->bHeaderConsumed = true; // Outgoing streams never read data.
	StreamContext->SendBuffer.SetNum(static_cast<int32>(Length) + 1);
	StreamContext->SendBuffer[0] = static_cast<uint8>(FrameType);
	if (Length > 0)
	{
		FMemory::Memcpy(StreamContext->SendBuffer.GetData() + 1, Data, Length);
	}

	HQUIC StreamHandle = nullptr;
	const QUIC_STATUS OpenStatus = Api->StreamOpen(Subscriber->ConnectionHandle, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, StreamCallback, StreamContext, &StreamHandle);
	if (QUIC_FAILED(OpenStatus))
	{
		delete StreamContext;
		OutError = FString::Printf(TEXT("StreamOpen failed (%s)"), *StatusToString(OpenStatus));
		return false;
	}
	StreamContext->StreamHandle = StreamHandle;

	const QUIC_STATUS StartStatus = Api->StreamStart(StreamHandle, QUIC_STREAM_START_FLAG_IMMEDIATE);
	if (QUIC_FAILED(StartStatus))
	{
		Api->StreamClose(StreamHandle);
		delete StreamContext;
		OutError = FString::Printf(TEXT("StreamStart failed (%s)"), *StatusToString(StartStatus));
		return false;
	}

	QUIC_BUFFER Buffer{};
	Buffer.Buffer = StreamContext->SendBuffer.GetData();
	Buffer.Length = StreamContext->SendBuffer.Num();

	const QUIC_STATUS SendStatus = Api->StreamSend(StreamHandle, &Buffer, 1, QUIC_SEND_FLAG_FIN, StreamContext);
	if (QUIC_FAILED(SendStatus))
	{
		Api->StreamShutdown(StreamHandle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, SendStatus);
		DestroyStreamContext(Api, StreamHandle, StreamContext);
		OutError = FString::Printf(TEXT("StreamSend failed (%s)"), *StatusToString(SendStatus));
		return false;
	}

	return true;
}

void FQuicConnection::HandlePeerStreamStarted(const TSharedPtr<FSubscriberContext>& Subscriber, HQUIC StreamHandle)
{
	if (!Subscriber.IsValid())
	{
		Api->StreamClose(StreamHandle);
		return;
	}

	FStreamContext* StreamContext = new FStreamContext();
	StreamContext->Owner = this;
	StreamContext->Subscriber = Subscriber;
	StreamContext->FrameType = EQuicFrameType::Control;
	StreamContext->bHeaderConsumed = false;
	StreamContext->StreamHandle = StreamHandle;
	Api->SetCallbackHandler(StreamHandle, reinterpret_cast<void*>(StreamCallback), StreamContext);
}

void FQuicConnection::HandleStreamReceive(FStreamContext* StreamContext, const QUIC_STREAM_EVENT* Event)
{
	if (!StreamContext || !Event)
	{
		return;
	}

	if (StreamContext->bOverflowed)
	{
		return;
	}

	const TSharedPtr<FSubscriberContext> Subscriber = StreamContext->Subscriber.Pin();
	const uint32 SubscriberId = Subscriber.IsValid() ? Subscriber->SubscriberId : 0;

	for (uint32 BufferIndex = 0; BufferIndex < Event->RECEIVE.BufferCount; ++BufferIndex)
	{
		const QUIC_BUFFER& Segment = Event->RECEIVE.Buffers[BufferIndex];
		uint32 Offset = 0;

		if (!StreamContext->bHeaderConsumed)
		{
			// Search for the first byte carrying the frame header.
			uint32 HeaderSearchIndex = 0;
			while (HeaderSearchIndex < Segment.Length && !StreamContext->bHeaderConsumed)
			{
				const uint8 HeaderValue = Segment.Buffer[HeaderSearchIndex];
				if (!IsValidFrameType(HeaderValue))
				{
					UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("Received QUIC stream with unknown frame type %u"), HeaderValue);
					StreamContext->bHeaderConsumed = true;
					return;
				}
				StreamContext->FrameType = static_cast<EQuicFrameType>(HeaderValue);
				StreamContext->bHeaderConsumed = true;
				HeaderSearchIndex++;
			}
			Offset = HeaderSearchIndex;
		}

		if (Segment.Length > Offset)
		{
			const int32 IncomingBytes = static_cast<int32>(Segment.Length - Offset);
			const int32 ExistingBytes = StreamContext->ReceiveBuffer.Num();
			if (ExistingBytes + IncomingBytes > O3DMoQ::Constants::MaxControlPayloadBytes)
			{
				StreamContext->bOverflowed = true;
				StreamContext->ReceiveBuffer.Reset();
				UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("QUIC control payload exceeded %d bytes (subscriber=%u). Aborting stream."),
					O3DMoQ::Constants::MaxControlPayloadBytes,
					SubscriberId);
				if (Api && StreamContext->StreamHandle)
				{
					Api->StreamShutdown(StreamContext->StreamHandle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, QUIC_STATUS_BUFFER_TOO_SMALL);
				}
				return;
			}
			StreamContext->ReceiveBuffer.AddUninitialized(IncomingBytes);
			FMemory::Memcpy(StreamContext->ReceiveBuffer.GetData() + ExistingBytes, Segment.Buffer + Offset, IncomingBytes);
		}
	}

	if (StreamContext->bOverflowed)
	{
		return;
	}

	if (Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN)
	{
		if (StreamContext->FrameType == EQuicFrameType::Control && Subscriber.IsValid())
		{
			DispatchControlFrame(Subscriber, MoveTemp(StreamContext->ReceiveBuffer));
		}
		else
		{
			StreamContext->ReceiveBuffer.Reset();
		}
	}
}

void FQuicConnection::DispatchControlFrame(const TSharedPtr<FSubscriberContext>& Subscriber, TArray<uint8>&& Payload)
{
	if (!Subscriber.IsValid() || !Listener)
	{
		return;
	}

	Listener->OnQuicSubscriberControlMessage(Subscriber->SubscriberId, Payload);
}

QUIC_STATUS QUIC_API FQuicConnection::ListenerCallback(HQUIC /*Listener*/, void* Context, QUIC_LISTENER_EVENT* Event)
{
	FQuicConnection* Self = static_cast<FQuicConnection*>(Context);
	if (!Self || !Self->Api)
	{
		return QUIC_STATUS_INVALID_PARAMETER;
	}

	switch (Event->Type)
	{
	case QUIC_LISTENER_EVENT_NEW_CONNECTION:
	{
		FScopeLock Lock(&Self->Guard);
		TSharedPtr<FSubscriberContext> Subscriber = Self->RegisterSubscriber(Event->NEW_CONNECTION.Connection);
		if (!Subscriber.IsValid())
		{
			Self->Api->ConnectionClose(Event->NEW_CONNECTION.Connection);
			return QUIC_STATUS_INTERNAL_ERROR;
		}

		Self->Api->SetCallbackHandler(Event->NEW_CONNECTION.Connection, reinterpret_cast<void*>(ConnectionCallback), Self);
		const QUIC_STATUS ConfigStatus = Self->Api->ConnectionSetConfiguration(Event->NEW_CONNECTION.Connection, Self->ConfigurationHandle);
		if (QUIC_FAILED(ConfigStatus))
		{
			Self->SubscribersByConnection.Remove(Event->NEW_CONNECTION.Connection);
			Self->SubscribersById.Remove(Subscriber->SubscriberId);
			Self->Api->ConnectionClose(Event->NEW_CONNECTION.Connection);
			return ConfigStatus;
		}

		return QUIC_STATUS_SUCCESS;
	}

	case QUIC_LISTENER_EVENT_STOP_COMPLETE:
		return QUIC_STATUS_SUCCESS;

	case QUIC_LISTENER_EVENT_DOS_MODE_CHANGED:
		UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("MsQuic DOS mode changed (enabled=%d)"), Event->DOS_MODE_CHANGED.DosModeEnabled ? 1 : 0);
		return QUIC_STATUS_SUCCESS;

	default:
		return QUIC_STATUS_SUCCESS;
	}
}

QUIC_STATUS QUIC_API FQuicConnection::ConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event)
{
	FQuicConnection* Self = static_cast<FQuicConnection*>(Context);
	if (!Self || !Self->Api)
	{
		return QUIC_STATUS_INVALID_PARAMETER;
	}

	switch (Event->Type)
	{
	case QUIC_CONNECTION_EVENT_CONNECTED:
	{
		FScopeLock Lock(&Self->Guard);
		if (TSharedPtr<FSubscriberContext> Subscriber = Self->GetSubscriberByConnection(Connection))
		{
			Subscriber->bConnected = true;
			if (Self->Listener)
			{
				Self->Listener->OnQuicSubscriberConnected(Subscriber->SubscriberId);
			}
		}
		UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("MsQuic subscriber connected."));
		break;
	}

	case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
	{
		FScopeLock Lock(&Self->Guard);
		Self->HandlePeerStreamStarted(Self->GetSubscriberByConnection(Connection), Event->PEER_STREAM_STARTED.Stream);
		break;
	}

	case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
		UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("MsQuic transport shutdown (status=%s)"), *StatusToString(Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
		break;

	case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
	{
		FScopeLock Lock(&Self->Guard);
		Self->RemoveSubscriber(Connection, TEXT("Connection closed"));
		Self->Api->ConnectionClose(Connection);
		break;
	}

	case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
		if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(Event->DATAGRAM_SEND_STATE_CHANGED.State) &&
			Event->DATAGRAM_SEND_STATE_CHANGED.ClientContext)
		{
			delete static_cast<FDatagramSendState*>(Event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
		}
		break;

	default:
		break;
	}

	return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API FQuicConnection::StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event)
{
	FStreamContext* StreamContext = static_cast<FStreamContext*>(Context);
	FQuicConnection* Owner = StreamContext ? StreamContext->Owner : nullptr;
	if (!Owner || !Owner->Api)
	{
		return QUIC_STATUS_INVALID_PARAMETER;
	}

	switch (Event->Type)
	{
	case QUIC_STREAM_EVENT_RECEIVE:
		Owner->HandleStreamReceive(StreamContext, Event);
		break;

	case QUIC_STREAM_EVENT_SEND_COMPLETE:
		StreamContext->SendBuffer.Reset();
		break;

	case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
		break;

	case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
		DestroyStreamContext(Owner->Api, Stream, StreamContext);
		return QUIC_STATUS_SUCCESS;

	default:
		break;
	}

	return QUIC_STATUS_SUCCESS;
}

void FQuicConnection::DestroyStreamContext(const QUIC_API_TABLE* ApiTable, HQUIC Stream, FStreamContext* Context)
{
	if (ApiTable && Stream)
	{
		ApiTable->StreamClose(Stream);
	}
	delete Context;
}
}
