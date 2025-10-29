// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebRTCConnector.h"
#include "WebRTCSignalingClient.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "HAL/IConsoleManager.h"
#include "O3DSConsoleVars.h"
#include "HAL/PlatformTime.h"
#include "Misc/Char.h"
#include "Containers/UnrealString.h"
#include "Containers/HashTable.h"
#include "Hash/CityHash.h"
#include "Misc/Guid.h"
#include "HAL/UnrealMemory.h"
#include "Containers/Array.h"
#include "Templates/UnrealTemplate.h"
#include "Misc/ByteSwap.h"
#include "Misc/Crc.h"
#include "Logging/LogMacros.h"
#include "O3DSLog.h"

// libdatachannel includes
#include <rtc/rtc.hpp>
#include <memory>
#include <vector>
#include <string>
#include <variant>

#include <rtc/rtppacketizer.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>

#if O3DS_WITH_OPUS
// libopus header is provided at ThirdParty/include/opus.h
#include <opus.h>
#endif

#if O3DS_WITH_OPUS
using OpusEncoderT = OpusEncoder;
using OpusDecoderT = OpusDecoder;
#else
struct OpusEncoderT;
struct OpusDecoderT;
#endif

// Console variables moved to O3DSConsoleVars.cpp to avoid unity-build redefinitions

const char* FWebRTCConnector::DataChannelLabel = "Open3DStream";

// Keep latest desired audio stream label to set on new tracks
static FString G_O3DS_DesiredAudioMsid;

// Helper to add labeled/unlabeled tracks
static std::shared_ptr<rtc::Track> AddOpusAudioSendTrackWithLabel(std::shared_ptr<rtc::PeerConnection> PC, int32 BitrateKbps, const FString& StreamLabel)
{
	if (!PC)
	{
		return nullptr;
	}
	// Build an audio media description with Opus payload111
	const bool bForceSendRecv = (CVarO3DSBroadcastWebRTCAudioForceSendRecv->GetInt() != 0);
	rtc::Description::Direction Dir = bForceSendRecv ? rtc::Description::Direction::SendRecv : rtc::Description::Direction::SendOnly;
	rtc::Description::Audio Audio("audio", Dir);
	Audio.addOpusCodec(111);
	if (BitrateKbps >0)
	{
		Audio.setBitrate(BitrateKbps *1000);
	}
	// Attach an SSRC and msid label derived from StreamLabel when provided
	if (!StreamLabel.IsEmpty())
	{
		uint32 Ssrc =0xA17C0000u ^ FCrc::StrCrc32(*StreamLabel);
		std::string Msid = TCHAR_TO_ANSI(*StreamLabel);
		std::string CName = "o3ds";
		Audio.addSSRC(Ssrc, CName, Msid, Msid);
	}
	return PC->addTrack(Audio);
}

// Unlabeled Opus send track helper (single definition)
static std::shared_ptr<rtc::Track> AddOpusAudioSendTrack(std::shared_ptr<rtc::PeerConnection> PC, int32 BitrateKbps)
{
	if (!PC)
	{
		return nullptr;
	}
	const bool bForceSendRecv = (CVarO3DSBroadcastWebRTCAudioForceSendRecv->GetInt() != 0);
	rtc::Description::Direction Dir = bForceSendRecv ? rtc::Description::Direction::SendRecv : rtc::Description::Direction::SendOnly;
	rtc::Description::Audio Audio("audio", Dir);
	Audio.addOpusCodec(111);
	if (BitrateKbps >0)
	{
		Audio.setBitrate(BitrateKbps *1000);
	}
	uint32 Ssrc =0xA17C1234u; std::string CName = "o3ds"; std::string Msid = "o3ds";
	Audio.addSSRC(Ssrc, CName, Msid, Msid);
	return PC->addTrack(Audio);
}

// Explicit RecvOnly Opus audio helper for answerers
static std::shared_ptr<rtc::Track> AddOpusAudioRecvTrack(std::shared_ptr<rtc::PeerConnection> PC)
{
	if (!PC)
	{
		return nullptr;
	}
	rtc::Description::Audio Audio("audio", rtc::Description::Direction::RecvOnly);
	Audio.addOpusCodec(111);
	// No SSRC for pure recvonly
	return PC->addTrack(Audio);
}

// ===== Missing methods restored =====
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
	// Preserve any previously bound data callback across restarts
	TFunction<void(const uint8*, int32)> SavedCallback = DataReceivedCallback;
	auto SavedAudioCb = AudioRxCallback;

	Stop();

	// Restore callbacks (Stop may have cleared internal state)
	if (SavedCallback)
	{
		DataReceivedCallback = MoveTemp(SavedCallback);
	}
	if (SavedAudioCb)
	{
		AudioRxCallback = MoveTemp(SavedAudioCb);
	}

	bIsServer = bInIsServer;
	LastError.Empty();
	bRemoteDescriptionSet = false;
	bLocalDescriptionSet = false;
	PendingRemoteCandidates.Reset();
	LastPeerState = -1;
	bSignalingIsConnected = false;

	// Snapshot negotiated channel settings
	bNegotiatedChannelEnabled = (CVarO3DSBroadcastWebRTCNegoChannel->GetInt() !=0);
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
			double Delay = FMath::Clamp((double)RetryAfterMs /1000.0,0.05,5.0);
			OfferBackoffSeconds = Delay;
			NextOfferTimeSeconds = Now + Delay;
			UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Collision - scheduling re-offer in %.2fs (action=%s)"), Delay, *Action);
		}
	};

	// Connect to signaling server
	SignalingConnectStartSeconds = FPlatformTime::Seconds();
	bSignalingJoinErrorReported = false;
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
	// Do NOT clear callbacks here; allow pre-bound sinks to persist across restarts
	LastPeerState = -1;
	bSignalingIsConnected = false;
	NextOfferTimeSeconds =0.0;
	OfferBackoffSeconds =0.0;
	NextReconnectTimeSeconds =0.0;
	ReconnectBackoffSeconds =0.0;

#if O3DS_WITH_OPUS
	DestroyOpus();
#endif
}

