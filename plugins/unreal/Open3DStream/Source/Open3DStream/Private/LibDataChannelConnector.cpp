// Copyright (c) Open3DStream Contributors

#include "LibDataChannelConnector.h"
#include "WebRTCSignalingClient.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

// libdatachannel includes
#include <rtc/rtc.hpp>
#include <cstddef> // for std::byte
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

// New CVars for Issue #87 resiliency
static TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCAutoReconnect(
    TEXT("o3ds.Broadcast.WebRTC.AutoReconnect"),
    1,
    TEXT("Enable auto-reconnect/re-offer logic on failures (0/1)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCBackoffInitialMs(
    TEXT("o3ds.Broadcast.WebRTC.BackoffInitialMs"),
    500,
    TEXT("Initial backoff for re-offer/reconnect in milliseconds."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCBackoffMaxMs(
    TEXT("o3ds.Broadcast.WebRTC.BackoffMaxMs"),
    10000,
    TEXT("Maximum backoff for re-offer/reconnect in milliseconds."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCNegoChannel(
    TEXT("o3ds.Broadcast.WebRTC.NegotiatedChannel"),
    0,
    TEXT("Use negotiated data channel with fixed id on both sides (0/1)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCChannelId(
    TEXT("o3ds.Broadcast.WebRTC.ChannelId"),
    42,
    TEXT("Fixed DataChannel id to use when NegotiatedChannel=1."),
    ECVF_Default);

const char* FLibDataChannelConnector::DataChannelLabel = "Open3DStream";

FLibDataChannelConnector::FLibDataChannelConnector()
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

FLibDataChannelConnector::~FLibDataChannelConnector()
{
	Stop();
}

void FLibDataChannelConnector::EnsurePeerConnectionForNewSession()
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

bool FLibDataChannelConnector::Start(const FString& Url, bool bInIsServer)
{
	// Preserve any previously bound data callback across restarts
	TFunction<void(const uint8*, int32)> SavedCallback = DataReceivedCallback;

	Stop();

	// Restore callback (Stop may have cleared internal state)
	if (SavedCallback)
	{
		DataReceivedCallback = MoveTemp(SavedCallback);
	}

	bIsServer = bInIsServer;
	LastError.Empty();
	bRemoteDescriptionSet = false;
	bLocalDescriptionSet = false;
	PendingRemoteCandidates.Reset();
	LastPeerState = -1;
	bSignalingIsConnected = false;

	// Snapshot negotiated channel settings
	bNegotiatedChannelEnabled = (CVarO3DSBroadcastWebRTCNegoChannel->GetInt() != 0);
	NegotiatedChannelId = CVarO3DSBroadcastWebRTCChannelId->GetInt();

	ResetReofferBackoff(/*bImmediate*/true);
	ResetReconnectBackoff(/*bImmediate*/true);

	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Starting connection (Mode: %s)"), bInIsServer ? TEXT("Server") : TEXT("Client"));

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

	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Parsed URL - Host: %s, Port: %d, Room: %s, Protocol: %s, Signaling: %s"),
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
	SignalingClient->OnCollision = [this](const FString& Action, int32 RetryAfterMs)
	{
		// If client gets collision, schedule a re-offer after suggested delay
		if (!bIsServer)
		{
			const double Now = FPlatformTime::Seconds();
			double Delay = FMath::Clamp((double)RetryAfterMs / 1000.0, 0.05, 5.0);
			OfferBackoffSeconds = Delay;
			NextOfferTimeSeconds = Now + Delay;
			UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Collision - scheduling re-offer in %.2fs (action=%s)"), Delay, *Action);
		}
	};

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

void FLibDataChannelConnector::Stop()
{
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Stopping connection"));

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
	// Do NOT clear DataReceivedCallback here; allow pre-bound sink to persist across restarts
	LastPeerState = -1;
	bSignalingIsConnected = false;
	NextOfferTimeSeconds = 0.0;
	OfferBackoffSeconds = 0.0;
	NextReconnectTimeSeconds = 0.0;
	ReconnectBackoffSeconds = 0.0;
}

bool FLibDataChannelConnector::Send(const uint8* Data, int32 Size)
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

void FLibDataChannelConnector::SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback)
{
	DataReceivedCallback = Callback;

	// Per-instance bind log to help diagnose receive-path wiring
	const TCHAR* RoleLabel = bIsServer ? TEXT("Server") : TEXT("Client");
	if (DataReceivedCallback)
	{
		UE_LOG(LogTemp, Verbose, TEXT("O3DS RX: DataReceivedCallback bound [%s %p]"), RoleLabel, this);
	}
	else
	{
		UE_LOG(LogTemp, Verbose, TEXT("O3DS RX: DataReceivedCallback cleared [%s %p]"), RoleLabel, this);
	}
}

bool FLibDataChannelConnector::SendDataReliable(const uint8* Data, int32 Size)
{
	// libdatachannel does not expose reliability control per-message.
	// All messages on this DataChannel use the reliability configured at channel creation.
	// For now, map SendDataReliable to Send().
	return Send(Data, Size);
}

bool FLibDataChannelConnector::SendDataLossy(const uint8* Data, int32 Size)
{
	// libdatachannel does not expose reliability control per-message.
	// To implement lossy messaging, we would need a separate DataChannel with maxRetransmits=0.
	// For now, map SendDataLossy to the same Send() as reliable.
	// TODO: Create separate unreliable DataChannel when EWebRTCReliability::Lossy is requested.
	return Send(Data, Size);
}

bool FLibDataChannelConnector::EnableAudioSend(const FAudioSendConfig& Config)
{
	// Audio tracks not yet implemented for libdatachannel connector.
	// Will be implemented in a future PR after LiveKit connector is added.
	UE_LOG(LogTemp, Warning, TEXT("FLibDataChannelConnector::EnableAudioSend() not yet implemented"));
	return false;
}

bool FLibDataChannelConnector::PushPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames,
                                       int32 NumChannels, int32 SampleRate, double TimestampSec)
{
    // Audio tracks not yet implemented for libdatachannel connector.
    // Stub to satisfy interface; return false to indicate unsupported.
    UE_LOG(LogTemp, Verbose, TEXT("FLibDataChannelConnector::PushPcm() not implemented (Stream=%s, Frames=%d, Ch=%d, Rate=%d)"),
        *StreamLabel, NumFrames, NumChannels, SampleRate);
    return false;
}

void FLibDataChannelConnector::Tick()
{
    // Process queued received data
    TArray<uint8> ReceivedData;
    while (ReceivedDataQueue.Dequeue(ReceivedData))
    {
        const TCHAR* RoleLabel = bIsServer ? TEXT("Server") : TEXT("Client");

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
                UE_LOG(LogTemp, Verbose, TEXT("WebRTC RX[%s %p]: First packet size=%d header_match=%s first_%d=%s"),
                    RoleLabel, this, ReceivedData.Num(), bHeaderMatch?TEXT("true"):TEXT("false"), DumpN, *Hex);
            }
        }

        if (DataReceivedCallback)
        {
            if (CVarO3DSWebRTCDebugRx->GetInt() != 0)
            {
                static int32 DispatchLogs = 0;
                if (DispatchLogs < 3)
                {
                    ++DispatchLogs;
                    UE_LOG(LogTemp, Verbose, TEXT("WebRTC RX[%s %p]: Dispatching %d bytes to receiver callback"), RoleLabel, this, ReceivedData.Num());
                }
            }
            DataReceivedCallback(ReceivedData.GetData(), ReceivedData.Num());
        }
        else if (CVarO3DSWebRTCDebugRx->GetInt() != 0)
        {
            static int32 DropLogs = 0;
            if (DropLogs < 3)
            {
                ++DropLogs;
                UE_LOG(LogTemp, Verbose, TEXT("WebRTC RX[%s %p]: Dropping %d bytes (no DataReceivedCallback bound)"), RoleLabel, this, ReceivedData.Num());
            }
        }
    }

    // Re-offer / reconnect timers (Issue #87)
    if (CVarO3DSBroadcastWebRTCAutoReconnect->GetInt() != 0)
    {
        const double Now = FPlatformTime::Seconds();
        // Client re-offer loop when no remote description yet
        if (!bIsServer && bSignalingIsConnected && !bRemoteDescriptionSet && PeerConnection)
        {
            if (Now >= NextOfferTimeSeconds && OfferBackoffSeconds > 0.0)
            {
                MaybeCreateOffer(TEXT("reoffer-timer"));
                // Backoff grows up to max
                const double MaxSec = (double)CVarO3DSBroadcastWebRTCBackoffMaxMs->GetInt() / 1000.0;
                OfferBackoffSeconds = FMath::Min(OfferBackoffSeconds * 2.0, MaxSec);
                const double Jitter = FMath::FRandRange(0.0, 0.25 * OfferBackoffSeconds);
                NextOfferTimeSeconds = Now + OfferBackoffSeconds + Jitter;
            }
        }

        // Reconnect/renegotiate on disconnection
        const bool bPeerDown = (LastPeerState == (int32)rtc::PeerConnection::State::Failed) ||
                               (LastPeerState == (int32)rtc::PeerConnection::State::Closed) ||
                               (LastPeerState == (int32)rtc::PeerConnection::State::Disconnected);
        if (bPeerDown && Now >= NextReconnectTimeSeconds && ReconnectBackoffSeconds > 0.0)
        {
            // Try to restart negotiation (fresh PC and offer if client; server just waits)
            UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Reconnect timer fired (state=%d)"), LastPeerState);
            EnsurePeerConnectionForNewSession();
            if (!bIsServer)
            {
                MaybeCreateOffer(TEXT("reconnect"));
            }
            const double MaxSec = (double)CVarO3DSBroadcastWebRTCBackoffMaxMs->GetInt() / 1000.0;
            ReconnectBackoffSeconds = FMath::Min(ReconnectBackoffSeconds * 2.0, MaxSec);
            const double Jitter = FMath::FRandRange(0.0, 0.25 * ReconnectBackoffSeconds);
            NextReconnectTimeSeconds = Now + ReconnectBackoffSeconds + Jitter;
        }
    }
}

void FLibDataChannelConnector::OnPeerConnectionStateChange(int StateInt)
{
	FScopeLock Lock(&this->PeerConnectionLock);

	rtc::PeerConnection::State State = static_cast<rtc::PeerConnection::State>(StateInt);
	LastPeerState = StateInt;

	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: PeerConnection state change"));

	switch (State)
	{
		case rtc::PeerConnection::State::Connecting:
			this->ConnectionState = TEXT("CONNECTING");
			break;
		case rtc::PeerConnection::State::Connected:
			this->ConnectionState = TEXT("CONNECTED");
			this->bIsConnected = true;
			// Reset backoffs on success
			ResetReofferBackoff(/*bImmediate*/false);
			ResetReconnectBackoff(/*bImmediate*/false);
			break;
		case rtc::PeerConnection::State::Disconnected:
			this->ConnectionState = TEXT("DISCONNECTED");
			this->bIsConnected = false;
			// Schedule reconnect attempts
			ResetReconnectBackoff(/*bImmediate*/true);
			break;
		case rtc::PeerConnection::State::Failed:
			this->ConnectionState = TEXT("FAILED");
			this->bIsConnected = false;
			this->LastError = TEXT("PeerConnection failed");
			// Allow reconnection attempts
			ResetReconnectBackoff(/*bImmediate*/true);
			break;
		case rtc::PeerConnection::State::Closed:
			this->ConnectionState = TEXT("CLOSED");
			this->bIsConnected = false;
			// Allow reconnection attempts
			ResetReconnectBackoff(/*bImmediate*/true);
			break;
		default:
			this->ConnectionState = TEXT("UNKNOWN");
	}

	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: PeerConnection state: %s"), *this->ConnectionState);

	// If the connection was closed/failed mid-session, keep signaling alive and allow a clean restart by recreating PC on next join
	if (State == rtc::PeerConnection::State::Failed || State == rtc::PeerConnection::State::Closed)
	{
		// Do not call Stop(): we want signaling callbacks to keep working in this editor session
		CleanupPeerConnection();
	}
}

void FLibDataChannelConnector::OnDataChannelOpen()
{
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Data channel opened"));
	bDataChannelOpen = true;
}

void FLibDataChannelConnector::OnDataChannelMessage(const std::vector<uint8>& Message)
{
	// Queue the message for processing in game thread
	TArray<uint8> Data(Message.data(), Message.size());
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Received %d bytes on data channel"), Data.Num());
	ReceivedDataQueue.Enqueue(Data);
}

void FLibDataChannelConnector::OnDataChannelError(const std::string& Error)
{
	LastError = FString(ANSI_TO_TCHAR(Error.c_str()));
	UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Data channel error: %s"), *LastError);
}

void FLibDataChannelConnector::OnDataChannelClosed()
{
	UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Data channel closed"));
	bDataChannelOpen = false;
}

void FLibDataChannelConnector::OnIceCandidate(const rtc::Candidate& Candidate)
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

void FLibDataChannelConnector::OnLocalDescription(const rtc::Description& Description)
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

void FLibDataChannelConnector::OnSignalingConnected()
{
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Signaling connected"));
	bSignalingIsConnected = true;
	
	// Only client mode creates data channel proactively
	// Server mode will receive data channel from peer
	if (!bIsServer)
	{
		CreateDataChannel();
		// In some cases the peer has already joined before this callback; proactively create offer
		MaybeCreateOffer(TEXT("on-signaling-connected"));
	}
}

void FLibDataChannelConnector::OnSignalingError(const FString& Error)
{
	LastError = Error;
	UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Signaling error: %s"), *Error);
	Stop();
}

void FLibDataChannelConnector::OnSignalingDisconnected(const FString& Reason)
{
	UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Signaling disconnected: %s"), *Reason);
	bIsConnected = false;
	bDataChannelOpen = false;
	bSignalingIsConnected = false;
}

void FLibDataChannelConnector::OnOfferReceived(const FString& SDP)
{
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Offer received from remote peer"));

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
		UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Creating answer in response to offer"));
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

void FLibDataChannelConnector::OnAnswerReceived(const FString& SDP)
{
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Answer received from remote peer"));

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

void FLibDataChannelConnector::OnIceCandidateReceived(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex)
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

void FLibDataChannelConnector::OnPeerJoined()
{
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Peer joined room, initiating connection"));

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
		UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Server mode - waiting for client offer"));
	}
	else
	{
		// Client creates/recreates offer when peer appears
		MaybeCreateOffer(TEXT("peer-joined"));
	}
}

bool FLibDataChannelConnector::ParseWebRtcUrl(const FString& Url, FString& OutHost, uint16& OutPort, FString& OutRoom, TMap<FString, FString>& OutParams)
{
	// Accepts formats like:
	// - webrtc://host:port/room?param=val
	// - ws://host/room?room=name&param=val
	// - wss://host:port?room=name&param=val

	OutHost.Empty();
	OutRoom.Empty();
	OutParams.Reset();
	OutPort =0;

	if (Url.IsEmpty())
	{
		LastError = TEXT("Empty URL");
		return false;
	}

	FString Working = Url;
	// Extract scheme
	int32 SchemePos = Working.Find(TEXT("://"));
	if (SchemePos == INDEX_NONE)
	{
		LastError = TEXT("URL must include scheme (webrtc:// or ws(s)://)");
		return false;
	}

	const FString Scheme = Working.Left(SchemePos).ToLower();
	Working = Working.Mid(SchemePos +3);

	if (!(Scheme.Equals(TEXT("webrtc"), ESearchCase::IgnoreCase) || Scheme.Equals(TEXT("ws"), ESearchCase::IgnoreCase) || Scheme.Equals(TEXT("wss"), ESearchCase::IgnoreCase)))
	{
		LastError = TEXT("URL must start with webrtc:// or ws(s)://");
		return false;
	}

	// Split off query string if present
	FString QueryString;
	int32 QPos = Working.Find(TEXT("?"));
	if (QPos != INDEX_NONE)
	{
		QueryString = Working.Mid(QPos +1);
		Working = Working.Left(QPos);
	}

	// Extract first path segment as room if present
	FString HostPort = Working;
	FString Path;
	int32 SlashPos = Working.Find(TEXT("/"));
	if (SlashPos != INDEX_NONE)
	{
		HostPort = Working.Left(SlashPos);
		Path = Working.Mid(SlashPos +1);
		// take only first segment as room
		int32 NextSlash = Path.Find(TEXT("/"));
		if (NextSlash != INDEX_NONE)
		{
			Path = Path.Left(NextSlash);
		}
	}

	// Parse host and optional port. Handle IPv6 [addr]:port
	FString HostPart = HostPort;
	FString PortStr;
	if (HostPart.StartsWith(TEXT("[")))
	{
		int32 Close = HostPart.Find(TEXT("]"));
		if (Close == INDEX_NONE)
		{
			LastError = TEXT("Malformed IPv6 host");
			return false;
		}
		OutHost = HostPart.Mid(0, Close +1);
		if (HostPart.Len() > Close +1 && HostPart[Close +1] == TEXT(':'))
		{
			PortStr = HostPart.Mid(Close +2);
		}
	}
	else
	{
		int32 LastColon = HostPart.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (LastColon != INDEX_NONE)
		{
			// If there is a colon and the substring after is numeric treat as port
			FString MaybePort = HostPart.Mid(LastColon +1);
			bool bAllDigits = true;
			for (int32 i =0; i < MaybePort.Len(); ++i)
			{
				if (!FChar::IsDigit(MaybePort[i])) { bAllDigits = false; break; }
			}
			if (bAllDigits && MaybePort.Len() >0)
			{
				OutHost = HostPart.Left(LastColon);
				PortStr = MaybePort;
			}
			else
			{
				OutHost = HostPart; // colon is part of hostname (uncommon)
			}
		}
		else
		{
			OutHost = HostPart;
		}
	}

	// Default ports
	if (PortStr.IsEmpty())
	{
		if (Scheme.Equals(TEXT("wss"), ESearchCase::IgnoreCase))
		{
			OutPort =443;
		}
		else if (Scheme.Equals(TEXT("ws"), ESearchCase::IgnoreCase))
		{
			OutPort =80;
		}
		else
		{
			// for webrtc scheme default to443 (secure)
			OutPort =443;
		}
	}
	else
	{
		int32 P = FCString::Atoi(*PortStr);
		if (P <=0 || P >65535)
		{
			LastError = TEXT("Invalid port in URL");
			return false;
		}
		OutPort = (uint16)P;
	}

	// Parse query params into map
	if (!QueryString.IsEmpty())
	{
		TArray<FString> Pairs;
		QueryString.ParseIntoArray(Pairs, TEXT("&"), true);
		for (const FString& Pair : Pairs)
		{
			if (Pair.IsEmpty()) continue;
			int32 Eq = Pair.Find(TEXT("="));
			if (Eq != INDEX_NONE)
			{
				const FString Key = Pair.Left(Eq);
				const FString Val = Pair.Mid(Eq +1);
				OutParams.Add(Key, Val);
			}
			else
			{
				OutParams.Add(Pair, TEXT(""));
			}
		}
	}

	// If room provided in path use it, else check params case-insensitively for 'room'
	if (!Path.IsEmpty())
	{
		OutRoom = Path;
	}
	else
	{
		// look for 'room' key case-insensitive
		for (const auto& Pair : OutParams)
		{
			if (Pair.Key.Equals(TEXT("room"), ESearchCase::IgnoreCase))
			{
				OutRoom = Pair.Value;
				break;
			}
		}
	}

	if (OutRoom.IsEmpty())
	{
		LastError = TEXT("URL must include room either in path or as ?room=...");
		return false;
	}

	// Normalize host (strip brackets around IPv6 for later comparison if needed)
	if (OutHost.StartsWith(TEXT("[")) && OutHost.EndsWith(TEXT("]")))
	{
		// keep brackets for logging but leave as-is
	}

	return true;
}

bool FLibDataChannelConnector::SetupPeerConnection()
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

		// If server mode, handle incoming data channels (non-negotiated mode)
		if (bIsServer && !bNegotiatedChannelEnabled)
		{
			PeerConnection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> IncomingChannel)
			{
				UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Incoming data channel from peer"));
				
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

		UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: PeerConnection created successfully"));

		// For negotiated channel mode, both sides must create the channel explicitly.
		// Do it here so server doesn't rely on onDataChannel callback.
		if (bNegotiatedChannelEnabled)
		{
			CreateDataChannel();
		}

		return true;
	}
	catch (const std::exception& e)
	{
		LastError = FString(ANSI_TO_TCHAR(e.what()));
		UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Failed to setup PeerConnection: %s"), *LastError);
		return false;
	}
}

bool FLibDataChannelConnector::CreateDataChannel()
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
		if (bNegotiatedChannelEnabled)
		{
			// Use negotiated channel with fixed id to avoid glare/timing
			rtc::DataChannelInit Init;
			Init.negotiated = true;
			Init.id = NegotiatedChannelId;
			DataChannel = this->PeerConnection->createDataChannel(Label, Init);
		}
		else
		{
			DataChannel = this->PeerConnection->createDataChannel(Label);
		}

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

		UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Data channel created successfully (negotiated=%d id=%d)"), bNegotiatedChannelEnabled?1:0, NegotiatedChannelId);
		return true;
	}
	catch (const std::exception& e)
	{
		LastError = FString(ANSI_TO_TCHAR(e.what()));
		UE_LOG(LogTemp, Error, TEXT("WebRTC Connector: Failed to create data channel: %s"), *LastError);
		return false;
	}
}

void FLibDataChannelConnector::CleanupPeerConnection()
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

void FLibDataChannelConnector::FlushPendingRemoteCandidates()
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

void FLibDataChannelConnector::ResetReofferBackoff(bool bImmediate)
{
	const double Initial = (double)CVarO3DSBroadcastWebRTCBackoffInitialMs->GetInt() / 1000.0;
	OfferBackoffSeconds = Initial;
	const double Now = FPlatformTime::Seconds();
	if (bImmediate)
	{
		NextOfferTimeSeconds = Now + FMath::FRandRange(0.0, 0.25 * OfferBackoffSeconds);
	}
	else
	{
		NextOfferTimeSeconds = Now + OfferBackoffSeconds;
	}
}

void FLibDataChannelConnector::ResetReconnectBackoff(bool bImmediate)
{
	const double Initial = (double)CVarO3DSBroadcastWebRTCBackoffInitialMs->GetInt() / 1000.0;
	ReconnectBackoffSeconds = Initial;
	const double Now = FPlatformTime::Seconds();
	if (bImmediate)
	{
		NextReconnectTimeSeconds = Now + FMath::FRandRange(0.0, 0.25 * ReconnectBackoffSeconds);
	}
	else
	{
		NextReconnectTimeSeconds = Now + ReconnectBackoffSeconds;
	}
}

void FLibDataChannelConnector::MaybeCreateOffer(const TCHAR* Context)
{
	if (bIsServer)
	{
		return; // server does not initiate offers
	}
	FScopeLock Lock(&PeerConnectionLock);
	if (!PeerConnection)
	{
		return;
	}
	try
	{
		// Ensure a data channel exists first in non-negotiated mode so SDP has the m= line
		if (!bNegotiatedChannelEnabled && !DataChannel)
		{
			CreateDataChannel();
		}
		PeerConnection->createOffer();
		UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Client created offer (%s)"), Context);
	}
	catch (const std::exception& e)
	{
		LastError = FString(ANSI_TO_TCHAR(e.what()));
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Failed to create offer (%s): %s"), Context, *LastError);
	}
}
