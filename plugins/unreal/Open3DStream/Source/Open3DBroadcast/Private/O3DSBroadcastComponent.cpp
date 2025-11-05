// Copyright (c) Open3DStream Contributors

#include "O3DSBroadcastComponent.h"
#include "Open3DBroadcast.h" // for LogO3DSBroadcast category declaration
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/Engine.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "Components/SkinnedMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/MorphTarget.h"
#include "Animation/NamedValueArray.h"
#include "O3DSBroadcastSerializer.h"
#include <limits>

#include "O3DSBroadcastTransportAdapter.h" // for transport enums
#include "IBroadcastTransport.h"
#include "Transports/O3DSTcpTransport.h"
#include "Transports/O3DSTcpServerTransport.h"
#include "Transports/O3DSUdpTransport.h"
#include "Transports/O3DSNngTransport.h"
#include "Transports/O3DSWebRtcTransport.h"
#include "O3DSBroadcastAudioCaptureComponent.h"

#include "O3DSBroadcastAudioCaptureComponent.h"
#include "AudioCaptureCore.h"
#include "IWebRTCConnector.h" // for FO3DSWebRtcConfig

// Forward declare property changed event type used in header override
#if WITH_EDITOR
#include "UObject/UnrealType.h"
#endif

// CVar to toggle verbose debug logging
static TAutoConsoleVariable<int32> CVarO3DSBroadcastDebugPose(
 TEXT("o3ds.Broadcast.DebugPose"),
0,
 TEXT("Enable per-frame pose debug logging for Open3DBroadcast (0/1)."),
 ECVF_Default);

// CVar to toggle curve debug logging
static TAutoConsoleVariable<int32> CVarO3DSBroadcastDebugCurves(
 TEXT("o3ds.Broadcast.DebugCurves"),
0,
 TEXT("Enable per-frame curve debug logging for Open3DBroadcast (0/1)."),
 ECVF_Default);

// CVar to show on-screen (viewport) notifications for state changes
static TAutoConsoleVariable<int32> CVarO3DSBroadcastOnScreen(
 TEXT("o3ds.Broadcast.OnScreen"),
0,
 TEXT("Show on-screen notifications for Open3DBroadcast state changes (0/1)."),
 ECVF_Default);

// New: Debug send path (serializer -> queue -> transport)
static TAutoConsoleVariable<int32> CVarO3DSBroadcastDebugSend(
 TEXT("o3ds.Broadcast.DebugSend"),
0,
 TEXT("Debug the send pipeline: queueing and draining (0/1)."),
 ECVF_Default);

// Optional debug tone controls for WebRTC connectivity tests
static TAutoConsoleVariable<int32> CVarO3DSBroadcastWebRTCDebugTone(
 TEXT("o3ds.Broadcast.WebRTC.DebugTone"),
 0,
 TEXT("When 1, request the connector to send a short debug tone on the audio track."),
 ECVF_Default);
static TAutoConsoleVariable<float> CVarO3DSBroadcastWebRTCToneHz(
 TEXT("o3ds.Broadcast.WebRTC.ToneHz"),
 440.0f,
 TEXT("Debug tone frequency in Hz (when DebugTone=1)."),
 ECVF_Default);
static TAutoConsoleVariable<float> CVarO3DSBroadcastWebRTCToneDur(
 TEXT("o3ds.Broadcast.WebRTC.ToneDur"),
 1.0f,
 TEXT("Debug tone duration in seconds (when DebugTone=1)."),
 ECVF_Default);

// Helpers to inject URL params matching adapter behavior
static FString O3DS_EnsureWebRtcRoleInUrl(const FString& InUrl, EO3DSTransportKind Kind, EO3DSWebRtcMode WebRtcMode, bool bUseLegacy)
{
 FString Out = InUrl;
 if (bUseLegacy)
 {
 if (Kind != EO3DSTransportKind::WebRTCClient && Kind != EO3DSTransportKind::WebRTCServer)
 {
 return Out; // leave as-is
 }
 if (!Out.Contains(TEXT("role="), ESearchCase::IgnoreCase))
 {
 const TCHAR* RoleStr = (Kind == EO3DSTransportKind::WebRTCClient) ? TEXT("publisher") : TEXT("subscriber");
 Out += Out.Contains(TEXT("?")) ? FString::Printf(TEXT("&role=%s"), RoleStr)
 : FString::Printf(TEXT("?role=%s"), RoleStr);
 }
 return Out;
 }
 // Family+mode path
 if (!Out.Contains(TEXT("role="), ESearchCase::IgnoreCase))
 {
 const TCHAR* RoleStr = (WebRtcMode == EO3DSWebRtcMode::Client) ? TEXT("publisher") : TEXT("subscriber");
 Out += Out.Contains(TEXT("?")) ? FString::Printf(TEXT("&role=%s"), RoleStr)
 : FString::Printf(TEXT("?role=%s"), RoleStr);
 }
 return Out;
}

#include "O3DSHelpers.h" // Shared helpers (sanitize/pattern/url)

// Minimal URL-encode for query values we generate (spaces and parentheses are common in device names)
static FString O3DS_MinimalUrlEncode(const FString& In)
{
 FString Out; Out.Reserve(In.Len());
 for (TCHAR C : In)
 {
 switch (C)
 {
 case ' ': Out += TEXT("%20"); break;
 case '(' : Out += TEXT("%28"); break;
 case ')' : Out += TEXT("%29"); break;
 case '"': Out += TEXT("%22"); break;
 case '#': Out += TEXT("%23"); break;
 case '&': Out += TEXT("%26"); break;
 case '+': Out += TEXT("%2B"); break;
 default: Out.AppendChar(C); break;
 }
 }
 return Out;
}