bool FWebRTCConnector::Send(const uint8* Data, int32 Size)
{
	if (!bDataChannelOpen || !DataChannel)
	{
		LastError = TEXT("Data channel is not open");
		if (CVarO3DSWebRTCVerbose->GetInt() !=0)
		{
			UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Send(%d) rejected, channel_open=%d, has_channel=%d"),
				Size, bDataChannelOpen ?1 :0, DataChannel ?1 :0);
		}
		return false;
	}

	if (CVarO3DSWebRTCVerbose->GetInt() !=0)
	{
		UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Send(%d)"), Size);
	}

	try
	{
		FScopeLock Lock(&DataChannelLock);
		
		// Convert uint8 data to std::byte for rtc::binary
		rtc::binary Out;
		Out.reserve(Size);
		for (int32 i =0; i < Size; ++i)
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

void FWebRTCConnector::Tick()
{
 // Process queued received data
 TArray<uint8> ReceivedData;
 while (ReceivedDataQueue.Dequeue(ReceivedData))
 {
 const TCHAR* RoleLabel = bIsServer ? TEXT("Server") : TEXT("Client");

 if (CVarO3DSWebRTCDebugRx->GetInt() !=0)
 {
 static bool bLoggedFirstRx = false;
 if (!bLoggedFirstRx)
 {
 bLoggedFirstRx = true;
 const int32 DumpN = FMath::Min(64, ReceivedData.Num());
 FString Hex; Hex.Reserve(DumpN *3);
 for (int32 i =0; i < DumpN; ++i)
 {
 Hex += FString::Printf(TEXT("%02X "), ReceivedData[i]);
 }
 static const uint8 ExpectedHeader[14] = {0x00,0xFF,0x03,0xFE,'O','3','D','S','-','S','T','A','R','T'};
 const bool bHeaderMatch = (ReceivedData.Num() >=14) && (FMemory::Memcmp(ReceivedData.GetData(), ExpectedHeader,14) ==0);
 UE_LOG(LogTemp, Verbose, TEXT("WebRTC RX[%s %p]: First packet size=%d header_match=%s first_%d=%s"),
 RoleLabel, this, ReceivedData.Num(), bHeaderMatch?TEXT("true"):TEXT("false"), DumpN, *Hex);
 }
 }

 if (DataReceivedCallback)
 {
 if (CVarO3DSWebRTCDebugRx->GetInt() !=0)
 {
 static int32 DispatchLogs =0;
 if (DispatchLogs <3)
 {
 ++DispatchLogs;
 UE_LOG(LogTemp, Verbose, TEXT("WebRTC RX[%s %p]: Dispatching %d bytes to receiver callback"), RoleLabel, this, ReceivedData.Num());
 }
 }
 DataReceivedCallback(ReceivedData.GetData(), ReceivedData.Num());
 }
 else if (CVarO3DSWebRTCDebugRx->GetInt() !=0)
 {
 static int32 DropLogs =0;
 if (DropLogs <3)
 {
 ++DropLogs;
 UE_LOG(LogTemp, Verbose, TEXT("WebRTC RX[%s %p]: Dropping %d bytes (no DataReceivedCallback bound)"), RoleLabel, this, ReceivedData.Num());
 }
 }
 }

 // Process decoded PCM audio frames
 FRxBuffer Rx;
 while (DecodedPcmQueue.Dequeue(Rx))
 {
 if (AudioRxCallback && Rx.PCM.Num() >0)
 {
 AudioRxCallback(Rx.PCM.GetData(), Rx.PCM.Num(), Rx.NumChannels, Rx.SampleRate);
 }
 // Also notify generic remote audio listeners with real stream/subject labels
 if (Rx.PCM.Num() >0)
 {
		// Convert int16 PCM to float [-1.0,1.0] for the delegate
		TArray<float> PCMFloat;
		PCMFloat.SetNumUninitialized(Rx.PCM.Num());
		for (int32 i =0; i < Rx.PCM.Num(); ++i)
		{
			PCMFloat[i] = static_cast<float>(Rx.PCM[i]) /32768.0f;
		}
		
		// Calculate NumFrames from total samples and channels
		const int32 NumFrames = (Rx.NumChannels >0) ? (Rx.PCM.Num() / Rx.NumChannels) :0;
		
		// Broadcast with correct parameter order: StreamLabel, SubjectName, float* PCM, NumFrames, NumChannels, SampleRate
		RemoteAudioDelegate.Broadcast(RxStreamLabel, RxSubjectName, PCMFloat.GetData(), NumFrames, Rx.NumChannels, Rx.SampleRate);
 }
 }

 // Re-offer / reconnect timers (Issue #87)
	// Signaling join watchdog: surface a clear error if no 'joined' ack arrives in time
	if (SignalingClient && !bSignalingIsConnected && !bSignalingJoinErrorReported && SignalingConnectStartSeconds > 0.0)
	{
		const double Now = FPlatformTime::Seconds();
		const int32 TimeoutMs = CVarO3DSBroadcastWebRTCSignalingJoinTimeoutMs->GetInt();
		if (TimeoutMs > 0)
		{
			const double ElapsedMs = (Now - SignalingConnectStartSeconds) * 1000.0;
			if (ElapsedMs >= (double)TimeoutMs)
			{
				LastError = FString::Printf(TEXT("Signaling join timeout after %d ms (ws=%s room=%s)"), TimeoutMs, *SignalingServerUrl, *RoomName);
				UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: %s"), *LastError);
				bSignalingJoinErrorReported = true;
			}
		}
	}

	// Re-offer / reconnect timers (Issue #87)
 if (CVarO3DSBroadcastWebRTCAutoReconnect->GetInt() !=0)
 {
 const double Now = FPlatformTime::Seconds();
 // Client re-offer loop when no remote description yet
 if (!bIsServer && bSignalingIsConnected && !bRemoteDescriptionSet && PeerConnection)
 {
 if (Now >= NextOfferTimeSeconds && OfferBackoffSeconds >0.0)
 {
 MaybeCreateOffer(TEXT("reoffer-timer"));
 // Backoff grows up to max
 const double MaxSec = (double)CVarO3DSBroadcastWebRTCBackoffMaxMs->GetInt() /1000.0;
 OfferBackoffSeconds = FMath::Min(OfferBackoffSeconds *2.0, MaxSec);
 const double Jitter = FMath::FRandRange(0.0,0.25 * OfferBackoffSeconds);
 NextOfferTimeSeconds = Now + OfferBackoffSeconds + Jitter;
 }
 }

 // Reconnect/renegotiate on disconnection
 const bool bPeerDown = (LastPeerState == (int32)rtc::PeerConnection::State::Failed) ||
 (LastPeerState == (int32)rtc::PeerConnection::State::Closed) ||
 (LastPeerState == (int32)rtc::PeerConnection::State::Disconnected);
 if (bPeerDown && Now >= NextReconnectTimeSeconds && ReconnectBackoffSeconds >0.0)
 {
 // Try to restart negotiation (fresh PC and offer if client; server just waits)
 UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Reconnect timer fired (state=%d)"), LastPeerState);
 EnsurePeerConnectionForNewSession();
 if (!bIsServer)
 {
 MaybeCreateOffer(TEXT("reconnect"));
 }
 const double MaxSec = (double)CVarO3DSBroadcastWebRTCBackoffMaxMs->GetInt() /1000.0;
 ReconnectBackoffSeconds = FMath::Min(ReconnectBackoffSeconds *2.0, MaxSec);
 const double Jitter = FMath::FRandRange(0.0,0.25 * ReconnectBackoffSeconds);
 NextReconnectTimeSeconds = Now + ReconnectBackoffSeconds + Jitter;
 }
 }

 // Optional audio send stats
 if (CVarO3DSWebRTCAudioDebug->GetInt() != 0)
 {
 	const double Now = FPlatformTime::Seconds();
 	if (AudioRt.LastStatsLogTime == 0.0)
 	{
 		AudioRt.LastStatsLogTime = Now;
 	}
 	const double Elapsed = Now - AudioRt.LastStatsLogTime;
 	if (Elapsed >= 2.0) // log every ~2s
 	{
 		static uint64 PrevPackets = 0;
 		static uint64 PrevBytes = 0;
 		const uint64 Pkts = AudioRt.SentPackets;
 		const uint64 Bytes = AudioRt.SentBytes;
 		const uint64 Dp = (Pkts >= PrevPackets) ? (Pkts - PrevPackets) : 0;
 		const uint64 Db = (Bytes >= PrevBytes) ? (Bytes - PrevBytes) : 0;
 		const double Pps = Elapsed > 0.0 ? (double)Dp / Elapsed : 0.0;
 		const double Bps = Elapsed > 0.0 ? (double)Db / Elapsed : 0.0;
 		UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Audio: sent %.1f pkts/s, %.0f B/s (total pkts=%llu bytes=%llu)"), Pps, Bps, (unsigned long long)Pkts, (unsigned long long)Bytes);
 		PrevPackets = Pkts;
 		PrevBytes = Bytes;
 		AudioRt.LastStatsLogTime = Now;
 	}
 }
}

void FWebRTCConnector::OnPeerConnectionStateChange(int StateInt)
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
			// Clear any stale error once we reach CONNECTED
			LastError.Empty();
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

void FWebRTCConnector::OnDataChannelOpen()
{
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Data channel opened"));
	bDataChannelOpen = true;
	// Clear any prior data channel errors to avoid stale status noise
	LastError.Empty();
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
		int32 SdpMLineIndex =0; // libdatachannel doesn't expose mLineIndex, use0

		SignalingClient->SendIceCandidate(Candidate_FString, SdpMid, SdpMLineIndex);
	}
}

