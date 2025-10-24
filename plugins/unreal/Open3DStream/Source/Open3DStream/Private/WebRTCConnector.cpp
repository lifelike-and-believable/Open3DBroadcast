// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebRTCConnector.h"
#include "WebRTCSignalingClient.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"

// libdatachannel includes
#include <rtc/rtc.hpp>
#include <memory>
#include <vector>
#include <string>
#include <variant>

static TAutoConsoleVariable<int32> CVarO3DSWebRTCVerbose(
    TEXT("o3ds.WebRTC.Verbose"),
    0,
    TEXT("Enable extra verbose logging for WebRTC connector (0/1)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSWebRTCDebugRx(
    TEXT("o3ds.WebRTC.DebugRx"),
    1,
    TEXT("Enable receiver-side debug logging for WebRTC data (0/1). Logs first packet and occasional stats."),
    ECVF_Default);

const char* FWebRTCConnector::DataChannelLabel = "Open3DStream";

FWebRTCConnector::FWebRTCConnector()
	: bIsConnected(false)
	, bDataChannelOpen(false)
	, bIsServer(false)
	, bRemoteDescriptionSet(false)
	, bLocalDescriptionSet(false)
	, ConnectionState(TEXT("NOTSTARTED"))
	, LastPeerState(-1)
{
	LastError.Empty();
}

FWebRTCConnector::~FWebRTCConnector()
{
	Stop();
}

void FWebRTCConnector::EnsurePeerConnectionForNewSession()
{
	FScopeLock Lock(&PeerConnectionLock);
	bool bNeedsNew = false;
	if (!PeerConnection)
	{
		bNeedsNew = true;
	}
	else if (LastPeerState == static_cast<int32>(rtc::PeerConnection::State::Failed) ||
	         LastPeerState == static_cast<int32>(rtc::PeerConnection::State::Closed))
	{
		bNeedsNew = true;
	}
	if (bNeedsNew)
	{
		CleanupPeerConnection();
		SetupPeerConnection();
	}
}

bool FWebRTCConnector::Start(const FString& Url, bool bInIsServer)
{
	Stop();

	bIsServer = bInIsServer;
	LastError.Empty();
	bRemoteDescriptionSet = false;
	bLocalDescriptionSet = false;
	PendingRemoteCandidates.Reset();
	LastPeerState = -1;

	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Starting connection (Mode: %s)"), bInIsServer ? TEXT("Server") : TEXT("Client"));

	// Parse URL
	FString Host;
	uint16 Port;
	FString Room;
	TMap<FString, FString> Params;

	if (!ParseWebRtcUrl(Url, Host, Port, Room, Params))
	{
		LastError = FString::Printf(TEXT("Invalid WebRTC URL: %s"), *Url);
		UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: %s"), *LastError);
		return false;
	}

	RoomName = Room;
	
	// Use wss:// for remote hosts (HTTPS/secure), ws:// for localhost
	FString Protocol = TEXT("ws");
	if (!Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) && 
	    !Host.Equals(TEXT("127.0.0.1")) && 
	    !Host.StartsWith(TEXT("192.168.")) &&
	    !Host.StartsWith(TEXT("10.")))
	{
		Protocol = TEXT("wss");
	}
	SignalingServerUrl = FString::Printf(TEXT("%s://%s:%d"), *Protocol, *Host, Port);

	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Parsed URL - Host: %s, Port: %d, Room: %s, Protocol: %s, Signaling: %s"),
		*Host, Port, *Room, *Protocol, *SignalingServerUrl);

	// Setup WebRTC configuration
	if (!SetupPeerConnection())
	{
		return false;
	}

	// Create signaling client
	SignalingClient = MakeUnique<FWebRTCSignalingClient>();

	// Bind signaling callbacks
	SignalingClient->OnSignalingConnected = [this]() { OnSignalingConnected(); };
	SignalingClient->OnSignalingError = [this](const FString& Error) { OnSignalingError(Error); };
	SignalingClient->OnSignalingDisconnected = [this](const FString& Reason) { OnSignalingDisconnected(Reason); };
	SignalingClient->OnOfferReceived = [this](const FString& SDP) { OnOfferReceived(SDP); };
	SignalingClient->OnAnswerReceived = [this](const FString& SDP) { OnAnswerReceived(SDP); };
	SignalingClient->OnIceCandidateReceived = [this](const FString& Candidate, const FString& SdpMid, int32 MLineIndex) 
	{
		OnIceCandidateReceived(Candidate, SdpMid, MLineIndex);
	};
	SignalingClient->OnPeerJoined = [this]() { OnPeerJoined(); };

	// Connect to signaling server
	if (!SignalingClient->Connect(SignalingServerUrl, Room, bInIsServer))
	{
		LastError = SignalingClient->GetLastError();
		UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Failed to connect to signaling server: %s"), *LastError);
		Stop();
		return false;
	}

	ConnectionState = TEXT("CONNECTING");
	return true;
}