// Ensure a local path id (e.g., /client) is present for WebRTC Client mode to match the
// WebRTCConnectorComponent default pattern expected by the sample signaling server.
static FString O3DS_EnsureWebRtcLocalPathId(const FString& InUrl, EO3DSWebRtcMode WebRtcMode, const FString& DefaultLocalId)
{
 if (WebRtcMode != EO3DSWebRtcMode::Client)
 {
 return InUrl;
 }

 // Split base and query to avoid appending into the query segment
 FString Base; TMap<FString,FString> Q; O3DSHelpers::UrlSplitQuery(InUrl, Base, Q);

 // Detect if Base already has a path after scheme://host:port
 int32 SchemeIdx = Base.Find(TEXT("://"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
 if (SchemeIdx == INDEX_NONE)
 {
 return InUrl; // unknown scheme; leave untouched
 }
 const int32 AfterScheme = SchemeIdx + 3;
 // Find first '/' after host:port
 const int32 FirstSlash = Base.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromStart, AfterScheme);
 FString NewBase = Base;
 if (FirstSlash == INDEX_NONE)
 {
 NewBase = Base + TEXT("/") + DefaultLocalId;
 }

 // Recompose with original query (if any)
 if (Q.Num() == 0)
 {
 return NewBase;
 }
 // Keep original query ordering by reusing the substring from original InUrl
 const int32 QMark = InUrl.Find(TEXT("?"));
 return (QMark != INDEX_NONE) ? (NewBase + InUrl.Mid(QMark)) : NewBase;
}

// Missing earlier. Inject mode parameters to URL based on selected family.
static FString O3DS_InjectModeIntoUrl(const FString& InUrl, EO3DSTransportFamily Family, EO3DSNngMode NngMode, EO3DSWebRtcMode WebRtcMode)
{
 FString Out = InUrl;
 if (Family == EO3DSTransportFamily::NNG)
 {
 if (!Out.Contains(TEXT("mode="), ESearchCase::IgnoreCase))
 {
 FString ModeParam;
 switch (NngMode)
 {
 case EO3DSNngMode::Publisher: ModeParam = TEXT("mode=pub"); break;
 case EO3DSNngMode::PairClient: ModeParam = TEXT("mode=pair&role=client"); break;
 case EO3DSNngMode::PairServer: ModeParam = TEXT("mode=pair&role=server"); break;
 case EO3DSNngMode::Push: ModeParam = TEXT("mode=push"); break;
 default: break;
 }
 if (!ModeParam.IsEmpty())
 {
 Out += Out.Contains(TEXT("?")) ? TEXT("&") + ModeParam : TEXT("?") + ModeParam;
 }
 }
 }
 else if (Family == EO3DSTransportFamily::WebRTC)
 {
 // Ensure role is present
 Out = O3DS_EnsureWebRtcRoleInUrl(Out, EO3DSTransportKind::Disabled, WebRtcMode, /*bUseLegacy=*/false);
 }
 return Out;
}

// Derive a legacy-like protocol string to pass to transports
static FString O3DS_GetProtocolNameLegacy(EO3DSTransportFamily Family, EO3DSTcpMode TcpMode, EO3DSWebRtcMode WebRtcMode)
{
 switch (Family)
 {
 case EO3DSTransportFamily::TCP:
 return (TcpMode == EO3DSTcpMode::Server)
 ? UEnum::GetValueAsString(EO3DSTransportKind::TCPServer)
 : UEnum::GetValueAsString(EO3DSTransportKind::TCP);
 case EO3DSTransportFamily::UDP:
 return UEnum::GetValueAsString(EO3DSTransportKind::UDP);
 case EO3DSTransportFamily::NNG:
 return UEnum::GetValueAsString(EO3DSTransportKind::NNG);
 case EO3DSTransportFamily::WebRTC:
 return (WebRtcMode == EO3DSWebRtcMode::Client)
 ? UEnum::GetValueAsString(EO3DSTransportKind::WebRTCClient)
 : UEnum::GetValueAsString(EO3DSTransportKind::WebRTCServer);
 default:
 break;
 }
 return TEXT("EO3DSTransportKind::Disabled");
}

void UO3DSBroadcastComponent::NotifyOnScreen(const FString& Message, const FColor& Color, float DisplayTime) const
{
 if (CVarO3DSBroadcastOnScreen.GetValueOnAnyThread() ==0)
 {
 return;
 }
 if (GEngine)
 {
 // Key -1: auto
 GEngine->AddOnScreenDebugMessage(-1, DisplayTime, Color, Message);
 }
}

UO3DSBroadcastComponent::UO3DSBroadcastComponent()
{
 PrimaryComponentTick.bCanEverTick = true; // we may drain an internal queue
 SetComponentTickEnabled(false);
}

void UO3DSBroadcastComponent::BeginPlay()
{
 Super::BeginPlay();

 if (!TargetMesh.IsValid())
 {
 // Auto-discover first skeletal mesh on owner
 if (AActor* Owner = GetOwner())
 {
 TargetMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
 }
 }

 UE_LOG(LogO3DSBroadcast, Log, TEXT("O3DS Broadcast component BeginPlay on %s"), *GetNameSafe(GetOwner()));
 NotifyOnScreen(FString::Printf(TEXT("O3DS Broadcast: BeginPlay on %s"), *GetNameSafe(GetOwner())), FColor::Cyan,2.0f);

 if (bAutoStartCapture)
 {
 StartCapture();
 }
}

void UO3DSBroadcastComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
 StopCapture();
 // Ensure serializer is cleaned up
 if (Serializer)
 {
 Serializer->Detach(this);
 delete Serializer;
 Serializer = nullptr;
 }

 TeardownInternalTransport();

 Super::EndPlay(EndPlayReason);
}

void UO3DSBroadcastComponent::CreateInternalTransport()
{
 if (!bAutoCreateTransport || InternalTransport)
 {
 return;
 }

 // Always use new TransportFamily + Mode UI. Legacy fields are ignored.
 switch (TransportFamily)
 {
 case EO3DSTransportFamily::NNG:
 InternalTransport = MakeUnique<FO3DSNngTransport>();
 break;
 case EO3DSTransportFamily::TCP:
 if (TcpMode == EO3DSTcpMode::Server)
 {
 InternalTransport = MakeUnique<FO3DSTcpServerTransport>();
 }
 else
 {
 InternalTransport = MakeUnique<FO3DSTcpTransport>();
 }
 break;
 case EO3DSTransportFamily::UDP:
 InternalTransport = MakeUnique<FO3DSUdpTransport>();
 break;
 case EO3DSTransportFamily::WebRTC:
 {
	 InternalTransport = MakeUnique<FO3DSWebRtcTransport>();
	 // Cache raw ptr for audio forwarding without RTTI
	 WebRtcTransportRaw = static_cast<FO3DSWebRtcTransport*>(InternalTransport.Get());
 break;
 }
 default:
 break;
 }
}

