#include "O3DSTestLibDCClientComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Modules/ModuleManager.h"

#include <rtc/rtc.hpp>
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

UO3DSTestLibDCClientComponent::UO3DSTestLibDCClientComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UO3DSTestLibDCClientComponent::BeginPlay()
{
	Super::BeginPlay();
	rtc::InitLogger(bVerbose ? rtc::LogLevel::Info : rtc::LogLevel::Warning);
	OpenWebSocket();
}

void UO3DSTestLibDCClientComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (WS.IsValid())
	{
		WS->Close();
		WS.Reset();
	}
	DC.reset();
	AudioTrack.reset();
	PC.reset();
	Super::EndPlay(EndPlayReason);
}

void UO3DSTestLibDCClientComponent::OpenWebSocket()
{
	FWebSocketsModule& WSM = FModuleManager::LoadModuleChecked<FWebSocketsModule>("WebSockets");

	FString Url = FullUrlOverride;
	if (Url.IsEmpty())
	{
		Url = SignalingUrlBase;
		if (bAppendLocalIdToUrl)
		{
			if (!Url.EndsWith(TEXT("/"))) Url += TEXT("/");
			Url += LocalId;
		}
	}

	WS = WSM.CreateWebSocket(Url);

	// ASYNC: no blocking waits. Chain setup on OnConnected.
	WS->OnConnected().AddLambda([this, Url]()
	{
		UE_LOG(LogTemp, Log, TEXT("[LibDC Client] WebSocket connected (%s)"), *Url);

		// Create PC after signaling is up (parity with sample)
		rtc::Configuration Cfg; // no STUN
		SetupPeerConnection(Cfg);

		// Audio track BEFORE data channel
		CreateAudioTrackBeforeDC();

		// Create DC (offerer) and wire callbacks
		DC = PC->createDataChannel("test");
		DC->onOpen([this]()
		{
			UE_LOG(LogTemp, Log, TEXT("[LibDC Client] DataChannel opened"));
			bDCOpen = true;
			SendHelloAndTone();
		});
		DC->onClosed([this]() { UE_LOG(LogTemp, Warning, TEXT("[LibDC Client] DataChannel closed")); });
		DC->onMessage([this](auto M)
		{
			if (std::holds_alternative<std::string>(M))
				UE_LOG(LogTemp, Log, TEXT("[LibDC Client] Received: %s"), *ToFStr(std::get<std::string>(M)));
		});
	});

	WS->OnConnectionError().AddLambda([this, Url](const FString& Err)
	{
		UE_LOG(LogTemp, Error, TEXT("[LibDC Client] WebSocket error: %s (url=%s)"), *Err, *Url);
	});

	WS->OnClosed().AddLambda([this, Url](int32 /*Status*/, const FString& Reason, bool /*bClean*/)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LibDC Client] WebSocket closed (url=%s) reason=%s"), *Url, *Reason);
	});

	WS->OnMessage().AddLambda([this](const FString& Msg)
	{
		HandleIncomingJson(Msg);
	});

	UE_LOG(LogTemp, Log, TEXT("[LibDC Client] Connecting signaling: %s"), *Url);
	WS->Connect();

	// IMPORTANT: removed blocking promise/future wait here
}


void UO3DSTestLibDCClientComponent::SetupPeerConnection(const rtc::Configuration& Cfg)
{
	PC = std::make_shared<rtc::PeerConnection>(Cfg);

	PC->onStateChange([this](rtc::PeerConnection::State S)
	{
		UE_LOG(LogTemp, Log, TEXT("[LibDC Client] PC state: %d"), (int)S);
	});

	PC->onLocalDescription([this](rtc::Description Desc)
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("id"), RemoteId);
		Msg->SetStringField(TEXT("type"), UTF8_TO_TCHAR(Desc.typeString().c_str()));
		Msg->SetStringField(TEXT("description"), UTF8_TO_TCHAR(std::string(Desc).c_str()));
		SendJson(Msg);
	});

	PC->onLocalCandidate([this](rtc::Candidate Cand)
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("id"), RemoteId);
		Msg->SetStringField(TEXT("type"), TEXT("candidate"));
		Msg->SetStringField(TEXT("candidate"), UTF8_TO_TCHAR(std::string(Cand).c_str()));
		Msg->SetStringField(TEXT("mid"), UTF8_TO_TCHAR(Cand.mid().c_str()));
		SendJson(Msg);
	});
}

void UO3DSTestLibDCClientComponent::CreateAudioTrackBeforeDC()
{
	if (!bSendAudio) return;

	const uint32 SSRC = 1234;
	rtc::Description::Audio Media("audio", rtc::Description::Direction::SendOnly);
	Media.addOpusCodec(111);
	Media.addSSRC(SSRC, "ue-audio");

	UE_LOG(LogTemp, Log, TEXT("[LibDC Client] Adding audio track BEFORE datachannel"));
	AudioTrack = PC->addTrack(Media);

	AudioTrack->onOpen([this]()
	{
		UE_LOG(LogTemp, Log, TEXT("[LibDC Client] Audio track opened"));
		bAudioOpen = true;
	});
	AudioTrack->onClosed([this]()
	{
		UE_LOG(LogTemp, Warning, TEXT("[LibDC Client] Audio track closed"));
	});
}