void FWebRTCConnector::OnLocalDescription(const rtc::Description& Description)
{
	if (SignalingClient)
	{
		FString SDP(ANSI_TO_TCHAR(std::string(Description).c_str()));

		// Quick SDP sanity: check for audio m-line and log codecs once
		{
			// Detect m=audio regardless of line endings (\n, \r\n)
			bool bHasAudio = SDP.Contains(TEXT("m=audio"), ESearchCase::IgnoreCase);
			bLocalSDPHasAudio = bHasAudio;
			if (!bHasAudio)
			{
				UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("WebRTC Connector: Local SDP has no m=audio; audio track may not negotiate"));
			}
			else
			{
				// Extract audio section robustly (handle CRLF or LF)
				int32 AudioStart = SDP.Find(TEXT("m=audio"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
				int32 NextM = INDEX_NONE;
				if (AudioStart != INDEX_NONE)
				{
					// Try various line ending patterns to find the next media section
					NextM = SDP.Find(TEXT("\nm="), ESearchCase::IgnoreCase, ESearchDir::FromStart, AudioStart + 1);
					if (NextM == INDEX_NONE)
					{
						NextM = SDP.Find(TEXT("\r\nm="), ESearchCase::IgnoreCase, ESearchDir::FromStart, AudioStart + 1);
					}
					if (NextM == INDEX_NONE)
					{
						// Fallback: find any subsequent "m="
						NextM = SDP.Find(TEXT("m="), ESearchCase::IgnoreCase, ESearchDir::FromStart, AudioStart + 1);
					}
					const FString Section = (NextM == INDEX_NONE) ? SDP.Mid(AudioStart) : SDP.Mid(AudioStart, NextM - AudioStart);
					bLocalDirRecvOnly = Section.Contains(TEXT("a=recvonly"));
					bLocalDirSendOnly = Section.Contains(TEXT("a=sendonly"));
					bLocalDirSendRecv = Section.Contains(TEXT("a=sendrecv"));
					bLocalDirInactive = Section.Contains(TEXT("a=inactive"));
					bLocalHasOpus111 = Section.Contains(TEXT("a=rtpmap:111 opus"), ESearchCase::IgnoreCase);
					if (bLocalHasOpus111)
					{
						UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: Local SDP includes Opus PT=111"));
					}
					UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("Local SDP audio dir: recvonly=%d sendonly=%d sendrecv=%d inactive=%d"),
						bLocalDirRecvOnly?1:0, bLocalDirSendOnly?1:0, bLocalDirSendRecv?1:0, bLocalDirInactive?1:0);
				}
			}
		}

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
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Connector: Signaling connected"));
	bSignalingIsConnected = true;
	SignalingConnectStartSeconds = 0.0;
	bSignalingJoinErrorReported = false;
	
	// Only client mode creates data channel proactively
	// Server mode will receive data channel from peer
	if (!bIsServer)
	{
		CreateDataChannel();
		// In some cases the peer has already joined before this callback; proactively create offer
		MaybeCreateOffer(TEXT("on-signaling-connected"));
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
	bSignalingIsConnected = false;
	// allow future connects to report a new timeout if needed
	SignalingConnectStartSeconds = 0.0;
	bSignalingJoinErrorReported = false;
}

void FWebRTCConnector::OnOfferReceived(const FString& SDP)
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
		// Diagnose remote SDP for audio support (server path)
		bRemoteSDPHasAudio = SDP.Contains(TEXT("m=audio"), ESearchCase::IgnoreCase);
		if (!bRemoteSDPHasAudio)
		{
			UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: Remote offer has no m=audio"));
		}
		else
		{
			// Log remote offer audio direction and Opus presence (robust line endings)
			int32 AudioStart = SDP.Find(TEXT("m=audio"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
			if (AudioStart != INDEX_NONE)
			{
				int32 NextM = SDP.Find(TEXT("\nm="), ESearchCase::IgnoreCase, ESearchDir::FromStart, AudioStart + 1);
				if (NextM == INDEX_NONE)
				{
					NextM = SDP.Find(TEXT("\r\nm="), ESearchCase::IgnoreCase, ESearchDir::FromStart, AudioStart + 1);
				}
				if (NextM == INDEX_NONE)
				{
					NextM = SDP.Find(TEXT("m="), ESearchCase::IgnoreCase, ESearchDir::FromStart, AudioStart + 1);
				}
				const FString Section = (NextM == INDEX_NONE) ? SDP.Mid(AudioStart) : SDP.Mid(AudioStart, NextM - AudioStart);
				bRemoteDirRecvOnly = Section.Contains(TEXT("a=recvonly"));
				bRemoteDirSendOnly = Section.Contains(TEXT("a=sendonly"));
				bRemoteDirSendRecv = Section.Contains(TEXT("a=sendrecv"));
				bRemoteDirInactive = Section.Contains(TEXT("a=inactive"));
				bRemoteHasOpus111 = Section.Contains(TEXT("a=rtpmap:111 opus"), ESearchCase::IgnoreCase);
				UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("Remote OFFER audio dir: recvonly=%d sendonly=%d sendrecv=%d inactive=%d opus111=%d"),
					bRemoteDirRecvOnly?1:0, bRemoteDirSendOnly?1:0, bRemoteDirSendRecv?1:0, bRemoteDirInactive?1:0, bRemoteHasOpus111?1:0);
			}
		}

#if O3DS_WITH_OPUS
		// Ensure our answer explicitly advertises audio reception when the offer has m=audio
		// Some stacks require a declared RecvOnly transceiver to keep the m=audio active
		if (bRemoteSDPHasAudio)
		{
			if (!AudioTrack && PeerConnection)
			{
				auto RecvTrack = AddOpusAudioRecvTrack(PeerConnection);
				UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("WebRTC Connector: Added RecvOnly audio to answer (offer had m=audio) ok=%d"), RecvTrack ? 1 : 0);
			}
		}
#endif

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

void FWebRTCConnector::OnAnswerReceived(const FString& SDP)
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
		// Diagnose remote SDP for audio support
		bRemoteSDPHasAudio = SDP.Contains(TEXT("m=audio"), ESearchCase::IgnoreCase);
		if (!bRemoteSDPHasAudio && bAudioSendEnabled)
		{
			UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("WebRTC Connector: Remote SDP has no m=audio; audio track will not open"));
		}
		else if (bRemoteSDPHasAudio)
		{
			// Log audio m-line direction and Opus PT presence (robust line endings)
			int32 AudioStart = SDP.Find(TEXT("m=audio"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
			int32 NextM = INDEX_NONE;
			if (AudioStart != INDEX_NONE)
			{
				NextM = SDP.Find(TEXT("\nm="), ESearchCase::IgnoreCase, ESearchDir::FromStart, AudioStart + 1);
				if (NextM == INDEX_NONE)
				{
					NextM = SDP.Find(TEXT("\r\nm="), ESearchCase::IgnoreCase, ESearchDir::FromStart, AudioStart + 1);
				}
				if (NextM == INDEX_NONE)
				{
					NextM = SDP.Find(TEXT("m="), ESearchCase::IgnoreCase, ESearchDir::FromStart, AudioStart + 1);
				}
				const FString Section = (NextM == INDEX_NONE) ? SDP.Mid(AudioStart) : SDP.Mid(AudioStart, NextM - AudioStart);
				bRemoteDirRecvOnly = Section.Contains(TEXT("a=recvonly"));
				bRemoteDirSendOnly = Section.Contains(TEXT("a=sendonly"));
				bRemoteDirSendRecv = Section.Contains(TEXT("a=sendrecv"));
				bRemoteDirInactive = Section.Contains(TEXT("a=inactive"));
				bRemoteHasOpus111 = Section.Contains(TEXT("a=rtpmap:111 opus"), ESearchCase::IgnoreCase);
				UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("Remote SDP audio dir: recvonly=%d sendonly=%d sendrecv=%d inactive=%d opus111=%d"),
					bRemoteDirRecvOnly?1:0, bRemoteDirSendOnly?1:0, bRemoteDirSendRecv?1:0, bRemoteDirInactive?1:0, bRemoteHasOpus111?1:0);
			}
		}
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

bool FWebRTCConnector::ParseWebRtcUrl(const FString& Url, FString& OutHost, uint16& OutPort, FString& OutRoom, TMap<FString, FString>& OutParams)
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

	// Parse query string into OutParams (case-preserving keys)
	if (!QueryString.IsEmpty())
	{
		TArray<FString> Pairs;
		QueryString.ParseIntoArray(Pairs, TEXT("&"), true);
		for (const FString& Pair : Pairs)
		{
			int32 Eq = Pair.Find(TEXT("="));
			if (Eq != INDEX_NONE)
			{
				const FString Key = Pair.Left(Eq);
				const FString Val = Pair.Mid(Eq + 1);
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

bool FWebRTCConnector::SetupPeerConnection()
{
	try
	{
		// Create configuration
		RtcConfig = std::make_shared<rtc::Configuration>();

		// Add STUN servers
		rtc::IceServer StunServer("stun.l.google.com",19302);
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

		// Audio receive: bind onTrack to decode Opus
		PeerConnection->onTrack([this](std::shared_ptr<rtc::Track> Track)
		{
			if (!Track)
				return;
			const std::string Desc = Track->description().description();
			if (Desc.find("audio") == std::string::npos)
			{
				return; // not audio
			}
			UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: Incoming audio track detected"));
			Track->onFrame([this](rtc::binary data, rtc::FrameInfo info)
			{
#if O3DS_WITH_OPUS
				const uint8* Enc = reinterpret_cast<const uint8*>(data.data());
				int32 EncSize = (int32)data.size();
				int ChannelsFromPkt =1;
				if (Enc && EncSize >0)
				{
					ChannelsFromPkt = opus_packet_get_nb_channels(Enc);
					if (ChannelsFromPkt <=0 || ChannelsFromPkt >2) ChannelsFromPkt =1;
				}
				if (!EnsureOpusDecoder(48000, ChannelsFromPkt))
				{
					return;
				}
				// Worst-case120 ms =5760 samples per ch
				const int MaxPerCh =5760;
				TArray<int16> Pcm; Pcm.SetNumZeroed(MaxPerCh * ChannelsFromPkt);
				int Decoded = opus_decode(OpusDec, Enc, EncSize, Pcm.GetData(), MaxPerCh,0);
				if (Decoded >0)
				{
					FRxBuffer Out; Out.NumChannels = ChannelsFromPkt; Out.SampleRate =48000;
					Out.PCM.SetNumUninitialized(Decoded * ChannelsFromPkt);
					FMemory::Memcpy(Out.PCM.GetData(), Pcm.GetData(), sizeof(int16) * Out.PCM.Num());
					DecodedPcmQueue.Enqueue(MoveTemp(Out));
					static bool bLoggedFirstDec = false;
					if (!bLoggedFirstDec && CVarO3DSWebRTCDebugRx->GetInt() !=0)
					{
						bLoggedFirstDec = true;
						UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: First decoded audio frame: samples=%d ch=%d sr=48000"), Decoded, ChannelsFromPkt);
					}
				}
#else
				(void)data; (void)info; // silence unused
#endif
			});
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

#if O3DS_WITH_OPUS
		// If audio sending is enabled, add Opus audio track now (respect stream label if provided)
		if (bAudioSendEnabled && !AudioTrack)
		{
			if (!AudioRt.Config.StreamLabel.IsEmpty())
			{
				AudioTrack = AddOpusAudioSendTrackWithLabel(PeerConnection, AudioRt.Config.BitrateKbps, AudioRt.Config.StreamLabel);
			}
			else
			{
				AudioTrack = AddOpusAudioSendTrack(PeerConnection, AudioRt.Config.BitrateKbps);
			}
			if (!AudioTrack)
			{
				UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("WebRTC Connector: Failed to add Opus audio track"));
			}
			else
			{
				UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("WebRTC Connector: Opus audio track added (stream=%s)"), *AudioRt.Config.StreamLabel);
				// Install RTP/RTCP media handler chain for Opus
				uint32 Ssrc =0;
				std::string CName = "o3ds";
				// Derive SSRC to match what we set in the description
				if (!AudioRt.Config.StreamLabel.IsEmpty())
				{
					Ssrc =0xA17C0000u ^ FCrc::StrCrc32(*AudioRt.Config.StreamLabel);
				}
				else
				{
					Ssrc =0xA17C1234u;
				}
				auto RtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(Ssrc, CName,111, rtc::OpusRtpPacketizer::DefaultClockRate);
				auto Packetizer = std::make_shared<rtc::OpusRtpPacketizer>(RtpCfg);
				// Attempt to attach MID for BUNDLE demux if available on this libdatachannel version
				try {
					if (AudioTrack) {
						const std::string Mid = AudioTrack->mid();
						if (!Mid.empty()) {
							RtpCfg->mid = Mid; // may not exist on older libdatachannel; build will reveal
							UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: Attached MID '%s' to RTP config"), *FString(Mid.c_str()));
						}
					}
				} catch (...) {
					// best-effort; ignore if not supported
				}
				auto SrReporter = std::make_shared<rtc::RtcpSrReporter>(RtpCfg);
				Packetizer->addToChain(SrReporter);
				auto NackResponder = std::make_shared<rtc::RtcpNackResponder>();
				Packetizer->addToChain(NackResponder);
				AudioTrack->setMediaHandler(Packetizer);
				// Track open/closed notifications to gate sending
				AudioTrack->onOpen([this, Track=AudioTrack]()
				{
					AudioRt.bTrackReady = true;
					FString MidStr = Track ? FString(ANSI_TO_TCHAR(Track->mid().c_str())) : FString();
					UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("WebRTC Connector: Audio track opened (mid=%s)"), MidStr.IsEmpty()?TEXT("<none>"):*MidStr);
				});
				AudioTrack->onClosed([this, Track=AudioTrack]()
				{
					AudioRt.bTrackReady = false;
					FString MidStr = Track ? FString(ANSI_TO_TCHAR(Track->mid().c_str())) : FString();
					UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("WebRTC Connector: Audio track closed (mid=%s)"), MidStr.IsEmpty()?TEXT("<none>"):*MidStr);
				});
			}
		}
#endif

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

	if (this->AudioTrack)
	{
		try { this->AudioTrack->close(); } catch (const std::exception&) {}
		this->AudioTrack.reset();
	}

	AudioRt.bTrackReady = false;
	AudioRt.NextSendRetryTimeSeconds =0.0;

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

void FWebRTCConnector::ResetReofferBackoff(bool bImmediate)
{
	const double Initial = (double)CVarO3DSBroadcastWebRTCBackoffInitialMs->GetInt() /1000.0;
	OfferBackoffSeconds = Initial;
	const double Now = FPlatformTime::Seconds();
	if (bImmediate)
	{
		NextOfferTimeSeconds = Now + FMath::FRandRange(0.0,0.25 * OfferBackoffSeconds);
	}
	else
	{
		NextOfferTimeSeconds = Now + OfferBackoffSeconds;
	}
}

void FWebRTCConnector::ResetReconnectBackoff(bool bImmediate)
{
	const double Initial = (double)CVarO3DSBroadcastWebRTCBackoffInitialMs->GetInt() /1000.0;
	ReconnectBackoffSeconds = Initial;
	const double Now = FPlatformTime::Seconds();
	if (bImmediate)
	{
		NextReconnectTimeSeconds = Now + FMath::FRandRange(0.0,0.25 * ReconnectBackoffSeconds);
	}
	else
	{
		NextReconnectTimeSeconds = Now + ReconnectBackoffSeconds;
	}
}

void FWebRTCConnector::MaybeCreateOffer(const TCHAR* Context)
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

// ======== Audio (Opus) API ========

void FWebRTCConnector::EnableAudioSend(const FAudioConfig& InConfig)
{
	UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("WebRTC Connector: EnableAudioSend sr=%d ch=%d br=%d frameMs=%d stream=%s"),
		InConfig.SampleRate, InConfig.NumChannels, InConfig.BitrateKbps, InConfig.FrameSizeMs, *InConfig.StreamLabel);
	AudioRt.Config = InConfig;
	AudioRt.FrameSizeSamples = FMath::Max(1, (InConfig.SampleRate * InConfig.FrameSizeMs) /1000);
	AudioRt.Timestamp =0;
	AudioRt.bTrackReady = false;
	AudioRt.NextSendRetryTimeSeconds =0.0;
	bAudioSendEnabled = true;
#if O3DS_WITH_OPUS
	EnsureOpusEncoder(InConfig);
	// If PC already exists, add audio track and renegotiate on client
	if (PeerConnection && !AudioTrack)
	{
		UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("Open3DStream: PeerConnection active and Opus available. Proceeding to add Audio Track."));

		// Prefer labeled track when stream label present
		if (!InConfig.StreamLabel.IsEmpty())
		{
			AudioTrack = AddOpusAudioSendTrackWithLabel(PeerConnection, InConfig.BitrateKbps, InConfig.StreamLabel);
		}
		else
		{
			AudioTrack = AddOpusAudioSendTrack(PeerConnection, InConfig.BitrateKbps);
		}
		if (!bIsServer && bSignalingIsConnected)
		{
			UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("Open3DStream: [WebRTC Client] Audio Enabled."));
			MaybeCreateOffer(TEXT("audio-enabled"));
		}
		if (AudioTrack)
		{
			// Install RTP/RTCP media handler chain and open/close gating, same as in SetupPeerConnection path
			uint32 Ssrc =0;
			std::string CName = "o3ds";
			if (!InConfig.StreamLabel.IsEmpty())
			{
				Ssrc =0xA17C0000u ^ FCrc::StrCrc32(*InConfig.StreamLabel);
			}
			else
			{
				Ssrc =0xA17C1234u;
			}
			UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("Open3DStream: [WebRTC Client] Audio Track Added. StreamLabel=%s SSRC=0x%08X"), *InConfig.StreamLabel, Ssrc);

			auto RtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(Ssrc, CName,111, rtc::OpusRtpPacketizer::DefaultClockRate);
			auto Packetizer = std::make_shared<rtc::OpusRtpPacketizer>(RtpCfg);
			// Attempt to attach MID for BUNDLE demux if available on this libdatachannel version
			try {
				if (AudioTrack) {
					const std::string Mid = AudioTrack->mid();
					if (!Mid.empty()) {
						RtpCfg->mid = Mid; // may not exist on older libdatachannel; build will reveal
						UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: Attached MID '%s' to RTP config"), *FString(Mid.c_str()));
					}
				}
			} catch (...) {
				// best-effort; ignore if not supported
			}
			auto SrReporter = std::make_shared<rtc::RtcpSrReporter>(RtpCfg);
			Packetizer->addToChain(SrReporter);
			auto NackResponder = std::make_shared<rtc::RtcpNackResponder>();
			Packetizer->addToChain(NackResponder);
			AudioTrack->setMediaHandler(Packetizer);
			AudioTrack->onOpen([this, Track=AudioTrack]()
			{
				AudioRt.bTrackReady = true;
				FString MidStr = Track ? FString(ANSI_TO_TCHAR(Track->mid().c_str())) : FString();
				UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("WebRTC Connector: Audio track opened (mid=%s)"), MidStr.IsEmpty()?TEXT("<none>"):*MidStr);
			});
			AudioTrack->onClosed([this, Track=AudioTrack]()
			{
				AudioRt.bTrackReady = false;
				FString MidStr = Track ? FString(ANSI_TO_TCHAR(Track->mid().c_str())) : FString();
				UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("WebRTC Connector: Audio track closed (mid=%s)"), MidStr.IsEmpty()?TEXT("<none>"):*MidStr);
			});
					// Initialize a small reoffer timer to ensure track opens after enabling
					{
						const double Now = FPlatformTime::Seconds();
						AudioOpenReofferBackoffSeconds = 1.0;
						NextAudioOpenReofferTimeSeconds = Now + AudioOpenReofferBackoffSeconds;
					}
		}
	}