void FWebRTCConnector::Stop()
{
	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Stopping connection"));

	// Cleanup signaling
	if (SignalingClient)
	{
		SignalingClient->Disconnect();
		SignalingClient.Reset();
	}

	CleanupPeerConnection();

	bIsConnected = false;
	bDataChannelOpen = false;
	bRemoteDescriptionSet = false;
	bLocalDescriptionSet = false;
	PendingRemoteCandidates.Reset();
	ConnectionState = TEXT("CLOSED");
	DataReceivedCallback = nullptr;
	LastPeerState = -1;
}

bool FWebRTCConnector::Send(const uint8* Data, int32 Size)
{
	if (!bDataChannelOpen || !DataChannel)
	{
		LastError = TEXT("Data channel is not open");
		if (CVarO3DSWebRTCVerbose->GetInt() != 0)
		{
			UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Send(%d) rejected, channel_open=%d, has_channel=%d"),
				Size, bDataChannelOpen ? 1 : 0, DataChannel ? 1 : 0);
		}
		return false;
	}

	if (CVarO3DSWebRTCVerbose->GetInt() != 0)
	{
		UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Send(%d)"), Size);
	}

	try
	{
		FScopeLock Lock(&DataChannelLock);
		
		// Convert uint8 data to std::byte for rtc::binary
		rtc::binary Out;
		Out.reserve(Size);
		for (int32 i = 0; i < Size; ++i)
		{
			Out.push_back(static_cast<std::byte>(Data[i]));
		}
		DataChannel->send(Out);
		
		return true;
	}
	catch (const std::exception& e)
	{
		LastError = FString(ANSI_TO_TCHAR(e.what()));
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Send failed: %s"), *LastError);
		return false;
	}
}

void FWebRTCConnector::SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback)
{
	DataReceivedCallback = Callback;
}

void FWebRTCConnector::Tick()
{
    // Process queued received data
    TArray<uint8> ReceivedData;
    while (ReceivedDataQueue.Dequeue(ReceivedData))
    {
        if (CVarO3DSWebRTCDebugRx->GetInt() != 0)
        {
            static bool bLoggedFirstRx = false;
            if (!bLoggedFirstRx)
            {
                bLoggedFirstRx = true;
                const int32 DumpN = FMath::Min(64, ReceivedData.Num());
                FString Hex; Hex.Reserve(DumpN * 3);
                for (int32 i = 0; i < DumpN; ++i)
                {
                    Hex += FString::Printf(TEXT("%02X "), ReceivedData[i]);
                }
                static const uint8 ExpectedHeader[14] = {0x00,0xFF,0x03,0xFE,'O','3','D','S','-','S','T','A','R','T'};
                const bool bHeaderMatch = (ReceivedData.Num() >= 14) && (FMemory::Memcmp(ReceivedData.GetData(), ExpectedHeader, 14) == 0);
                UE_LOG(LogTemp, Warning, TEXT("WebRTC RX: First packet size=%d header_match=%s first_%d=%s"),
                    ReceivedData.Num(), bHeaderMatch?TEXT("true"):TEXT("false"), DumpN, *Hex);
            }
        }

        if (DataReceivedCallback)
        {
            DataReceivedCallback(ReceivedData.GetData(), ReceivedData.Num());
        }
    }
}