void UO3DSTestLibDCClientComponent::SendHelloAndTone()
{
	if (DC && bDCOpen.load())
	{
		DC->send(std::string("Hello from UE client - audio test!"));
		UE_LOG(LogTemp, Log, TEXT("[LibDC Client] Sent hello message"));
	}

	if (bSendAudio && AudioTrack)
	{
		std::thread([this]()
		{
			for (int i = 0; i < 100 && !bAudioOpen.load(); ++i)
				std::this_thread::sleep_for(100ms);
			if (!bAudioOpen.load())
			{
				UE_LOG(LogTemp, Error, TEXT("[LibDC Client] Timeout waiting for audio track to open"));
				return;
			}
			SendFakeOpusRtpTone(1.0);
		}).detach();
	}
}

void UO3DSTestLibDCClientComponent::SendJson(const TSharedPtr<FJsonObject>& Obj)
{
	if (!WS.IsValid()) return;
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
	WS->Send(Out);
}

void UO3DSTestLibDCClientComponent::HandleIncomingJson(const FString& JsonStr)
{
	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return;

	const FString Type = Obj->GetStringField(TEXT("type"));
	if (Type == TEXT("offer") || Type == TEXT("answer"))
	{
		const FString Sdp = Obj->GetStringField(TEXT("description"));
		PC->setRemoteDescription(rtc::Description(ToStd(Sdp), ToStd(Type)));
	}
	else if (Type == TEXT("candidate"))
	{
		const FString Cand = Obj->GetStringField(TEXT("candidate"));
		const FString Mid = Obj->GetStringField(TEXT("mid"));
		PC->addRemoteCandidate(rtc::Candidate(ToStd(Cand), ToStd(Mid)));
	}
}

void UO3DSTestLibDCClientComponent::GenerateSinePCM16(std::vector<uint8>& Out, double Freq, double DurationSec, int SR)
{
	const int NumSamples = static_cast<int>(DurationSec * SR);
	Out.resize(NumSamples * 2);
	for (int i = 0; i < NumSamples; ++i)
	{
		double t = (double)i / (double)SR;
		double v = sin(2.0 * PI * Freq * t) * 0.5; // 50% vol
		int16 S = (int16)FMath::RoundToInt(v * 32767.0);
		Out[2 * i + 0] = (uint8)(S & 0xFF);
		Out[2 * i + 1] = (uint8)((S >> 8) & 0xFF);
	}
}

void UO3DSTestLibDCClientComponent::SendFakeOpusRtpTone(double DurationSec)
{
	// Generate raw PCM (like the sample; we’re not actually Opus-encoding)
	std::vector<uint8> PCM;
	GenerateSinePCM16(PCM, 440.0, DurationSec, 48000);

	const size_t Chunk = 960; // bytes per packet payload (demo)
	size_t Off = 0;
	int Seq = 0;
	const uint32 SSRC = 1234;

	while (Off < PCM.size() && AudioTrack && AudioTrack->isOpen())
	{
		size_t Payload = FMath::Min(Chunk, PCM.size() - Off);
		std::vector<std::byte> RTP(12 + Payload);

		RTP[0] = std::byte(0x80); // V=2
		RTP[1] = std::byte(111);  // PT=111 (Opus)
		RTP[2] = std::byte((Seq >> 8) & 0xFF);
		RTP[3] = std::byte(Seq & 0xFF);
		uint32 TS = (uint32)(Seq * 960); // ~20ms @48k
		RTP[4] = std::byte((TS >> 24) & 0xFF);
		RTP[5] = std::byte((TS >> 16) & 0xFF);
		RTP[6] = std::byte((TS >> 8) & 0xFF);
		RTP[7] = std::byte(TS & 0xFF);
		RTP[8] = std::byte((SSRC >> 24) & 0xFF);
		RTP[9] = std::byte((SSRC >> 16) & 0xFF);
		RTP[10] = std::byte((SSRC >> 8) & 0xFF);
		RTP[11] = std::byte(SSRC & 0xFF);
		std::copy(PCM.begin() + Off, PCM.begin() + Off + Payload, reinterpret_cast<uint8*>(&RTP[12]));

		try { AudioTrack->send(RTP); }
		catch (...) { break; }

		Off += Payload;
		Seq++;
		std::this_thread::sleep_for(20ms);
	}

	UE_LOG(LogTemp, Log, TEXT("[LibDC Client] Sent RTP tone packets: seq=%d totalBytes=%d"), Seq, (int)PCM.size());
}