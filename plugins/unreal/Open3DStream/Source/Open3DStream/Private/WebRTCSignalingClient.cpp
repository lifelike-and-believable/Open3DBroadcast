#include "WebRTCSignalingClient.h"

FWebRTCSignalingClient::FWebRTCSignalingClient()
	: bIsConnected(false)
	, bIsServerMode(false)
{
	LastError.Empty();
}

FWebRTCSignalingClient::~FWebRTCSignalingClient()
{
	Disconnect();
}

bool FWebRTCSignalingClient::Connect(const FString& SignalingUrl, const FString& RoomName, bool bInIsServer)
{
	if (bIsConnected)
	{
		LastError = TEXT("Already connected to signaling server");
		return false;
	}

	CurrentRoomName = RoomName;
	bIsServerMode = bInIsServer;

	// Parse WebSocket URL
	FString FinalUrl = SignalingUrl;
	if (!FinalUrl.EndsWith(TEXT("/ws")))
	{
		FinalUrl += TEXT("/ws");
	}

	UE_LOG(LogTemp, Log, TEXT("WebRTC Signaling: Connecting to %s (Room: %s, Mode: %s)"), 
		*FinalUrl, 
		*RoomName, 
		bInIsServer ? TEXT("Server") : TEXT("Client"));

	// Create WebSocket connection
	if (!FModuleManager::Get().IsModuleLoaded("WebSockets"))
	{
		FModuleManager::Get().LoadModule("WebSockets");
	}

	WebSocket = FWebSocketsModule::Get().CreateWebSocket(FinalUrl);
	if (!WebSocket.IsValid())
	{
		LastError = TEXT("Failed to create WebSocket");
		UE_LOG(LogTemp, Error, TEXT("WebRTC Signaling: %s"), *LastError);
		return false;
	}

	// Bind WebSocket delegates
	WebSocket->OnConnected().AddRaw(this, &FWebRTCSignalingClient::OnWebSocketConnected);
	WebSocket->OnMessage().AddRaw(this, &FWebRTCSignalingClient::OnWebSocketMessage);
	WebSocket->OnClosed().AddRaw(this, &FWebRTCSignalingClient::OnWebSocketClosed);
	WebSocket->OnConnectionError().AddRaw(this, &FWebRTCSignalingClient::OnWebSocketError);

	// Connect
	WebSocket->Connect();

	return true;
}

void FWebRTCSignalingClient::Disconnect()
{
	if (WebSocket.IsValid())
	{
		WebSocket->OnConnected().RemoveAll(this);
		WebSocket->OnMessage().RemoveAll(this);
		WebSocket->OnClosed().RemoveAll(this);
		WebSocket->OnConnectionError().RemoveAll(this);

		WebSocket->Close();
		WebSocket.Reset();
	}

	bIsConnected = false;
}

void FWebRTCSignalingClient::SendOffer(const FString& SDP)
{
	if (!bIsConnected || !WebSocket.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Signaling: Cannot send offer - not connected"));
		return;
	}

	FString Message = CreateOfferMessage(SDP);
	WebSocket->Send(Message);
	UE_LOG(LogTemp, Log, TEXT("WebRTC Signaling: Sent offer"));
}

void FWebRTCSignalingClient::SendAnswer(const FString& SDP)
{
	if (!bIsConnected || !WebSocket.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Signaling: Cannot send answer - not connected"));
		return;
	}

	FString Message = CreateAnswerMessage(SDP);
	WebSocket->Send(Message);
	UE_LOG(LogTemp, Log, TEXT("WebRTC Signaling: Sent answer"));
}

void FWebRTCSignalingClient::SendIceCandidate(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex)
{
	if (!bIsConnected || !WebSocket.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Signaling: Cannot send ICE candidate - not connected"));
		return;
	}

	FString Message = CreateIceCandidateMessage(Candidate, SdpMid, SdpMLineIndex);
	WebSocket->Send(Message);
}