void FWebRTCConnector::OnPeerConnectionStateChange(int StateInt)
{
	FScopeLock Lock(&this->PeerConnectionLock);

	rtc::PeerConnection::State State = static_cast<rtc::PeerConnection::State>(StateInt);
	LastPeerState = StateInt;

	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: PeerConnection state change"));

	switch (State)
	{
		case rtc::PeerConnection::State::Connecting:
			this->ConnectionState = TEXT("CONNECTING");
			break;
		case rtc::PeerConnection::State::Connected:
			this->ConnectionState = TEXT("CONNECTED");
			this->bIsConnected = true;
			break;
		case rtc::PeerConnection::State::Disconnected:
			this->ConnectionState = TEXT("DISCONNECTED");
			this->bIsConnected = false;
			break;
		case rtc::PeerConnection::State::Failed:
			this->ConnectionState = TEXT("FAILED");
			this->bIsConnected = false;
			this->LastError = TEXT("PeerConnection failed");
			break;
		case rtc::PeerConnection::State::Closed:
			this->ConnectionState = TEXT("CLOSED");
			this->bIsConnected = false;
			break;
		default:
			this->ConnectionState = TEXT("UNKNOWN");
	}

	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: PeerConnection state: %s"), *this->ConnectionState);

	// If the connection was closed/failed mid-session, keep signaling alive and allow a clean restart by recreating PC on next join
	if (State == rtc::PeerConnection::State::Failed || State == rtc::PeerConnection::State::Closed)
	{
		// Do not call Stop(): we want signaling callbacks to keep working in this editor session
		CleanupPeerConnection();
	}
}

void FWebRTCConnector::OnDataChannelOpen()
{
	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Data channel opened"));
	bDataChannelOpen = true;
}

void FWebRTCConnector::OnDataChannelMessage(const std::vector<uint8>& Message)
{
	// Queue the message for processing in game thread
	TArray<uint8> Data(Message.data(), Message.size());
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Received %d bytes on data channel"), Data.Num());
	ReceivedDataQueue.Enqueue(Data);
}

void FWebRTCConnector::OnDataChannelError(const std::string& Error)
{
	LastError = FString(ANSI_TO_TCHAR(Error.c_str()));
	UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Data channel error: %s"), *LastError);
}

void FWebRTCConnector::OnDataChannelClosed()
{
	UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Data channel closed"));
	bDataChannelOpen = false;
}

void FWebRTCConnector::OnIceCandidate(const rtc::Candidate& Candidate)
{
	if (SignalingClient)
	{
		// Convert libdatachannel candidate to string
		std::string CandidateStr = Candidate.candidate();
		FString Candidate_FString(ANSI_TO_TCHAR(CandidateStr.c_str()));
		FString SdpMid(ANSI_TO_TCHAR(Candidate.mid().c_str()));
		int32 SdpMLineIndex = 0; // libdatachannel doesn't expose mLineIndex, use 0

		SignalingClient->SendIceCandidate(Candidate_FString, SdpMid, SdpMLineIndex);
	}
}

void FWebRTCConnector::OnLocalDescription(const rtc::Description& Description)
{
	if (SignalingClient)
	{
		FString SDP(ANSI_TO_TCHAR(std::string(Description).c_str()));

		if (Description.type() == rtc::Description::Type::Offer)
		{
			SignalingClient->SendOffer(SDP);
		}
		else if (Description.type() == rtc::Description::Type::Answer)
		{
			SignalingClient->SendAnswer(SDP);
		}
	}

	// libdatachannel calls onLocalDescription after setLocalDescription, safe to mark and try flush queued ICE
	bLocalDescriptionSet = true;
	FlushPendingRemoteCandidates();
}

void FWebRTCConnector::OnSignalingConnected()
{
	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Signaling connected"));
	
	// Only client mode creates data channel proactively
	// Server mode will receive data channel from peer
	if (!bIsServer)
	{
		CreateDataChannel();
		// In some cases the peer has already joined before this callback; proactively create offer
		try
		{
			FScopeLock Lock(&PeerConnectionLock);
			if (PeerConnection)
			{
				PeerConnection->createOffer();
				UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Client created offer on signaling connect"));
			}
		}
		catch (const std::exception& e)
		{
			LastError = FString(ANSI_TO_TCHAR(e.what()));
			UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Failed to create offer on connect: %s"), *LastError);
		}
	}
}

