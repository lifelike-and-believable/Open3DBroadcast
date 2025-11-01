#include "O3DSTestLibDCServerComponent.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Modules/ModuleManager.h"

#include <rtc/rtc.hpp>

// Local helpers
static std::string ToStd(const FString& S)
{
	FTCHARToUTF8 C(*S);
	return std::string(C.Get(), C.Length());
}
static FString ToFStr(const std::string& S)
{
	return FString(UTF8_TO_TCHAR(S.c_str()));
}

UO3DSTestLibDCServerComponent::UO3DSTestLibDCServerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	RemotePeerId.Reset();
}

void UO3DSTestLibDCServerComponent::BeginPlay()
{
	Super::BeginPlay();
	rtc::InitLogger(bVerbose ? rtc::LogLevel::Info : rtc::LogLevel::Warning);
	OpenWebSocket();
}

void UO3DSTestLibDCServerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (WS.IsValid())
	{
		WS->Close();
		WS.Reset();
	}
	// Reset refs
	RecvAudioTrack.reset();
	ServerDC.reset();
	PC.reset();
	RemotePeerId.Reset();
	Super::EndPlay(EndPlayReason);
}

void UO3DSTestLibDCServerComponent::OpenWebSocket()
{
	FWebSocketsModule& WSM = FModuleManager::LoadModuleChecked<FWebSocketsModule>("WebSockets");

	// Build URL with forward slashes
	FString Url = SignalingUrlBase;
	if (!Url.EndsWith(TEXT("/"))) Url += TEXT("/");
	Url += LocalId;

	WS = WSM.CreateWebSocket(Url);

	WS->OnConnected().AddLambda([this]()
	{
		UE_LOG(LogTemp, Log, TEXT("[LibDC Server] WebSocket connected"));
	});

	WS->OnConnectionError().AddLambda([this, UrlCopy = Url](const FString& Err)
	{
		UE_LOG(LogTemp, Error, TEXT("[LibDC Server] WebSocket error: %s (url=%s)"), *Err, *UrlCopy);
	});

	WS->OnClosed().AddLambda([this](int32 /*StatusCode*/, const FString& /*Reason*/, bool /*bWasClean*/)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LibDC Server] WebSocket closed"));
	});

	WS->OnMessage().AddLambda([this](const FString& Msg)
	{
		HandleIncomingJson(Msg);
	});

	UE_LOG(LogTemp, Log, TEXT("[LibDC Server] Connecting signaling: %s"), *Url);
	WS->Connect();
}

// Helper to attach callbacks to a receive audio track (pre-added or onTrack)
void UO3DSTestLibDCServerComponent::AttachRecvAudioCallbacks(const std::shared_ptr<rtc::Track>& Track)
{
	if (!Track) return;

	UE_LOG(LogTemp, Log, TEXT("[LibDC Server] Attaching callbacks to recv audio track. mid=%s dir=%d"),
		*ToFStr(Track->mid()), (int)Track->direction());

	Track->onOpen([this]() { UE_LOG(LogTemp, Log, TEXT("[LibDC Server] Audio track opened")); });
	Track->onClosed([this]() { UE_LOG(LogTemp, Warning, TEXT("[LibDC Server] Audio track closed")); });
	Track->onMessage([this](auto M)
	{
		if (std::holds_alternative<rtc::binary>(M))
		{
			const auto& Bin = std::get<rtc::binary>(M);
			// Promote to Log so it’s visible during tests
			UE_LOG(LogTemp, Log, TEXT("[LibDC Server] Audio RTP packet: %d bytes"), (int)Bin.size());
		}
	});
}

