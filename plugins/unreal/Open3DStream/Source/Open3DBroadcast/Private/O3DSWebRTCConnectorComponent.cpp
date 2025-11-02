#include "O3DSWebRTCConnectorComponent.h"
#include "Open3DShared/Public/IWebRTCConnector.h"
#include "Open3DShared/Public/WebRTCConnectorFactory.h"
#include "Logging/LogMacros.h"

// Logging controls for example connector component receive path
static TAutoConsoleVariable<int32> CVarO3DSWebRTCExampleLogDataContent(
	TEXT("o3ds.WebRTCExample.LogDataContent"),
	0,
	TEXT("When 1, log the received DataChannel payload as text (truncated)."),
	ECVF_Default);
static TAutoConsoleVariable<int32> CVarO3DSWebRTCExampleLogDataCount(
	TEXT("o3ds.WebRTCExample.LogDataCount"),
	0,
	TEXT("When 1, log only the number of bytes received on the DataChannel."),
	ECVF_Default);

// Ensure a local path id (e.g., /client) is present for Client role when no explicit local-id path
// is provided. This mirrors the behavior needed by the sample signaling server which uses the path
// segment as the receiver identity.
static FString O3DS_EnsureClientPathIdIfMissing(const FString& InUrl, EO3DSWebRtcRole Role, const FString& DefaultLocalId)
{
	if (Role != EO3DSWebRtcRole::Client)
	{
		return InUrl;
	}
	FString Base = InUrl;
	int32 QIdx;
	if (InUrl.FindChar('?', QIdx))
	{
		Base = InUrl.Left(QIdx);
	}
	const int32 SchemeIdx = Base.Find(TEXT("://"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
	if (SchemeIdx == INDEX_NONE)
	{
		return InUrl; // unknown scheme; leave untouched
	}
	const int32 AfterScheme = SchemeIdx + 3;
	const int32 FirstSlash = Base.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromStart, AfterScheme);
	if (FirstSlash != INDEX_NONE)
	{
		return InUrl; // already has a path id
	}
	// Append path id preserving any query
	FString WithPath = Base + TEXT("/") + DefaultLocalId;
	return (QIdx != INDEX_NONE) ? (WithPath + InUrl.Mid(QIdx)) : WithPath;
}

UO3DSWebRTCConnectorComponent::UO3DSWebRTCConnectorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UO3DSWebRTCConnectorComponent::BeginPlay()
{
	Super::BeginPlay();

	FO3DSWebRtcConfig Cfg;
	Cfg.Backend = EO3DSWebRtcBackend::LibDataChannel;
	Cfg.Role = bServer ? EO3DSWebRtcRole::Server : EO3DSWebRtcRole::Client;
	Cfg.SignalingUrl = SignalingUrl;
	// If running as client and no explicit local-id path is provided, auto-append "/client"
	// to match the expected identity contract of the sample signaling server.
	if (!bAppendLocalIdToUrl || LocalId.IsEmpty())
	{
		Cfg.SignalingUrl = O3DS_EnsureClientPathIdIfMissing(Cfg.SignalingUrl, Cfg.Role, TEXT("client"));
	}
	if (bAppendLocalIdToUrl && !LocalId.IsEmpty())
	{
		if (!Cfg.SignalingUrl.EndsWith(TEXT("/")))
		{
			Cfg.SignalingUrl += TEXT("/");
		}
		Cfg.SignalingUrl += LocalId;
	}
	Cfg.Room = Room;
	Cfg.bEnableAudio = bEnableAudio;
	Cfg.bSendDebugTone = bSendDebugTone;
	Cfg.ToneHz = ToneHz;
	Cfg.ToneDurationSec = ToneDurationSec;
	Cfg.bVerbose = bVerbose;

	Connector = FWebRTCConnectorFactory::Create(Cfg.Backend);
	if (!Connector.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("WebRTCConnectorFactory returned null"));
		return;
	}

	Connector->OnState().AddUObject(this, &UO3DSWebRTCConnectorComponent::OnState);
	Connector->OnData().AddUObject(this, &UO3DSWebRTCConnectorComponent::OnData);
	Connector->OnRemoteAudioRtp().AddLambda([this](const TArray<uint8>& Bytes)
	{
		UE_LOG(LogTemp, Log, TEXT("[ExampleConnector] RTP: %d bytes"), Bytes.Num());
	});

	if (!Connector->Start(Cfg))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to start WebRTC connector"));
		Connector.Reset();
	}
}

void UO3DSWebRTCConnectorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Connector.IsValid())
	{
		Connector->Stop();
		Connector.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void UO3DSWebRTCConnectorComponent::OnState(const FString& State, bool bIsError)
{
	UE_LOG(LogTemp, Log, TEXT("[ExampleConnector] State: %s%s"), *State, bIsError ? TEXT(" (ERR)") : TEXT(""));
	if (State == TEXT("DataChannelOpen"))
	{
		const char* Msg = "hello from example component";
		Connector->Send(reinterpret_cast<const uint8*>(Msg), (int32)strlen(Msg));
	}
}

void UO3DSWebRTCConnectorComponent::OnData(const TArray<uint8>& Bytes)
{
	const bool bLogContent = (CVarO3DSWebRTCExampleLogDataContent.GetValueOnAnyThread() != 0);
	const bool bLogCount = (CVarO3DSWebRTCExampleLogDataCount.GetValueOnAnyThread() != 0);
	if (bLogContent)
	{
		// Truncate to keep logs readable
		const int32 MaxChars = 128;
		FString AsText; AsText.Reserve(FMath::Min(Bytes.Num(), MaxChars));
		const int32 N = FMath::Min(Bytes.Num(), MaxChars);
		for (int32 i = 0; i < N; ++i) { AsText.AppendChar((TCHAR)Bytes[i]); }
		if (Bytes.Num() > MaxChars) { AsText += TEXT("..."); }
		UE_LOG(LogTemp, Log, TEXT("[ExampleConnector] Data: %s (%d bytes)"), *AsText, Bytes.Num());
	}
	else if (bLogCount)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[ExampleConnector] Data: %d bytes"), Bytes.Num());
	}
}