void FWebRTCConnector::OnSignalingError(const FString& Error)
{
	LastError = Error;
	UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Signaling error: %s"), *Error);
	Stop();
}

void FWebRTCConnector::OnSignalingDisconnected(const FString& Reason)
{
	UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Signaling disconnected: %s"), *Reason);
	bIsConnected = false;
	bDataChannelOpen = false;
}

void FWebRTCConnector::OnOfferReceived(const FString& SDP)
{
	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Offer received from remote peer"));

	FScopeLock Lock(&PeerConnectionLock);
	// Recreate peer connection if needed (e.g., after a prior Closed/Failed state)
	EnsurePeerConnectionForNewSession();
	if (!PeerConnection)
	{
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: PeerConnection not ready (offer) even after ensure"));
		return;
	}

	try
	{
		std::string SdpStr(TCHAR_TO_ANSI(*SDP));
		PeerConnection->setRemoteDescription(rtc::Description(SdpStr, rtc::Description::Type::Offer));
		bRemoteDescriptionSet = true;
		
		// Server must create answer in response to offer
		UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Creating answer in response to offer"));
		PeerConnection->createAnswer();
		
		// Don't flush ICE candidates yet - wait for local description (answer) to be set
		// FlushPendingRemoteCandidates() will be called in OnLocalDescription
	}
	catch (const std::exception& e)
	{
		LastError = FString(ANSI_TO_TCHAR(e.what()));
		UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Failed to set remote offer: %s"), *LastError);
	}
}

void FWebRTCConnector::OnAnswerReceived(const FString& SDP)
{
	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Answer received from remote peer"));

	FScopeLock Lock(&PeerConnectionLock);
	if (!PeerConnection)
	{
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: PeerConnection not ready"));
		return;
	}

	try
	{
		std::string SdpStr(TCHAR_TO_ANSI(*SDP));
		PeerConnection->setRemoteDescription(rtc::Description(SdpStr, rtc::Description::Type::Answer));
		bRemoteDescriptionSet = true;
		FlushPendingRemoteCandidates();
	}
	catch (const std::exception& e)
	{
		LastError = FString(ANSI_TO_TCHAR(e.what()));
		UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Failed to set remote answer: %s"), *LastError);
	}
}

void FWebRTCConnector::OnIceCandidateReceived(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex)
{
	FScopeLock Lock(&PeerConnectionLock);
	if (!PeerConnection)
	{
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: PeerConnection not ready for ICE candidate"));
		return;
	}

	try
	{
		// Do not add remote ICE until both ends have descriptions to avoid "no ICE transport" errors
		if (!bRemoteDescriptionSet || !bLocalDescriptionSet)
		{
			PendingRemoteCandidates.Emplace(Candidate, SdpMid, SdpMLineIndex);
			UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Queued ICE candidate (waiting descriptions)"));
			return;
		}

		std::string CandidateStr(TCHAR_TO_ANSI(*Candidate));
		std::string SdpMidStr(TCHAR_TO_ANSI(*SdpMid));
		rtc::Candidate RtcCandidate(CandidateStr, SdpMidStr);
		PeerConnection->addRemoteCandidate(RtcCandidate);
	}
	catch (const std::exception& e)
	{
		LastError = FString(ANSI_TO_TCHAR(e.what()));
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Failed to add ICE candidate: %s"), *LastError);
	}
}

void FWebRTCConnector::OnPeerJoined()
{
	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Peer joined room, initiating connection"));

	FScopeLock Lock(&PeerConnectionLock);
	// Recreate if prior session closed/failed
	EnsurePeerConnectionForNewSession();
	if (!PeerConnection)
	{
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: PeerConnection not ready"));
		return;
	}

	if (bIsServer)
	{
		// Server waits for client to send offer
		UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Server mode - waiting for client offer"));
	}
	else
	{
		// Client creates offer
		UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Client mode - creating offer"));
		try
		{
			PeerConnection->createOffer();
		}
		catch (const std::exception& e)
		{
			LastError = FString(ANSI_TO_TCHAR(e.what()));
			UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Failed to create offer: %s"), *LastError);
		}
	}
}