void UO3DSBroadcastComponent::StartInternalTransport()
{
 if (!InternalTransport)
 {
 return;
 }

 FString EffectiveUrl = Url;
 FString EffectiveKey = Key;

    // Normalize common mistakes in tcp URL formatting to reduce user error
    if (TransportFamily == EO3DSTransportFamily::TCP || TransportFamily == EO3DSTransportFamily::NNG)
    {
        const FString Normalized = O3DSHelpers::NormalizeTcpUrlHostPort(EffectiveUrl);
        if (!Normalized.Equals(EffectiveUrl))
        {
            UE_LOG(LogO3DSBroadcast, Log, TEXT("Normalized URL '%s' -> '%s'"), *EffectiveUrl, *Normalized);
            EffectiveUrl = Normalized;
        }
    }

 // Inject family/mode specific URL params for non-WebRTC families only.
 if (TransportFamily != EO3DSTransportFamily::WebRTC)
 {
	 EffectiveUrl = O3DS_InjectModeIntoUrl(EffectiveUrl, TransportFamily, NngMode, WebRtcMode);
 }

 // For WebRTC, do not modify URL path or query here; connectors assemble backend-specific needs.

 const FString ProtocolName = O3DS_GetProtocolNameLegacy(TransportFamily, TcpMode, WebRtcMode);

 // If WebRTC, prepare connector pre-start config (role, backend, audio, token, room) before starting transport
 if (TransportFamily == EO3DSTransportFamily::WebRTC && WebRtcTransportRaw)
 {
     FO3DSWebRtcConfig Cfg;
     Cfg.Backend = bWebRtcBackendIsLiveKit ? EO3DSWebRtcBackend::LiveKit : EO3DSWebRtcBackend::LibDataChannel;
     Cfg.Role = (WebRtcMode == EO3DSWebRtcMode::Client) ? EO3DSWebRtcRole::Client : EO3DSWebRtcRole::Server;
     Cfg.SignalingUrl = EffectiveUrl;
     Cfg.Room = WebRtcRoom;
     if (bWebRtcBackendIsLiveKit)
     {
         Cfg.Token = LiveKitToken;
     }
     Cfg.bVerbose = (CVarO3DSBroadcastDebugSend.GetValueOnAnyThread() != 0);
     Cfg.bEnableAudio = bEnableWebRTCAudio;
     if (Cfg.bEnableAudio)
     {
         Cfg.SampleRate = WebRTCAudioSampleRate;
         Cfg.NumChannels = WebRTCAudioNumChannels;
         Cfg.BitrateKbps = WebRTCAudioBitrateKbps;
         if (WebRTCAudioMode == EO3DSWebRTCAudioMode::Input)
         {
             Cfg.AudioDeviceName = WebRTCInputDeviceName.ToString();
         }
         else if (WebRTCSubmixToTap)
         {
             Cfg.SubmixName = WebRTCSubmixToTap->GetName();
         }
         if (CVarO3DSBroadcastWebRTCDebugTone.GetValueOnAnyThread() != 0)
         {
             Cfg.bSendDebugTone = true;
             Cfg.ToneHz = CVarO3DSBroadcastWebRTCToneHz.GetValueOnAnyThread();
             Cfg.ToneDurationSec = CVarO3DSBroadcastWebRTCToneDur.GetValueOnAnyThread();
         }
     }
     WebRtcTransportRaw->ApplyPreStartConfig(Cfg);
 }

 // Do not append room/token/backend to URL; connectors receive them via pre-start config.

 if (!InternalTransport->Start(EffectiveUrl, ProtocolName, EffectiveKey))
 {
 UE_LOG(LogO3DSBroadcast, Warning, TEXT("Built-in transport failed to start: %s %s"), *ProtocolName, *EffectiveUrl);
 InternalTransport.Reset();
 }
 else
 {
 // Helpful verbose log for LiveKit URL (token elided)
 if (TransportFamily == EO3DSTransportFamily::WebRTC && bWebRtcBackendIsLiveKit)
 {
 	FString Elided = EffectiveUrl;
 	// naive elision for token query value
 	const int32 TokIdx = Elided.Find(TEXT("token="), ESearchCase::IgnoreCase);
 	if (TokIdx != INDEX_NONE)
 	{
 		int32 End = Elided.Find(TEXT("&"), ESearchCase::IgnoreCase, ESearchDir::FromStart, TokIdx);
 		if (End == INDEX_NONE) { End = Elided.Len(); }
 		Elided = Elided.Left(TokIdx) + TEXT("token=<elided>") + Elided.Mid(End);
 	}
 	UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[LiveKit] Starting with URL: %s"), *Elided);
 }
 // Hook serializer -> queue enqueuer
 if (!SerializedFrameHandle.IsValid() && Serializer)
 {
 SerializedFrameHandle = Serializer->OnSerializedFrame.AddUObject(this, &UO3DSBroadcastComponent::OnSerializedForTransport);
 if (CVarO3DSBroadcastDebugSend->GetInt() !=0)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Send pipeline hooked: Serializer -> Queue (component %s)"), *GetNameSafe(this));
 }
 }
 SetComponentTickEnabled(true);
 UE_LOG(LogO3DSBroadcast, Log, TEXT("Built-in transport started: %s %s"), *ProtocolName, *EffectiveUrl);
 }
}

void UO3DSBroadcastComponent::TeardownInternalTransport()
{
 if (Serializer && SerializedFrameHandle.IsValid())
 {
 Serializer->OnSerializedFrame.Remove(SerializedFrameHandle);
 SerializedFrameHandle.Reset();
 }

 if (InternalTransport)
 {
 InternalTransport->Stop();
 InternalTransport.Reset();
 }
 // Clear cached raw pointer when transport goes away
 WebRtcTransportRaw = nullptr;

 // Drain queue
 FQItem Item; while (SendQueue.Dequeue(Item)) {}
 QueuedBytes.Store(0);
 SetComponentTickEnabled(false);
}

void UO3DSBroadcastComponent::OnSerializedForTransport(const FString& /*Subject*/, const TArray<uint8>& Buffer, double Timestamp)
{
 if (!InternalTransport)
 {
 return;
 }

 const uint64 NewQ = QueuedBytes.Load() + (uint64)Buffer.Num();
 const int32 LimitBytes = (MaxQueuedBytes >0) ? MaxQueuedBytes : TransportMaxQueuedBytes; // use new, fallback legacy
 if (LimitBytes >0 && NewQ > (uint64)LimitBytes)
 {
 DroppedFrames.Store(DroppedFrames.Load() +1);
 if (CVarO3DSBroadcastDebugSend->GetInt() !=0)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Send queue full, dropping frame (%d bytes), limit=%d current=%llu"), Buffer.Num(), LimitBytes, (unsigned long long)QueuedBytes.Load());
 }
 return;
 }

 FQItem Item; Item.Data = Buffer; Item.Ts = Timestamp;
 SendQueue.Enqueue(MoveTemp(Item));
 QueuedBytes.Store(NewQ);

 if (CVarO3DSBroadcastDebugSend->GetInt() !=0)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Queued %d bytes. Total queued=%llu"), Buffer.Num(), (unsigned long long)NewQ);
 }
}