#else
	UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("Open3DStream: Opus not fully available (library or headers missing). Audio send disabled."));
#endif
}

void FWebRTCConnector::DisableAudioSend()
{
	bAudioSendEnabled = false;
	AudioRt.Pending.Reset();
	AudioRt.bTrackReady = false;
	if (AudioTrack)
	{
		try { AudioTrack->close(); } catch (const std::exception&) {}
		AudioTrack.reset();
	}
}

bool FWebRTCConnector::PushAudioPCM16(const int16* Samples, int32 NumSamples)
{
	if (!bAudioSendEnabled)
	{
		if (CVarO3DSWebRTCVerbose->GetInt() !=0)
		{
			UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: PushAudioPCM16 called but audio send disabled"));
		}
		return false;
	}
#if O3DS_WITH_OPUS
	// Backoff if the last send failed due to track not open
	const double Now = FPlatformTime::Seconds();
	if (AudioRt.NextSendRetryTimeSeconds > 0.0 && Now < AudioRt.NextSendRetryTimeSeconds)
	{
		// Still allow buffering a little while we're backing off
		if (Samples && NumSamples > 0)
		{
			const int32 Old = AudioRt.Pending.Num();
			AudioRt.Pending.AddUninitialized(NumSamples);
			FMemory::Memcpy(AudioRt.Pending.GetData() + Old, Samples, sizeof(int16) * NumSamples);
		}
		// Indicate we accepted the audio for later send
		return true;
	}

	// Ensure encoder is ready (based on config set by EnableAudioSend)
	if (!EnsureOpusEncoder(AudioRt.Config))
	{
		return false;
	}

	// Append incoming samples to a bounded pending buffer so early frames aren't lost during negotiation
	if (Samples && NumSamples > 0)
	{
		int32 Old = AudioRt.Pending.Num();
		AudioRt.Pending.AddUninitialized(NumSamples);
		FMemory::Memcpy(AudioRt.Pending.GetData() + Old, Samples, sizeof(int16) * NumSamples);

		// Cap pending buffer to ~250ms to avoid unbounded growth
		const int32 MaxPendingSamples = (AudioRt.Config.SampleRate * AudioRt.Config.NumChannels) / 4; // 250ms
		if (AudioRt.Pending.Num() > MaxPendingSamples)
		{
			const int32 Drop = AudioRt.Pending.Num() - MaxPendingSamples;
			FMemory::Memmove(AudioRt.Pending.GetData(), AudioRt.Pending.GetData() + Drop, (AudioRt.Pending.Num() - Drop) * sizeof(int16));
			AudioRt.Pending.SetNum(MaxPendingSamples, EAllowShrinking::No);
		}

		if (CVarO3DSWebRTCVerbose->GetInt() != 0)
		{
			UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: PushAudioPCM16 buffered samples=%d pending=%d"), NumSamples, AudioRt.Pending.Num());
		}
	}

	// Snapshot connection state and track under lock to avoid races with CleanupPeerConnection
	std::shared_ptr<rtc::Track> LocalAudioTrack;
	std::shared_ptr<rtc::PeerConnection> LocalPC;
	bool bConnected = false;
	bool bHaveDescriptions = false;
	{
		FScopeLock Lock(&PeerConnectionLock);
		LocalAudioTrack = AudioTrack;
		LocalPC = PeerConnection;
		bConnected = bIsConnected;
		bHaveDescriptions = bLocalDescriptionSet && bRemoteDescriptionSet;
	}
	if (!LocalPC || !bConnected || !bHaveDescriptions)
	{
		static double sLastWarn = 0.0; const double nowWarn = FPlatformTime::Seconds();
		if (nowWarn - sLastWarn > 1.0)
		{
			UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("WebRTC: Audio not ready (connected=%d localDesc=%d remoteDesc=%d pc=%d), deferring frames"),
				bConnected ? 1 : 0, bLocalDescriptionSet ? 1 : 0, bRemoteDescriptionSet ? 1 : 0, LocalPC ? 1 : 0);
			sLastWarn = nowWarn;
		}
		// We buffered above; report accepted
		return true;
	}

	// If track exists but isn't open yet, still attempt a probe send (some stacks open on first frame).
	// Keep re-offer cadence active to help finalize negotiation.
	bool bTrackWasNotOpen = false;
	if (LocalAudioTrack && !LocalAudioTrack->isOpen())
	{
		bTrackWasNotOpen = true;
		AudioRt.bTrackReady = false;
		AudioRt.NextSendRetryTimeSeconds = Now + 0.25;
		if (!bIsServer)
		{
			const double Now2 = FPlatformTime::Seconds();
			if (Now2 >= NextAudioOpenReofferTimeSeconds)
			{
				MaybeCreateOffer(TEXT("audio-not-open"));
				AudioOpenReofferBackoffSeconds = FMath::Min(8.0, AudioOpenReofferBackoffSeconds * 2.0);
				const double Jitter = (double)FMath::FRandRange(0.0f, (float)(0.25 * AudioOpenReofferBackoffSeconds));
				NextAudioOpenReofferTimeSeconds = Now2 + AudioOpenReofferBackoffSeconds + Jitter;
				// If we've been reoffering for a while and the track still won't open, force a track recreate
				if (AudioOpenReofferBackoffSeconds >= 4.0)
				{
					UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("WebRTC Connector: Audio track still not open after reoffers; recreating track"));
					{
						FScopeLock Lock(&PeerConnectionLock);
						if (AudioTrack)
						{
							try { AudioTrack->close(); } catch (const std::exception&) {}
							AudioTrack.reset();
							LocalAudioTrack.reset();
						}
					}
					// Trigger a fresh offer so the new track appears in SDP immediately
					MaybeCreateOffer(TEXT("audio-recreate-track"));
				}
			}
		}
		if (CVarO3DSWebRTCVerbose->GetInt() != 0)
		{
			UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: PushAudioPCM16 probe send while track not open"));
		}
		// Fall through to try sending; exceptions will be caught below
	}

	// If we don't currently have a track, attempt to add one (e.g. after reconnect)
	if (!LocalAudioTrack && bAudioSendEnabled)
	{
		{
			FScopeLock Lock(&PeerConnectionLock);
			if (!AudioTrack && PeerConnection)
			{
				if (!AudioRt.Config.StreamLabel.IsEmpty())
				{
					AudioTrack = AddOpusAudioSendTrackWithLabel(PeerConnection, AudioRt.Config.BitrateKbps, AudioRt.Config.StreamLabel);
				}
				else
				{
					AudioTrack = AddOpusAudioSendTrack(PeerConnection, AudioRt.Config.BitrateKbps);
				}
				LocalAudioTrack = AudioTrack;
				if (AudioTrack)
				{
					// Reinstall media handler chain on re-attach
					uint32 Ssrc = 0;
					std::string CName = "o3ds";
					if (!AudioRt.Config.StreamLabel.IsEmpty())
					{
						Ssrc = 0xA17C0000u ^ FCrc::StrCrc32(*AudioRt.Config.StreamLabel);
					}
					else
					{
						Ssrc = 0xA17C1234u;
					}
					auto RtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(Ssrc, CName, 111, rtc::OpusRtpPacketizer::DefaultClockRate);
					auto Packetizer = std::make_shared<rtc::OpusRtpPacketizer>(RtpCfg);
					// Attempt to attach MID for BUNDLE demux if available on this libdatachannel version
					try {
						if (AudioTrack) {
							const std::string Mid = AudioTrack->mid();
							if (!Mid.empty()) {
								RtpCfg->mid = Mid; // may not exist on older libdatachannel; build will reveal
								UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: Attached MID '%s' to RTP config"), *FString(Mid.c_str()));
							}
						}
					} catch (...) {
						// best-effort; ignore if not supported
					}
					auto SrReporter = std::make_shared<rtc::RtcpSrReporter>(RtpCfg);
					Packetizer->addToChain(SrReporter);
					auto NackResponder = std::make_shared<rtc::RtcpNackResponder>();
					Packetizer->addToChain(NackResponder);
					AudioTrack->setMediaHandler(Packetizer);
					LocalAudioTrack->onOpen([this, Track=LocalAudioTrack]()
					{
						AudioRt.bTrackReady = true;
						if (CVarO3DSWebRTCVerbose->GetInt() != 0)
						{
							FString MidStr = Track ? FString(ANSI_TO_TCHAR(Track->mid().c_str())) : FString();
							UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: Audio track opened (mid=%s)"), MidStr.IsEmpty()?TEXT("<none>"):*MidStr);
						}
					});
					AudioTrack->onClosed([this, Track=AudioTrack]()
					{
						AudioRt.bTrackReady = false;
						if (CVarO3DSWebRTCVerbose->GetInt() != 0)
						{
							FString MidStr = Track ? FString(ANSI_TO_TCHAR(Track->mid().c_str())) : FString();
							UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: Audio track closed (mid=%s)"), MidStr.IsEmpty()?TEXT("<none>"):*MidStr);
						}
					});
				}
			}
		}
		if (!bIsServer && bSignalingIsConnected)
		{
			MaybeCreateOffer(TEXT("audio-ensure-before-send"));
		}
		if (!LocalAudioTrack)
		{
			// Avoid tight loop
			AudioRt.NextSendRetryTimeSeconds = Now + 0.25;
			return true; // accepted into buffer
		}
	}

	if (!Samples || NumSamples <= 0)
	{
		return true; // nothing to encode yet
	}

	const int32 FrameSamplesTotal = AudioRt.FrameSizeSamples * AudioRt.Config.NumChannels;
	// Step in48kHz RTP clock regardless of source sample rate
	const uint32 Step48k = (uint32)((int64)AudioRt.Config.FrameSizeMs *48000 /1000);
	uint8 Encoded[4000];
	while (AudioRt.Pending.Num() >= FrameSamplesTotal)
	{
		int16* FramePtr = AudioRt.Pending.GetData();
		int EncBytes = opus_encode(OpusEnc, FramePtr, AudioRt.FrameSizeSamples, Encoded, sizeof(Encoded));
		if (EncBytes >0)
		{
			// Send over audio track
			rtc::binary Packet;
			Packet.resize(EncBytes);
			for (int i =0; i < EncBytes; ++i) Packet[i] = static_cast<std::byte>(Encoded[i]);
			// RTP timestamp increments in48kHz clock
			rtc::FrameInfo FI{ (uint32)AudioRt.Timestamp };
			AudioRt.Timestamp += Step48k;
			if (LocalAudioTrack)
			{
				try
				{
					LocalAudioTrack->sendFrame(Packet, FI);
				}
				catch (const std::exception& e)
				{
					UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("WebRTC Connector: sendFrame threw exception: %s"), *FString(ANSI_TO_TCHAR(e.what())));
					// Backoff and mark not ready; try to re-add track on next attempt
					AudioRt.bTrackReady = false;
					AudioRt.NextSendRetryTimeSeconds = FPlatformTime::Seconds() +0.25;
					return false;
				}
			}
			// Update debug counters
			AudioRt.SentPackets += 1;
			AudioRt.SentBytes += (uint64)EncBytes;
			if (CVarO3DSWebRTCVerbose->GetInt() !=0)
			{
				UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("WebRTC Connector: Encoded and sent audio packet %d bytes (timestamp=%u)"), EncBytes, FI.timestamp);
			}
			AudioRt.bTrackReady = true;
		}
		// Pop consumed samples
		const int32 Remaining = AudioRt.Pending.Num() - FrameSamplesTotal;
		if (Remaining >0)
		{
			FMemory::Memmove(AudioRt.Pending.GetData(), AudioRt.Pending.GetData() + FrameSamplesTotal, Remaining * sizeof(int16));
		}
		AudioRt.Pending.SetNum(Remaining, EAllowShrinking::No);
	}
	return true;