void FWebRTCSignalingClient::OnWebSocketConnected()
{
	UE_LOG(LogTemp, Log, TEXT("WebRTC Signaling: WebSocket connected"));
	
	// Send join message
	FString JoinMsg = CreateJoinMessage();
	WebSocket->Send(JoinMsg);
}

void FWebRTCSignalingClient::OnWebSocketMessage(const FString& Message)
{
	UE_LOG(LogTemp, Verbose, TEXT("WebRTC Signaling: Received message: %s"), *Message);
	ParseSignalingMessage(Message);
}

void FWebRTCSignalingClient::OnWebSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogTemp, Warning, TEXT("WebRTC Signaling: WebSocket closed (Code: %d, Reason: %s, Clean: %d)"), 
		StatusCode, *Reason, bWasClean ? 1 : 0);
	
	bIsConnected = false;
	
	if (OnSignalingDisconnected)
	{
		OnSignalingDisconnected(Reason);
	}
}

void FWebRTCSignalingClient::OnWebSocketError(const FString& Error)
{
	LastError = Error;
	UE_LOG(LogTemp, Error, TEXT("WebRTC Signaling: WebSocket error: %s"), *Error);
	
	if (OnSignalingError)
	{
		OnSignalingError(Error);
	}
}

bool FWebRTCSignalingClient::ParseSignalingMessage(const FString& MessageStr)
{
	// Parse JSON
	TSharedPtr<FJsonObject> JsonMessage;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MessageStr);
	if (!FJsonSerializer::Deserialize(Reader, JsonMessage) || !JsonMessage.IsValid())
	{
		LastError = TEXT("Failed to parse signaling message JSON");
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Signaling: %s - Raw: %s"), *LastError, *MessageStr);
		return false;
	}

	FString MessageType = JsonMessage->GetStringField(TEXT("type"));
	UE_LOG(LogTemp, Log, TEXT("WebRTC Signaling: Received message type: %s"), *MessageType);

	if (MessageType == TEXT("offer"))
	{
		return ParseOffer(JsonMessage);
	}
	else if (MessageType == TEXT("answer"))
	{
		return ParseAnswer(JsonMessage);
	}
	else if (MessageType == TEXT("ice"))
	{
		return ParseIceCandidate(JsonMessage);
	}
	else if (MessageType == TEXT("peer-joined"))
	{
		return ParsePeerJoined(JsonMessage);
	}
	else if (MessageType == TEXT("peer-left"))
	{
		return ParsePeerLeft(JsonMessage);
	}
	else if (MessageType == TEXT("joined"))
	{
		// Successfully joined room
		bIsConnected = true;
		UE_LOG(LogTemp, Log, TEXT("WebRTC Signaling: Successfully joined room '%s'"), *CurrentRoomName);
		
		if (OnSignalingConnected)
		{
			OnSignalingConnected();
		}
		return true;
	}

	LastError = FString::Printf(TEXT("Unknown message type: %s"), *MessageType);
	UE_LOG(LogTemp, Warning, TEXT("WebRTC Signaling: %s"), *LastError);
	return false;
}

bool FWebRTCSignalingClient::ParseOffer(const TSharedPtr<FJsonObject>& JsonMessage)
{
	if (!JsonMessage->HasField(TEXT("sdp")))
	{
		LastError = TEXT("Offer message missing SDP field");
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Signaling: %s"), *LastError);
		return false;
	}

	FString SDP = JsonMessage->GetStringField(TEXT("sdp"));
	
	if (OnOfferReceived)
	{
		OnOfferReceived(SDP);
	}

	return true;
}

bool FWebRTCSignalingClient::ParseAnswer(const TSharedPtr<FJsonObject>& JsonMessage)
{
	if (!JsonMessage->HasField(TEXT("sdp")))
	{
		LastError = TEXT("Answer message missing SDP field");
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Signaling: %s"), *LastError);
		return false;
	}

	FString SDP = JsonMessage->GetStringField(TEXT("sdp"));
	
	if (OnAnswerReceived)
	{
		OnAnswerReceived(SDP);
	}

	return true;
}