void UO3DSBroadcastComponent::StartCapture()
{
 if (bIsCapturing)
 {
 return;
 }

 // Lazily create and attach serializer (M2)
 if (!Serializer)
 {
 Serializer = new FO3DSBroadcastSerializer();
 Serializer->Attach(this);
 // Relay serialized frames out of the component for dev/testing (loopback)
 Serializer->OnSerializedFrame.AddLambda([this](const FString& Subject, const TArray<uint8>& Buffer, double Timestamp)
 {
 OnSerializedFrame.Broadcast(Subject, Buffer, Timestamp);
 });
 }

 // Create optional built-in transport (but don't start it yet)
 UE_LOG(LogO3DSBroadcast, Log, TEXT("[WEBRTC SETUP 1/7] Creating internal transport"));
 CreateInternalTransport();

 // Pre-start config is applied inside StartInternalTransport
 StartInternalTransport();

 // If WebRTC audio is enabled, ensure an audio capture component is present and wire PCM sink -> WebRTC transport
 if (TransportFamily == EO3DSTransportFamily::WebRTC && bEnableWebRTCAudio)
 {
	 AActor* Owner = GetOwner();
	 if (Owner)
	 {
		 UO3DSBroadcastAudioCaptureComponent* AudioComp = Owner->FindComponentByClass<UO3DSBroadcastAudioCaptureComponent>();
		 if (!AudioComp)
		 {
			 AudioComp = NewObject<UO3DSBroadcastAudioCaptureComponent>(Owner, UO3DSBroadcastAudioCaptureComponent::StaticClass(), NAME_None, RF_Transactional);
			 if (AudioComp)
			 {
				 AudioComp->RegisterComponent();
			 }
		 }

		 if (AudioComp)
		 {
			 // Configure capture mode and targets from this component's settings
			 AudioComp->CaptureMode = (WebRTCAudioMode == EO3DSWebRTCAudioMode::Input) ? EO3DSCaptureMode::Input : EO3DSCaptureMode::Mix;
			 AudioComp->Config.SampleRate = WebRTCAudioSampleRate;
			 AudioComp->Config.NumChannels = WebRTCAudioNumChannels;
			 AudioComp->Config.BitrateKbps = WebRTCAudioBitrateKbps;
			 if (AudioComp->CaptureMode == EO3DSCaptureMode::Mix)
			 {
				 AudioComp->Config.SubmixToTap = WebRTCSubmixToTap;
			 }
			 else
			 {
				 AudioComp->InputDeviceName = WebRTCInputDeviceName;
			 }

			 // Provide a friendly label and the PCM sink that forwards to WebRTC transport
			 const FString Label = TEXT("o3ds-webrtc");
			 AudioComp->SetStreamLabel(Label);

			 // Weak owner pointer for safety; transport raw cleared on teardown
			 TWeakObjectPtr<UO3DSBroadcastComponent> WeakThis(this);
			 const int32 TargetSR = WebRTCAudioSampleRate;
			 const int32 TargetCh = WebRTCAudioNumChannels;
			 AudioComp->SetAudioSink([WeakThis, TargetSR, TargetCh](const FString& /*Label*/, const float* InInterleaved, int32 InFrames, int32 InCh, int32 InSR, double /*Ts*/)
			 {
				 UO3DSBroadcastComponent* Self = WeakThis.Get();
				 if (!Self || !Self->WebRtcTransportRaw || !InInterleaved || InFrames <= 0 || InCh <= 0 || InSR <= 0)
				 {
					 return false;
				 }

				 // Resample and channel-convert into interleaved int16
				 const int32 OutSR = TargetSR;
				 const int32 OutCh = TargetCh;
				 const double Ratio = (double)OutSR / (double)InSR;
				 const int32 OutFrames = (int32)FMath::Max(1.0, FMath::RoundToDouble(InFrames * Ratio));
				 TArray<int16> Out;
				 Out.SetNumUninitialized(OutFrames * OutCh);

				 auto SampleAt = [&](int32 frame, int32 ch)->float
				 {
					 // Linear interpolation in time on a per-channel basis
					 const double srcPos = (double)frame / Ratio; // map output frame -> source position
					 const int32 i0 = FMath::Clamp((int32)srcPos, 0, InFrames - 1);
					 const int32 i1 = FMath::Clamp(i0 + 1, 0, InFrames - 1);
					 const float frac = (float)(srcPos - (double)i0);
					 const int32 idx0 = i0 * InCh;
					 const int32 idx1 = i1 * InCh;
					 const int32 c0 = FMath::Clamp(ch, 0, InCh - 1);
					 const float s0 = InInterleaved[idx0 + c0];
					 const float s1 = InInterleaved[idx1 + c0];
					 return s0 + (s1 - s0) * frac;
				 };

				 for (int32 of = 0; of < OutFrames; ++of)
				 {
					 if (OutCh == InCh)
					 {
						 for (int32 c = 0; c < OutCh; ++c)
						 {
							 const float f = SampleAt(of, c);
							 const int32 s = FMath::Clamp((int32)FMath::RoundToInt(f * 32767.0f), -32768, 32767);
							 Out[of * OutCh + c] = (int16)s;
						 }
					 }
					 else if (OutCh == 1)
					 {
						 // Downmix: average all input channels
						 float acc = 0.0f;
						 for (int32 c = 0; c < InCh; ++c) { acc += SampleAt(of, c); }
						 const float f = acc / (float)InCh;
						 const int32 s = FMath::Clamp((int32)FMath::RoundToInt(f * 32767.0f), -32768, 32767);
						 Out[of] = (int16)s;
					 }
					 else // OutCh > 1 and possibly InCh == 1; duplicate or truncate as needed
					 {
						 for (int32 c = 0; c < OutCh; ++c)
						 {
							 const int32 srcC = (InCh == 1) ? 0 : FMath::Min(c, InCh - 1);
							 const float f = SampleAt(of, srcC);
							 const int32 s = FMath::Clamp((int32)FMath::RoundToInt(f * 32767.0f), -32768, 32767);
							 Out[of * OutCh + c] = (int16)s;
						 }
					 }
				 }

				 return Self->WebRtcTransportRaw->SendAudioPcm16(Out.GetData(), Out.Num(), OutSR, OutCh);
			 });
			 
			 // Start capture with the configured mode (tears down and restarts if already running from BeginPlay)
			 EO3DSCaptureMode DesiredMode = (WebRTCAudioMode == EO3DSWebRTCAudioMode::Input) ? EO3DSCaptureMode::Input : EO3DSCaptureMode::Mix;
			 AudioComp->StartCaptureWithMode(DesiredMode);
		 }
	 }
 }

 BindToTarget();
 bIsCapturing = TargetMesh.IsValid();
 LastCaptureTime =0.0;
 FrameCounter =0;
 if (bIsCapturing)
 {
 UE_LOG(LogO3DSBroadcast, Log, TEXT("O3DS Broadcast: Started capture on %s"), *GetNameSafe(TargetMesh.Get()));
 NotifyOnScreen(FString::Printf(TEXT("O3DS Broadcast: Started on %s"), *GetNameSafe(TargetMesh.Get())), FColor::Green,2.0f);
 }
}