#else
	(void)Samples; (void)NumSamples; return false;
#endif
}

void FWebRTCConnector::SetAudioReceiveCallback(TFunction<void(const int16* PCM, int32 NumSamples, int32 NumChannels, int32 SampleRate)> Callback)
{
	AudioRxCallback = MoveTemp(Callback);
}

void FWebRTCConnector::GetAudioSendStatus(FAudioSendStatus& OutStatus) const
{
	// Snapshot state, guarding shared resources
	std::shared_ptr<rtc::Track> LocalAudioTrack;
	{
		FScopeLock Lock(const_cast<FCriticalSection*>(&PeerConnectionLock));
		LocalAudioTrack = AudioTrack;
	}

	OutStatus.bSignalingConnected = bSignalingIsConnected;
	OutStatus.bPeerConnected = bIsConnected;
	OutStatus.bLocalDesc = bLocalDescriptionSet;
	OutStatus.bRemoteDesc = bRemoteDescriptionSet;
	OutStatus.bLocalSdpHasAudio = bLocalSDPHasAudio;
	OutStatus.bRemoteSdpHasAudio = bRemoteSDPHasAudio;
	OutStatus.bDataChannelOpen = bDataChannelOpen;
	OutStatus.bAudioSendEnabled = bAudioSendEnabled;
	OutStatus.bAudioTrackPresent = (LocalAudioTrack != nullptr);
	OutStatus.bAudioTrackOpen = (LocalAudioTrack ? LocalAudioTrack->isOpen() : false);
	OutStatus.bOpusEncoderReady = (OpusEnc != nullptr);
	OutStatus.LastPeerStateInt = LastPeerState;
	OutStatus.ConnectionStateLabel = ConnectionState;
	OutStatus.StreamLabel = AudioRt.Config.StreamLabel;
	OutStatus.PendingSamples = AudioRt.Pending.Num();
	OutStatus.SentPackets = AudioRt.SentPackets;
	OutStatus.SentBytes = AudioRt.SentBytes;
	OutStatus.LastError = LastError;

	// Summarize SDP directions and codec presence
	{
		const int lr = bLocalDirRecvOnly?1:0, ls = bLocalDirSendOnly?1:0, lsr = bLocalDirSendRecv?1:0, li = bLocalDirInactive?1:0;
		OutStatus.LocalAudioDir = FString::Printf(TEXT("recvonly=%d sendonly=%d sendrecv=%d inactive=%d"), lr, ls, lsr, li);
		OutStatus.bLocalOpus111 = bLocalHasOpus111;
	}

	{
		const int rr = bRemoteDirRecvOnly?1:0, rs = bRemoteDirSendOnly?1:0, rsr = bRemoteDirSendRecv?1:0, ri = bRemoteDirInactive?1:0;
		OutStatus.RemoteAudioDir = FString::Printf(TEXT("recvonly=%d sendonly=%d sendrecv=%d inactive=%d"), rr, rs, rsr, ri);
		OutStatus.bRemoteOpus111 = bRemoteHasOpus111;
	}
}

