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
#include "AudioCaptureCore.h"

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
1,
 TEXT("Debug the send pipeline: queueing and draining (0/1)."),
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
 const TCHAR* RoleStr = (Kind == EO3DSTransportKind::WebRTCClient) ? TEXT("client") : TEXT("server");
 Out += Out.Contains(TEXT("?")) ? FString::Printf(TEXT("&role=%s"), RoleStr)
 : FString::Printf(TEXT("?role=%s"), RoleStr);
 }
 return Out;
 }
 // Family+mode path
 if (!Out.Contains(TEXT("role="), ESearchCase::IgnoreCase))
 {
 const TCHAR* RoleStr = (WebRtcMode == EO3DSWebRtcMode::Client) ? TEXT("client") : TEXT("server");
 Out += Out.Contains(TEXT("?")) ? FString::Printf(TEXT("&role=%s"), RoleStr)
 : FString::Printf(TEXT("?role=%s"), RoleStr);
 }
 return Out;
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
 // For WebRTC: prepare channel/connector now so audio can be configured before Start()
 FO3DSWebRtcTransport* Wrtc = static_cast<FO3DSWebRtcTransport*>(InternalTransport.Get());
 if (Wrtc && !Wrtc->PrepareChannel())
 {
 UE_LOG(LogO3DSBroadcast, Error, TEXT("Failed to prepare WebRTC channel"));
 InternalTransport.Reset();
 }
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

 // Inject family/mode specific URL params
 EffectiveUrl = O3DS_InjectModeIntoUrl(EffectiveUrl, TransportFamily, NngMode, WebRtcMode);

 const FString ProtocolName = O3DS_GetProtocolNameLegacy(TransportFamily, TcpMode, WebRtcMode);

 // If WebRTC and audio enabled, push config into transport (stub for now)
 if (TransportFamily == EO3DSTransportFamily::WebRTC && bEnableWebRTCAudio)
 {
 if (FO3DSWebRtcTransport* Wrtc = static_cast<FO3DSWebRtcTransport*>(InternalTransport.Get()))
 {
 FO3DSWebRTCAudioConfig AudioCfg;
 AudioCfg.bEnable = true;
 AudioCfg.DeviceHint = WebRTCInputDeviceName.ToString();
 AudioCfg.SampleRate = WebRTCAudioSampleRate;
 AudioCfg.NumChannels = WebRTCAudioNumChannels;
 AudioCfg.BitrateKbps = WebRTCAudioBitrateKbps;
 AudioCfg.PlayoutDelayMs = WebRTCAudioPlayoutDelayMs;
 Wrtc->SetAudioConfig(AudioCfg);
 }
 }

 // Ensure WebRTC URL carries room parameter when enabled on component
 if (TransportFamily == EO3DSTransportFamily::WebRTC)
 {
 if (!EffectiveUrl.Contains(TEXT("room="), ESearchCase::IgnoreCase) && !WebRtcRoom.IsEmpty())
 {
 EffectiveUrl += EffectiveUrl.Contains(TEXT("?")) ? FString::Printf(TEXT("&room=%s"), *WebRtcRoom)
 : FString::Printf(TEXT("?room=%s"), *WebRtcRoom);
 }
 }

 if (!InternalTransport->Start(EffectiveUrl, ProtocolName, EffectiveKey))
 {
 UE_LOG(LogO3DSBroadcast, Warning, TEXT("Built-in transport failed to start: %s %s"), *ProtocolName, *EffectiveUrl);
 InternalTransport.Reset();
 }
 else
 {
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
 CreateInternalTransport();

 // For WebRTC with audio: configure audio BEFORE starting transport
 // This ensures EnableAudioSend() is called before the PeerConnection is created
 if (TransportFamily == EO3DSTransportFamily::WebRTC && bEnableWebRTCAudio)
 {
 if (AActor* Owner = GetOwner())
 {
 UO3DSBroadcastAudioCaptureComponent* AudioCap = Owner->FindComponentByClass<UO3DSBroadcastAudioCaptureComponent>();
 if (!AudioCap)
 {
 AudioCap = NewObject<UO3DSBroadcastAudioCaptureComponent>(Owner);
 if (AudioCap)
 {
 AudioCap->RegisterComponent();
 }
 }
 if (AudioCap)
 {
 // Map settings from component UX
 AudioCap->Config.SampleRate = WebRTCAudioSampleRate;
 AudioCap->Config.NumChannels = WebRTCAudioNumChannels;
 AudioCap->Config.BitrateKbps = WebRTCAudioBitrateKbps;
 AudioCap->CaptureMode = (WebRTCAudioMode == EO3DSWebRTCAudioMode::Mix) ? EO3DSCaptureMode::Mix : EO3DSCaptureMode::Input;
 AudioCap->InputDeviceName = WebRTCInputDeviceName;
 AudioCap->Config.SubmixToTap = WebRTCSubmixToTap;
 AudioCap->SubjectName = *BuildSubjectName(TargetMesh.Get());

	 // Inject connector and configure audio BEFORE transport starts
	 if (InternalTransport)
	 {
	  if (FO3DSWebRtcTransport* Wrtc = static_cast<FO3DSWebRtcTransport*>(InternalTransport.Get()))
	  {
	   TSharedPtr<IWebRTCConnector> Conn = Wrtc->GetConnector();
	   if (Conn.IsValid())
	   {
		AudioCap->SetConnector(Conn);
	   }
	  }
	 }
 }
 }
 }

 // Now start the transport (after audio is configured)
 StartInternalTransport();

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
 uint64 H = 1469598103934665603ull; // FNV-1a 64-bit offset basis
 auto Mix = [&H](const void* Data, SIZE_T Bytes)
 {
 const uint8* P = static_cast<const uint8*>(Data);
 for (SIZE_T i = 0; i < Bytes; ++i)
 {
 H ^= P[i];
 H *= 1099511628211ull; // FNV prime
 }
 };

 for (const FName& N : InNames)
 {
 const FString S = N.ToString();
 Mix(*S, S.Len() * sizeof(TCHAR));
 }
 if (InParents.Num() > 0)
 {
 Mix(InParents.GetData(), InParents.Num() * sizeof(int32));
 }
 return H;
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
 // Replace whitespace with underscore
 FString Out = Raw;
 Out = Out.Replace(TEXT(" "), TEXT("_"));

 // Remove characters not in [-._A-Za-z0-9/]
 // Build allowed set and remove others by iterating
 FString Result;
 Result.Reserve(Out.Len());
 for (int32 i = 0; i < Out.Len(); ++i)
 {
 TCHAR C = Out[i];
 bool bAllow = false;
 if ((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'))
 bAllow = true;
 else if (C == '-' || C == '.' || C == '_' || C == '/')
 bAllow = true;

 if (bAllow)
 Result.AppendChar(C);
 // else drop
 }
 return Result;
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
 auto Match = [](const TCHAR* str, const TCHAR* pat) -> bool
 {
 // iterative backtracking for '*'
 const TCHAR* s = str;
 const TCHAR* p = pat;
 const TCHAR* star = nullptr;
 const TCHAR* ss = nullptr;
 while (*s)
 {
 if (*p == '?' || *p == *s)
 {
 ++s; ++p;
 }
 else if (*p == '*')
 {
 star = p++;
 ss = s;
 }
 else if (star)
 {
 p = star + 1;
 s = ++ss;
 }
 else
 {
 return false;
 }
 }
 while (*p == '*') ++p;
 return *p == 0;
 };

 return Match(*Text, *Pattern);
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
 bWebRtcBackendIsLiveKit = (WebRtcBackend == EO3DSWebRtcBackend::LiveKit);

 // Presentation flags depend on auto-create and family/backend/audio
 const bool bAuto = bAutoCreateTransport;
 bShowNngProps = bAuto && bTransportFamilyIsNNG;
 bShowTcpProps = bAuto && bTransportFamilyIsTCP;
 bShowWebRtcProps = bAuto && bTransportFamilyIsWebRTC;
 bShowWebRtcAudioProps = bShowWebRtcProps && bEnableWebRTCAudio;
 bShowLiveKitProps = bAuto && bTransportFamilyIsWebRTC && bWebRtcBackendIsLiveKit;
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