void UO3DSBroadcastComponent::StopCapture()
{
 if (!bIsCapturing)
 {
 return;
 }
 UnbindFromTarget();
 bIsCapturing = false;

 // Detach serializer (keep allocated for now; freed on EndPlay)
 if (Serializer)
 {
 Serializer->Detach(this);
 }

 // Teardown transport if we created one
 TeardownInternalTransport();

 UE_LOG(LogO3DSBroadcast, Log, TEXT("O3DS Broadcast: Stopped capture on %s"), *GetNameSafe(TargetMesh.Get()));
 NotifyOnScreen(FString::Printf(TEXT("O3DS Broadcast: Stopped on %s"), *GetNameSafe(TargetMesh.Get())), FColor::Yellow,2.0f);
}

void UO3DSBroadcastComponent::BindToTarget()
{
 if (!TargetMesh.IsValid())
 {
 UE_LOG(LogO3DSBroadcast, Warning, TEXT("No TargetMesh set for UO3DSBroadcastComponent on %s"), *GetNameSafe(GetOwner()));
 return;
 }
 EnsureSkeletonCache(TargetMesh.Get());
 // Bind to post-evaluation delegate so we sample after the pose is finalized
 if (USkinnedMeshComponent* Skinned = TargetMesh.Get())
 {
 if (!BoneTransformsFinalizedHandle.IsValid())
 {
 // Use virtual registration to be safe against base pointer (USkinnedMeshComponent) in UE 5.6
 BoneTransformsFinalizedHandle = Skinned->RegisterOnBoneTransformsFinalizedDelegate(
 FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateUObject(this, &UO3DSBroadcastComponent::HandleBoneTransformsFinalized));
 }
 }
 UE_LOG(LogO3DSBroadcast, Log, TEXT("O3DS Broadcast: Bound to %s"), *GetNameSafe(TargetMesh.Get()));
}

void UO3DSBroadcastComponent::UnbindFromTarget()
{
 if (USkinnedMeshComponent* Skinned = TargetMesh.Get())
 {
 if (BoneTransformsFinalizedHandle.IsValid())
 {
 Skinned->UnregisterOnBoneTransformsFinalizedDelegate(BoneTransformsFinalizedHandle);
 BoneTransformsFinalizedHandle.Reset();
 }
 UE_LOG(LogO3DSBroadcast, Log, TEXT("O3DS Broadcast: Unbound from %s"), *GetNameSafe(Skinned));
 }
}

uint64 UO3DSBroadcastComponent::ComputeDescriptorHash(const TArray<FName>& InNames, const TArray<int32>& InParents) const
{
    return O3DSHelpers::HashNamesAndParents(InNames, InParents);
}

void UO3DSBroadcastComponent::EnsureSkeletonCache(USkeletalMeshComponent* SkelComp)
{
 if (!SkelComp)
 {
 return;
 }
 USkeletalMesh* Mesh = SkelComp->GetSkeletalMeshAsset();
 USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;

 if (CachedSkeletalMesh.Get() != Mesh || CachedSkeleton.Get() != Skeleton)
 {
 RefreshSkeletonCache(SkelComp);
 // Also refresh curve caches on mesh/skeleton change
 bCurveCacheInitialized = false;
 }
}

void UO3DSBroadcastComponent::RefreshSkeletonCache(USkeletalMeshComponent* SkelComp)
{
 BoneNames.Reset();
 ParentIndices.Reset();

 USkeletalMesh* Mesh = SkelComp ? SkelComp->GetSkeletalMeshAsset() : nullptr;
 USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
 CachedSkeletalMesh = Mesh;
 CachedSkeleton = Skeleton;

 if (!Mesh)
 {
 return;
 }

 // IMPORTANT: Build descriptor from the mesh's reference skeleton, not the USkeleton asset.
 // This guarantees index order matches GetComponentSpaceTransforms() and vertex skinning.
 const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
 const int32 NumBones = RefSkel.GetNum();
 BoneNames.Reserve(NumBones);
 ParentIndices.Reserve(NumBones);

 for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
 {
 BoneNames.Add(RefSkel.GetBoneName(BoneIndex));
 ParentIndices.Add(RefSkel.GetParentIndex(BoneIndex));
 }

 // Rebuild descriptor cache and mark dirty if changed
 uint64 NewHash = ComputeDescriptorHash(BoneNames, ParentIndices);
 bool bChanged = (!DescriptorCache.IsValid()) || (DescriptorCache.BoneNames.Num() != BoneNames.Num()) || (DescriptorCache.Hash != NewHash);

 DescriptorCache.BoneNames = BoneNames;
 DescriptorCache.ParentIndices = ParentIndices;
 DescriptorCache.Hash = NewHash;
 bDescriptorDirty = bChanged;

 const bool bDebug = (CVarO3DSBroadcastDebugPose.GetValueOnAnyThread() != 0);
 if (bDebug)
 {
 UE_LOG(LogO3DSBroadcast, Log, TEXT("Cached skeleton for %s: %d bones, Hash=0x%llx%s"),
 *GetNameSafe(SkelComp), NumBones, (unsigned long long)DescriptorCache.Hash, bDescriptorDirty ? TEXT(" [Changed]") : TEXT(""));
 }

 // Emit descriptor if updated
 if (bDescriptorDirty)
 {
 const FString Subject = BuildSubjectName(SkelComp);
 OnDescriptorReady.Broadcast(Subject, DescriptorCache);
 UE_LOG(LogO3DSBroadcast, Log, TEXT("[O3DS] Descriptor emitted for %s (%d bones)"), *Subject, NumBones);
 }
}