#if O3DS_WITH_OPUS
bool FWebRTCConnector::EnsureOpusEncoder(const FAudioConfig& In)
{
	if (OpusEnc)
	{
		return true;
	}
	int Err =0;
	OpusEnc = opus_encoder_create(In.SampleRate, In.NumChannels, OPUS_APPLICATION_AUDIO, &Err);
	if (!OpusEnc || Err != OPUS_OK)
	{
		OpusEnc = nullptr;
		UE_LOG(O3DSWebRTCAudioLog, Error, TEXT("Opus: Failed to create encoder err=%d"), Err);
		return false;
	}
	opus_encoder_ctl(OpusEnc, OPUS_SET_BITRATE(In.BitrateKbps *1000));
	opus_encoder_ctl(OpusEnc, OPUS_SET_COMPLEXITY(5));
	opus_encoder_ctl(OpusEnc, OPUS_SET_INBAND_FEC(1));
	if (CVarO3DSWebRTCVerbose->GetInt() != 0)
	{
		UE_LOG(O3DSWebRTCAudioLog, Verbose, TEXT("Opus: Encoder created sr=%d ch=%d br=%d kbps"), In.SampleRate, In.NumChannels, In.BitrateKbps);
	}
	return true;
}

bool FWebRTCConnector::EnsureOpusDecoder(int32 SampleRate, int32 NumChannels)
{
	if (OpusDec)
	{
		return true;
	}
	int Err =0;
	OpusDec = opus_decoder_create(SampleRate, NumChannels, &Err);
	if (!OpusDec || Err != OPUS_OK)
	{
		OpusDec = nullptr;
		UE_LOG(O3DSWebRTCAudioLog, Error, TEXT("Opus: Failed to create decoder err=%d"), Err);
		return false;
	}
	return true;
}