void UO3DSTestLibDCServerComponent::SetupPeerConnection(const rtc::Configuration& Cfg)
{
	PC = std::make_shared<rtc::PeerConnection>(Cfg);

	// Pre-provision a recvonly audio transceiver so answering includes audio m-line
	try
	{
		rtc::Description::Audio RecvMedia("audio", rtc::Description::Direction::RecvOnly);
		RecvMedia.addOpusCodec(111);
		RecvAudioTrack = PC->addTrack(RecvMedia);
		UE_LOG(LogTemp, Log, TEXT("[LibDC Server] Pre-added RecvOnly audio transceiver with Opus"));
		// Attach callbacks now because onTrack may not fire for pre-added tracks
		AttachRecvAudioCallbacks(RecvAudioTrack);
	}
	catch (...)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LibDC Server] Failed to pre-add RecvOnly audio transceiver"));
	}

	PC->onStateChange([this](rtc::PeerConnection::State S)
	{
		UE_LOG(LogTemp, Log, TEXT("[LibDC Server] PC state: %d"), (int)S);
	});

	PC->onLocalDescription([this](rtc::Description Desc)
	{
		if (RemotePeerId.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("[LibDC Server] RemotePeerId not set; cannot send local description"));
			return;
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("id"), RemotePeerId);
		Msg->SetStringField(TEXT("type"), UTF8_TO_TCHAR(Desc.typeString().c_str()));
		Msg->SetStringField(TEXT("description"), UTF8_TO_TCHAR(std::string(Desc).c_str()));
		SendJson(Msg);
	});

	PC->onLocalCandidate([this](rtc::Candidate Cand)
	{
		if (RemotePeerId.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("[LibDC Server] RemotePeerId not set; cannot send candidate"));
			return;
		}

		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("id"), RemotePeerId);
		Msg->SetStringField(TEXT("type"), TEXT("candidate"));
		Msg->SetStringField(TEXT("candidate"), UTF8_TO_TCHAR(std::string(Cand).c_str()));
		Msg->SetStringField(TEXT("mid"), UTF8_TO_TCHAR(Cand.mid().c_str()));
		SendJson(Msg);
	});

	PC->onDataChannel([this](std::shared_ptr<rtc::DataChannel> DC)
	{
		ServerDC = DC; // keep strong ref (debug aid)
		UE_LOG(LogTemp, Log, TEXT("[LibDC Server] DataChannel received: %s"), *ToFStr(DC->label()));
		DC->onOpen([this]() { UE_LOG(LogTemp, Log, TEXT("[LibDC Server] DataChannel opened")); });
		DC->onClosed([this]() { UE_LOG(LogTemp, Warning, TEXT("[LibDC Server] DataChannel closed")); });
		DC->onMessage([this](rtc::message_variant M)
		{
			if (std::holds_alternative<std::string>(M))
			{
				UE_LOG(LogTemp, Log, TEXT("[LibDC Server] Text: %s"), *ToFStr(std::get<std::string>(M)));
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("[LibDC Server] Binary: %d bytes"), (int)std::get<rtc::binary>(M).size());
			}
		});
	});

	PC->onTrack([this](std::shared_ptr<rtc::Track> Track)
	{
		UE_LOG(LogTemp, Log, TEXT("[LibDC Server] Audio track received. mid=%s dir=%d"),
			*ToFStr(Track->mid()), (int)Track->direction());

		// Keep and attach callbacks (handles non-pre-added case or additional tracks)
		RecvAudioTrack = Track;
		AttachRecvAudioCallbacks(RecvAudioTrack);
	});
}

void UO3DSTestLibDCServerComponent::SendJson(const TSharedPtr<FJsonObject>& Obj)
{
	if (!WS.IsValid()) return;
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
	WS->Send(Out);
}

void UO3DSTestLibDCServerComponent::HandleIncomingJson(const FString& JsonStr)
{
	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return;

	const FString Type = Obj->GetStringField(TEXT("type"));
	const FString Id = Obj->GetStringField(TEXT("id"));

	if (Type == TEXT("offer"))
	{
		RemotePeerId = Id; // remember who to reply to
		UE_LOG(LogTemp, Log, TEXT("[LibDC Server] Offer from %s"), *RemotePeerId);

		rtc::Configuration Cfg; // no STUN, parity with sample
		if (!PC) SetupPeerConnection(Cfg);

		const FString Sdp = Obj->GetStringField(TEXT("description"));
		PC->setRemoteDescription(rtc::Description(ToStd(Sdp), "offer"));

		// Be explicit: create the answer after setting remote offer
		PC->createAnswer();
	}
	else if (Type == TEXT("answer"))
	{
		if (!PC) return;
		const FString Sdp = Obj->GetStringField(TEXT("description"));
		PC->setRemoteDescription(rtc::Description(ToStd(Sdp), "answer"));
	}
	else if (Type == TEXT("candidate"))
	{
		if (!PC) return;
		const FString Cand = Obj->GetStringField(TEXT("candidate"));
		const FString Mid = Obj->GetStringField(TEXT("mid"));
		PC->addRemoteCandidate(rtc::Candidate(ToStd(Cand), ToStd(Mid)));
	}
}