bool FWebRTCConnector::ParseWebRtcUrl(const FString& Url, FString& OutHost, uint16& OutPort, FString& OutRoom, TMap<FString, FString>& OutParams)
{
	// Expected format: webrtc://host:port/room?param=value&param2=value2

	// Remove scheme
	FString UrlWithoutScheme = Url;
	if (!UrlWithoutScheme.StartsWith(TEXT("webrtc://")))
	{
		LastError = TEXT("URL must start with webrtc://");
		return false;
	}

	UrlWithoutScheme.RemoveFromStart(TEXT("webrtc://"));

	// Extract query string
	FString QueryString;
	int32 QueryPos = UrlWithoutScheme.Find(TEXT("?"));
	if (QueryPos != INDEX_NONE)
	{
		QueryString = UrlWithoutScheme.Mid(QueryPos + 1);
		UrlWithoutScheme = UrlWithoutScheme.Left(QueryPos);
	}

	// Extract room
	FString HostPort = UrlWithoutScheme;
	int32 RoomPos = UrlWithoutScheme.Find(TEXT("/"));
	if (RoomPos != INDEX_NONE)
	{
		HostPort = UrlWithoutScheme.Left(RoomPos);
		OutRoom = UrlWithoutScheme.Mid(RoomPos + 1);
	}
	else
	{
		LastError = TEXT("URL must contain /room");
		return false;
	}

	// Parse host and port
	if (!HostPort.Contains(TEXT(":")))
	{
		LastError = TEXT("URL must contain :port");
		return false;
	}

	int32 PortPos = HostPort.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	OutHost = HostPort.Left(PortPos);
	FString PortStr = HostPort.Mid(PortPos + 1);
	OutPort = FCString::Atoi(*PortStr);

	// Parse query parameters
	if (!QueryString.IsEmpty())
	{
		TArray<FString> Params;
		QueryString.ParseIntoArray(Params, TEXT("&"));

		for (const FString& Param : Params)
		{
			int32 EqualPos = Param.Find(TEXT("="));
			if (EqualPos != INDEX_NONE)
			{
				FString Key = Param.Left(EqualPos);
				FString Value = Param.Mid(EqualPos + 1);
				OutParams.Add(Key, Value);
			}
		}
	}

	return true;
}

bool FWebRTCConnector::SetupPeerConnection()
{
	try
	{
		// Create configuration
		RtcConfig = std::make_shared<rtc::Configuration>();

		// Add STUN servers
		rtc::IceServer StunServer("stun.l.google.com", 19302);
		RtcConfig->iceServers.push_back(StunServer);

		// Create PeerConnection
		PeerConnection = std::make_shared<rtc::PeerConnection>(*RtcConfig);

		// Bind callbacks
		PeerConnection->onStateChange([this](rtc::PeerConnection::State State)
		{
			OnPeerConnectionStateChange(static_cast<int>(State));
		});

		PeerConnection->onLocalCandidate([this](rtc::Candidate Candidate)
		{
			OnIceCandidate(Candidate);
		});

		PeerConnection->onLocalDescription([this](rtc::Description Description)
		{
			OnLocalDescription(Description);
		});

		// If server mode, handle incoming data channels
		if (bIsServer)
		{
			PeerConnection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> IncomingChannel)
			{
				UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Incoming data channel from peer"));
				
				{
					FScopeLock Lock(&DataChannelLock);
					DataChannel = IncomingChannel;
				}

				// Bind callbacks for incoming data channel
				IncomingChannel->onOpen([this]()
				{
					OnDataChannelOpen();
				});

				IncomingChannel->onMessage([this](const std::variant<rtc::binary, std::string>& Message)
				{
					if (std::holds_alternative<rtc::binary>(Message))
					{
						const auto& Binary = std::get<rtc::binary>(Message);
						// Convert std::byte to uint8
						std::vector<uint8> Buffer;
						Buffer.reserve(Binary.size());
						for (const auto& b : Binary)
						{
							Buffer.push_back(static_cast<uint8>(b));
						}
						OnDataChannelMessage(Buffer);
					}
					else if (std::holds_alternative<std::string>(Message))
					{
						const auto& Str = std::get<std::string>(Message);
						// Treat text payload as raw bytes
						const uint8* Ptr = reinterpret_cast<const uint8*>(Str.data());
						std::vector<uint8> Buffer(Ptr, Ptr + Str.size());
						OnDataChannelMessage(Buffer);
					}
				});

				IncomingChannel->onError([this](const std::string& Error)
				{
					OnDataChannelError(Error);
				});

				IncomingChannel->onClosed([this]()
				{
					OnDataChannelClosed();
				});
			});
		}

		UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: PeerConnection created successfully"));
		return true;
	}
	catch (const std::exception& e)
	{
		LastError = FString(ANSI_TO_TCHAR(e.what()));
		UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Failed to setup PeerConnection: %s"), *LastError);
		return false;
	}
}