void FWebRTCConnector::DestroyOpus()
{
	if (OpusEnc)
	{
		opus_encoder_destroy(OpusEnc);
		OpusEnc = nullptr;
	}
	if (OpusDec)
	{
		opus_decoder_destroy(OpusDec);
		OpusDec = nullptr;
	}
}
#endif

// Static active connector registry
TWeakPtr<FWebRTCConnector> FWebRTCConnector::ActiveConnector;

void FWebRTCConnector::SetActiveConnector(const TSharedPtr<FWebRTCConnector>& InConnector)
{
	ActiveConnector = InConnector;
}

TSharedPtr<FWebRTCConnector> FWebRTCConnector::GetActiveConnector()
{
	return ActiveConnector.Pin();
}

// Console command to print current audio send status for broadcaster
static FAutoConsoleCommand CmdO3DSDumpAudioStatus(
	TEXT("o3ds.WebRTC.Audio.Status"),
	TEXT("Print Open3DStream WebRTC audio send status (client/broadcaster)."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		TSharedPtr<FWebRTCConnector> Conn = FWebRTCConnector::GetActiveConnector();
		if (!Conn.IsValid())
		{
			UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("O3DS: No active WebRTC connector"));
			return;
		}
		FWebRTCConnector::FAudioSendStatus S; Conn->GetAudioSendStatus(S);
		UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("--- O3DS WebRTC Audio Send Status ---"));
		UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("Signaling=%d PeerConnected=%d LocalDesc=%d RemoteDesc=%d DataChannelOpen=%d"),
			S.bSignalingConnected?1:0, S.bPeerConnected?1:0, S.bLocalDesc?1:0, S.bRemoteDesc?1:0, S.bDataChannelOpen?1:0);
		UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("LocalSDP.m=audio=%d RemoteSDP.m=audio=%d"), S.bLocalSdpHasAudio?1:0, S.bRemoteSdpHasAudio?1:0);
		UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("Local SDP audio dir: %s opus111=%d"), *S.LocalAudioDir, S.bLocalOpus111?1:0);
		UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("Remote SDP audio dir: %s opus111=%d"), *S.RemoteAudioDir, S.bRemoteOpus111?1:0);
		UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("AudioEnabled=%d TrackPresent=%d TrackOpen=%d OpusReady=%d"),
			S.bAudioSendEnabled?1:0, S.bAudioTrackPresent?1:0, S.bAudioTrackOpen?1:0, S.bOpusEncoderReady?1:0);
		UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("PeerState=%d ConnState=%s StreamLabel=%s"),
			S.LastPeerStateInt, *S.ConnectionStateLabel, *S.StreamLabel);
		UE_LOG(O3DSWebRTCAudioLog, Log, TEXT("PendingSamples=%d SentPackets=%llu SentBytes=%llu"),
			S.PendingSamples, (unsigned long long)S.SentPackets, (unsigned long long)S.SentBytes);
		if (!S.LastError.IsEmpty())
		{
			UE_LOG(O3DSWebRTCAudioLog, Warning, TEXT("LastError: %s"), *S.LastError);
		}
	})
);