bool FWebRTCSignalingClient::ParseIceCandidate(const TSharedPtr<FJsonObject>& JsonMessage)
{
	if (!JsonMessage->HasField(TEXT("candidate")) || 
		!JsonMessage->HasField(TEXT("sdpMid")) || 
		!JsonMessage->HasField(TEXT("sdpMLineIndex")))
	{
		LastError = TEXT("ICE candidate message missing required fields");
		UE_LOG(LogTemp, Warning, TEXT("WebRTC Signaling: %s"), *LastError);
		return false;
	}

	FString Candidate = JsonMessage->GetStringField(TEXT("candidate"));
	FString SdpMid = JsonMessage->GetStringField(TEXT("sdpMid"));
	int32 SdpMLineIndex = JsonMessage->GetIntegerField(TEXT("sdpMLineIndex"));

	if (OnIceCandidateReceived)
	{
		OnIceCandidateReceived(Candidate, SdpMid, SdpMLineIndex);
	}

	return true;
}

bool FWebRTCSignalingClient::ParsePeerJoined(const TSharedPtr<FJsonObject>& JsonMessage)
{
	UE_LOG(LogTemp, Log, TEXT("WebRTC Signaling: Peer joined room"));
	
	if (OnPeerJoined)
	{
		OnPeerJoined();
	}

	return true;
}

bool FWebRTCSignalingClient::ParsePeerLeft(const TSharedPtr<FJsonObject>& JsonMessage)
{
	UE_LOG(LogTemp, Log, TEXT("WebRTC Signaling: Peer left room"));
	
	if (OnPeerLeft)
	{
		OnPeerLeft();
	}

	return true;
}

FString FWebRTCSignalingClient::CreateJoinMessage() const
{
	TSharedPtr<FJsonObject> JsonMessage = MakeShareable(new FJsonObject());
	JsonMessage->SetStringField(TEXT("type"), TEXT("join"));
	JsonMessage->SetStringField(TEXT("room"), CurrentRoomName);
	JsonMessage->SetStringField(TEXT("name"), bIsServerMode ? TEXT("unreal-server") : TEXT("unreal-client"));

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonMessage.ToSharedRef(), Writer);

	return OutputString;
}

FString FWebRTCSignalingClient::CreateOfferMessage(const FString& SDP) const
{
	TSharedPtr<FJsonObject> JsonMessage = MakeShareable(new FJsonObject());
	JsonMessage->SetStringField(TEXT("type"), TEXT("offer"));
	JsonMessage->SetStringField(TEXT("sdp"), SDP);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonMessage.ToSharedRef(), Writer);

	return OutputString;
}

FString FWebRTCSignalingClient::CreateAnswerMessage(const FString& SDP) const
{
	TSharedPtr<FJsonObject> JsonMessage = MakeShareable(new FJsonObject());
	JsonMessage->SetStringField(TEXT("type"), TEXT("answer"));
	JsonMessage->SetStringField(TEXT("sdp"), SDP);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonMessage.ToSharedRef(), Writer);

	return OutputString;
}

FString FWebRTCSignalingClient::CreateIceCandidateMessage(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex) const
{
	TSharedPtr<FJsonObject> JsonMessage = MakeShareable(new FJsonObject());
	JsonMessage->SetStringField(TEXT("type"), TEXT("ice"));
	JsonMessage->SetStringField(TEXT("candidate"), Candidate);
	JsonMessage->SetStringField(TEXT("sdpMid"), SdpMid);
	JsonMessage->SetNumberField(TEXT("sdpMLineIndex"), SdpMLineIndex);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonMessage.ToSharedRef(), Writer);

	return OutputString;
}