FString UO3DSBroadcastComponent::BuildSubjectName(const USkeletalMeshComponent* SkelComp) const
{
 // If user provided an explicit subject name, prefer it.
 if (!SubjectName.IsEmpty())
 {
 return SanitizeSubjectName(SubjectName);
 }

 const UWorld* World = SkelComp ? SkelComp->GetWorld() : nullptr;
 const FString WorldName = World ? World->GetName() : TEXT("World");
 const FString ActorName = SkelComp && SkelComp->GetOwner() ? SkelComp->GetOwner()->GetName() : TEXT("Actor");
 const FString CompName = SkelComp ? SkelComp->GetName() : TEXT("SkeletalMeshComponent");
 const FString Raw = FString::Printf(TEXT("%s/%s/%s"), *WorldName, *ActorName, *CompName);
 return SanitizeSubjectName(Raw);
}

FString UO3DSBroadcastComponent::SanitizeSubjectName(const FString& Raw) const
{
    return O3DSHelpers::SanitizeSubjectName(Raw);
}

void UO3DSBroadcastComponent::EnsureCurveCache(USkeletalMeshComponent* SkelComp)
{
 if (!bCurveCacheInitialized)
 {
 RefreshCurveCache(SkelComp);
 }
}

void UO3DSBroadcastComponent::RefreshCurveCache(USkeletalMeshComponent* SkelComp)
{
 CurveNames.Reset();
 CurveValues.Reset();
 LastSentCurveValues.Reset();
 LastSentHasValue.Reset();
 MorphNameSet.Reset();
 CurveNameSet.Reset();

 if (!SkelComp)
 {
 bCurveCacheInitialized = true;
 return;
 }

 USkeletalMesh* SkelMesh = SkelComp->GetSkeletalMeshAsset();
 USkeleton* Skeleton = SkelMesh ? SkelMesh->GetSkeleton() : nullptr;

 // 1) Morph targets from skeletal mesh (also tracked for clamp-to-unit)
 if (SkelMesh)
 {
 const TArray<UMorphTarget*>& Morphs = SkelMesh->GetMorphTargets();
 CurveNames.Reserve(CurveNames.Num() + Morphs.Num());
 for (UMorphTarget* MT : Morphs)
 {
 if (!MT) continue;
 const FName Name = MT->GetFName();
 if (!CurveNameSet.Contains(Name))
 {
 MorphNameSet.Add(Name);
 CurveNames.Add(Name);
 CurveNameSet.Add(Name);
 }
 }
 }

 // 2) All named animation curves declared on the skeleton (attribute/material/morph metadata)
 if (Skeleton)
 {
 TArray<FName> SkeletonCurveNames;
 Skeleton->GetCurveMetaDataNames(SkeletonCurveNames);

 CurveNames.Reserve(CurveNames.Num() + SkeletonCurveNames.Num());
 for (const FName& N : SkeletonCurveNames)
 {
 if (N != NAME_None && !CurveNameSet.Contains(N))
 {
 CurveNames.Add(N);
 CurveNameSet.Add(N);
 }
 }
 }

 // Optional: Apply include/exclude name patterns to the descriptor itself so index set is stable and predictable.
 if (IncludeCurvePatterns.Num() > 0 || ExcludeCurvePatterns.Num() > 0)
 {
 TArray<FName> Filtered;
 Filtered.Reserve(CurveNames.Num());
 for (const FName& N : CurveNames)
 {
 if (IsCurveAllowedByPatterns(N))
 {
 Filtered.Add(N);
 }
 }
 CurveNames = MoveTemp(Filtered);
 // Rebuild set after filtering
 CurveNameSet.Reset();
 for (const FName& N : CurveNames)
 {
 CurveNameSet.Add(N);
 }
 }

 // Deterministic ordering: stable-sort lexicographically (case-sensitive)
 CurveNames.Sort([](const FName& A, const FName& B)
 {
 return FCString::Strcmp(*A.ToString(), *B.ToString()) < 0;
 });

 CurveValues.SetNumZeroed(CurveNames.Num());
 LastSentCurveValues.SetNumZeroed(CurveNames.Num());
 LastSentHasValue.SetNumZeroed(CurveNames.Num());
 bCurveCacheInitialized = true;
}

void UO3DSBroadcastComponent::CaptureCurves(USkeletalMeshComponent* SkelComp)
{
 EnsureCurveCache(SkelComp);
 if (!SkelComp)
 {
 return;
 }

 const bool bDebugCurves = (CVarO3DSBroadcastDebugCurves.GetValueOnAnyThread() != 0);

 // Initialize to 0.0
 for (int32 i = 0; i < CurveNames.Num(); ++i)
 {
 CurveValues[i] = 0.0f;
 }

 // Snapshot of component-level morph overrides so we can detect if an override exists (even if its value is 0)
 const TMap<FName, float>& MorphOverrides = SkelComp->GetMorphTargetCurves();

 // Fill values strictly from the component (predictable, post-eval state). No AnimInstance fallback.
 for (int32 i = 0; i < CurveNames.Num(); ++i)
 {
 const FName& Name = CurveNames[i];
 float V = 0.0f;

 // Prefer explicit morph overrides stored on the component
 if (MorphNameSet.Contains(Name))
 {
 if (const float* Override = MorphOverrides.Find(Name))
 {
 V = *Override;
 CurveValues[i] = V;
 continue;
 }
 }

 // Component's final blended curves (valid after evaluation)
 float OutVal = 0.0f;
 if (SkelComp->GetCurveValue(Name, 0.0f, OutVal))
 {
 V = OutVal;
 }
 else if (MorphNameSet.Contains(Name))
 {
 // For morphs without an explicit override map entry, query current morph value as fallback on the component
 V = SkelComp->GetMorphTarget(Name);
 }

 CurveValues[i] = V;
 }

 if (bDebugCurves)
 {
 const int32 LogCount = FMath::Min(5, CurveNames.Num());
 for (int32 i = 0; i < LogCount; ++i)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("  Curve[%d] %s = %.4f"), i, *CurveNames[i].ToString(), CurveValues[i]);
 }
 }
}