bool FWebRTCConnector::CreateDataChannel()
{
	FScopeLock Lock(&PeerConnectionLock);

		if (!this->PeerConnection)
	{
		LastError = TEXT("PeerConnection not initialized");
		return false;
	}

	try
	{
		// Create data channel
		std::string Label(DataChannelLabel);
		DataChannel = this->PeerConnection->createDataChannel(Label);

		if (!DataChannel)
		{
			LastError = TEXT("Failed to create data channel");
			return false;
		}

		// Bind data channel callbacks
		auto LocalDataChannel = DataChannel;
		LocalDataChannel->onOpen([this]()
		{
			OnDataChannelOpen();
		});

		LocalDataChannel->onMessage([this](const std::variant<rtc::binary, std::string>& Message)
		{
			if (std::holds_alternative<rtc::binary>(Message))
			{
				const auto& Binary = std::get<rtc::binary>(Message);
				// Convert std::byte to uint8
				std::vector<uint8> Buffer;
				Buffer.reserve(Binary.size());
				for (const auto& b : Binary)
				{
					Buffer.push_back(static_cast<uint8>(b));
				}
				OnDataChannelMessage(Buffer);
			}
			else if (std::holds_alternative<std::string>(Message))
			{
				const auto& Str = std::get<std::string>(Message);
				const uint8* Ptr = reinterpret_cast<const uint8*>(Str.data());
				std::vector<uint8> Buffer(Ptr, Ptr + Str.size());
				OnDataChannelMessage(Buffer);
			}
		});
		LocalDataChannel->onError([this](const std::string& Error)
		{
			OnDataChannelError(Error);
		});

		LocalDataChannel->onClosed([this]()
		{
			OnDataChannelClosed();
		});

		UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Data channel created successfully"));
		return true;
	}
	catch (const std::exception& e)
	{
		LastError = FString(ANSI_TO_TCHAR(e.what()));
		UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Failed to create data channel: %s"), *LastError);
		return false;
	}
}

void FWebRTCConnector::CleanupPeerConnection()
{
	FScopeLock PeerLock(&PeerConnectionLock);

	if (this->DataChannel)
	{
		try
		{
			this->DataChannel->close();
		}
		catch (const std::exception&)
		{
			// Ignore
		}

		this->DataChannel.reset();
	}

	if (this->PeerConnection)
	{
		try
		{
			this->PeerConnection->close();
		}
		catch (const std::exception&)
		{
			// Ignore
		}

		this->PeerConnection.reset();
	}

	RtcConfig.reset();
}

void FWebRTCConnector::FlushPendingRemoteCandidates()
{
	// Require both descriptions for stable ICE transport per libdatachannel behavior
	if (!bRemoteDescriptionSet || !bLocalDescriptionSet || !PeerConnection)
	{
		return;
	}

	for (const auto& Tuple : PendingRemoteCandidates)
	{
		const FString& Candidate = Tuple.Get<0>();
		const FString& SdpMid = Tuple.Get<1>();
		// int32 Index = Tuple.Get<2>(); // not used by libdatachannel

		try
		{
			std::string CandidateStr(TCHAR_TO_ANSI(*Candidate));
			std::string SdpMidStr(TCHAR_TO_ANSI(*SdpMid));
			rtc::Candidate RtcCandidate(CandidateStr, SdpMidStr);
			PeerConnection->addRemoteCandidate(RtcCandidate);
		}
		catch (const std::exception& e)
		{
			LastError = FString(ANSI_TO_TCHAR(e.what()));
			UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Failed to add queued ICE candidate: %s"), *LastError);
		}
	}
	PendingRemoteCandidates.Reset();
}