// Simple wildcard matching with '*' and '?' (case-sensitive)
bool UO3DSBroadcastComponent::NameMatchesPattern(const FString& Text, const FString& Pattern) const
{
    return O3DSHelpers::NameMatchesPattern(Text, Pattern);
}

bool UO3DSBroadcastComponent::IsCurveAllowedByPatterns(const FName& Name) const
{
 const FString S = Name.ToString();
 // Exclude has priority
 for (const FString& P : ExcludeCurvePatterns)
 {
 if (!P.IsEmpty() && NameMatchesPattern(S, P))
 {
 return false;
 }
 }
 if (IncludeCurvePatterns.Num() == 0)
 {
 return true; // no includes => allow by default
 }
 for (const FString& P : IncludeCurvePatterns)
 {
 if (!P.IsEmpty() && NameMatchesPattern(S, P))
 {
 return true;
 }
 }
 return false;
}

void UO3DSBroadcastComponent::BuildFilteredCurves(TArray<FName>& OutNames, TArray<float>& OutValues)
{
 OutNames.Reset();
 OutValues.Reset();

 OutNames.Reserve(CurveNames.Num());
 OutValues.Reserve(CurveNames.Num());

 for (int32 i = 0; i < CurveNames.Num(); ++i)
 {
 const FName& N = CurveNames[i];
 float V = CurveValues[i];

 // Drop NaN/Inf if enabled
 if (bDropNaNAndInfinity && !FMath::IsFinite(V))
 {
 if (bLogFilteredCurves)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Dropped curve %s (NaN/Inf)"), *N.ToString());
 }
 continue;
 }

 // Clamp morphs
 if (bClampMorphCurvesToUnit && MorphNameSet.Contains(N))
 {
 V = FMath::Clamp(V, 0.0f, 1.0f);
 }

 if (bEnableCurveFiltering)
 {
 // Include/Exclude patterns
 if (!IsCurveAllowedByPatterns(N))
 {
 if (bLogFilteredCurves)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Filtered curve %s (pattern)"), *N.ToString());
 }
 continue;
 }

 // Epsilon threshold
 if (FMath::Abs(V) < CurveEpsilon)
 {
 if (bLogFilteredCurves)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Filtered curve %s (epsilon %.6f) V=%.6f"), *N.ToString(), CurveEpsilon, V);
 }
 continue;
 }

 // Delta threshold
 const bool bHasLast = LastSentHasValue.IsValidIndex(i) ? (LastSentHasValue[i] != 0) : false;
 if (bHasLast)
 {
 const float Last = LastSentCurveValues[i];
 if (FMath::Abs(V - Last) < CurveDeltaThreshold)
 {
 if (bLogFilteredCurves)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Filtered curve %s (delta %.6f < %.6f) V=%.6f Last=%.6f"), *N.ToString(), FMath::Abs(V-Last), CurveDeltaThreshold, V, Last);
 }
 continue;
 }
 }
 }

 // Include in output
 OutNames.Add(N);
 OutValues.Add(V);
 if (LastSentCurveValues.IsValidIndex(i))
 {
 LastSentCurveValues[i] = V;
 LastSentHasValue[i] = 1;
 }
 }
}

void UO3DSBroadcastComponent::HandleBoneTransformsFinalized()
{
 if (!bIsCapturing)
 {
 return;
 }

 USkeletalMeshComponent* SkelComp = TargetMesh.Get();
 if (!SkelComp)
 {
 return;
 }

 EnsureSkeletonCache(SkelComp);

 // Optional rate limiting
 if (CaptureRateHz > 0.0f)
 {
 const double Now = FPlatformTime::Seconds();
 const double MinDelta = 1.0 / FMath::Max(1.0f, CaptureRateHz);
 if ((Now - LastCaptureTime) < MinDelta)
 {
 return;
 }
 LastCaptureTime = Now;
 }

 const TArray<FTransform>& CompSpace = SkelComp->GetComponentSpaceTransforms();
 const int32 Count = CompSpace.Num();

 // Do not rebuild descriptor based on CompSpace count; this varies with LOD/RequiredBones.
 // Just clamp to the min to avoid out-of-bounds.

 const int32 N = FMath::Min(Count, BoneNames.Num());
 const FString Subject = BuildSubjectName(SkelComp);

 const bool bDebug = (CVarO3DSBroadcastDebugPose.GetValueOnAnyThread() != 0);
 if (bDebug)
 {
 if (Count != BoneNames.Num())
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("[O3DS] CompSpace count (%d) differs from RefSkeleton bones (%d); clamping."), Count, BoneNames.Num());
 }
 UE_LOG(LogO3DSBroadcast, Log, TEXT("[O3DS] Pose #%llu Subject=%s Bones=%d"), (unsigned long long)(FrameCounter + 1ull), *Subject, N);
 }

 // Optional fix: compute effective parent index in the current component-space order
 auto GetEffectiveParentIndex = [&](int32 BoneIdx) -> int32
 {
 // Fast path: cached ref-skeleton parent that is valid for current CompSpace
 if (BoneIdx >= 0 && BoneIdx < ParentIndices.Num())
 {
 const int32 P = ParentIndices[BoneIdx];
 if (P >= 0 && P < Count)
 {
 return P;
 }
 }

 // Fallback: use names to resolve a parent index that exists in current CompSpace
 const FName BoneName = (BoneIdx >= 0 && BoneIdx < BoneNames.Num()) ? BoneNames[BoneIdx] : NAME_None;
 if (BoneName == NAME_None)
 {
 return INDEX_NONE;
 }

 const FName ParentName = SkelComp->GetParentBone(BoneName);
 if (ParentName == NAME_None)
 {
 return INDEX_NONE;
 }

 const int32 ParentByName = SkelComp->GetBoneIndex(ParentName);
 return (ParentByName >= 0 && ParentByName < Count) ? ParentByName : INDEX_NONE;
 };

 // Build pose frame
 FO3DSPoseFrame Frame;
 Frame.Subject = Subject;
 Frame.FrameIndex = ++FrameCounter;
 Frame.BoneLocalTransforms.SetNumUninitialized(N);

 for (int32 i = 0; i < N; ++i)
 {
 const int32 ParentIdx = GetEffectiveParentIndex(i);
 const FTransform& ThisCS = CompSpace[i];
 FTransform Rel;
 if (ParentIdx >= 0 && ParentIdx < CompSpace.Num())
 {
 Rel = ThisCS.GetRelativeTransform(CompSpace[ParentIdx]);
 }
 else
 {
 Rel = ThisCS; // root relative to component origin
 }

 // Normalize quaternion to keep stable rotations
 FQuat Q = Rel.GetRotation();
 Q.Normalize();
 Rel.SetRotation(Q);

 Frame.BoneLocalTransforms[i] = Rel;

 if (bDebug && i < 5)
 {
 const FVector T = Rel.GetTranslation();
 const FVector S = Rel.GetScale3D();
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("  [%d] %s p=%d T(%.2f,%.2f,%.2f) Q(%.3f,%.3f,%.3f,%.3f) S(%.2f,%.2f,%.2f)"),
 i, *BoneNames[i].ToString(), ParentIdx, T.X, T.Y, T.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
 }
 }

 // Capture curves after pose
 CaptureCurves(SkelComp);

 // Build normalized + filtered curve lists
 TArray<FName> FilteredCurveNames;
 TArray<float> FilteredCurveValues;
 BuildFilteredCurves(FilteredCurveNames, FilteredCurveValues);

 Frame.CurveNames = MoveTemp(FilteredCurveNames);
 Frame.CurveValues = MoveTemp(FilteredCurveValues);

 // If descriptor was marked dirty earlier (e.g., skeleton changed), emit it before first frame after change
 if (bDescriptorDirty && DescriptorCache.IsValid())
 {
 OnDescriptorReady.Broadcast(Subject, DescriptorCache);
 UE_LOG(LogO3DSBroadcast, Log, TEXT("[O3DS] Descriptor re-sent for %s"), *Subject);
 bDescriptorDirty = false;
 }

 // Emit frame
 OnPoseFrameReady.Broadcast(Subject, Frame);
}

void UO3DSBroadcastComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
 Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

 // Drain internal transport queue if enabled
 if (InternalTransport)
 {
 const bool bTransportReady = InternalTransport->IsConnected();
 // Allow transport (e.g., WebRTC) to pump its event loop regardless of connection state
 InternalTransport->Tick(DeltaTime);

 if (!bTransportReady)
 {
 // Don't dequeue until transport is ready/open to avoid dropping frames (e.g., descriptor before WebRTC opens)
 if (CVarO3DSBroadcastDebugSend->GetInt() != 0)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Transport not ready; deferring queue drain (queued=%llu)"), (unsigned long long)QueuedBytes.Load());
 }
 return;
 }

 constexpr int32 MaxPerTick = 32;
 int32 Count = 0;
 while (Count < MaxPerTick)
 {
 FQItem Item;
 if (!SendQueue.Dequeue(Item))
 {
 break;
 }
 QueuedBytes.Store(QueuedBytes.Load() - (uint64)Item.Data.Num());
 if (CVarO3DSBroadcastDebugSend->GetInt() != 0)
 {
 UE_LOG(LogO3DSBroadcast, Verbose, TEXT("Dequeuing %d bytes to transport"), Item.Data.Num());
 }
 InternalTransport->Send(Item.Data.GetData(), Item.Data.Num(), Item.Ts);
 ++Count;
 }
 }
}

void UO3DSBroadcastComponent::UpdateEditConditionHelpers()
{
 bTransportFamilyIsNNG = (TransportFamily == EO3DSTransportFamily::NNG);
 bTransportFamilyIsTCP = (TransportFamily == EO3DSTransportFamily::TCP);
 bTransportFamilyIsWebRTC = (TransportFamily == EO3DSTransportFamily::WebRTC);

 // Backend flag driven by selection
 bWebRtcBackendIsLiveKit = (WebRtcBackend == EO3DSWebRtcBackendSender::LiveKit);

 // Presentation flags depend on auto-create and family/backend/audio
 const bool bAuto = bAutoCreateTransport;
 bShowNngProps = bAuto && bTransportFamilyIsNNG;
 bShowTcpProps = bAuto && bTransportFamilyIsTCP;
 bShowWebRtcProps = bAuto && bTransportFamilyIsWebRTC;
 bShowWebRtcAudioProps = bShowWebRtcProps && bEnableWebRTCAudio;
 // Expose LiveKit-specific fields whenever WebRTC+LiveKit is selected, regardless of auto-transport
 bShowLiveKitProps = bTransportFamilyIsWebRTC && bWebRtcBackendIsLiveKit;
}

void UO3DSBroadcastComponent::PostInitProperties()
{
 Super::PostInitProperties();
 UpdateEditConditionHelpers();
}

#if WITH_EDITOR
void UO3DSBroadcastComponent::PostLoad()
{
 Super::PostLoad();
 UpdateEditConditionHelpers();
}
#endif

void UO3DSBroadcastComponent::OnRegister()
{
 Super::OnRegister();
 UpdateEditConditionHelpers();
}

#if WITH_EDITOR
void UO3DSBroadcastComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
 Super::PostEditChangeProperty(PropertyChangedEvent);
 // Update helper flags so EditCondition expressions referencing them remain valid
 UpdateEditConditionHelpers();

 const FName Prop = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
 // Properties that should force a full restart of the WebRTC connection (audio or animation config changes)
 static const TSet<FName> RestartProps = {
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, bEnableWebRTCAudio),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, WebRTCAudioMode),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, WebRTCInputDeviceName),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, WebRTCSubmixToTap),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, WebRTCAudioSampleRate),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, WebRTCAudioNumChannels),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, WebRTCAudioBitrateKbps),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, WebRTCAudioPlayoutDelayMs),
        GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, WebRtcBackend),
        GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, LiveKitToken),
		// Animation/pose serialization related
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, bClampMorphCurvesToUnit),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, bDropNaNAndInfinity),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, bEnableCurveFiltering),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, CurveEpsilon),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, CurveDeltaThreshold),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, IncludeCurvePatterns),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, ExcludeCurvePatterns),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, SubjectName),
		GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, TargetMesh)
	};

	if (RestartProps.Contains(Prop))
	{
		const bool bWasCapturing = bIsCapturing;
		StopCapture();
		if (bAutoStartCapture || bWasCapturing)
		{
			StartCapture();
		}
	}
}
#endif

TArray<FName> UO3DSBroadcastComponent::GetAvailableInputDeviceOptions() const
{
	TArray<FName> Options;
	Audio::FAudioCapture Temp;
	TArray<Audio::FCaptureDeviceInfo> Devices;
	if (Temp.GetCaptureDevicesAvailable(Devices) >0)
	{
		for (const auto& D : Devices)
		{
			Options.Add(FName(*D.DeviceName));
		}
	}
	return Options;
}
